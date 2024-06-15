#include <linux/cdev.h>
#include <linux/etherdevice.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/netpoll.h>
#include <linux/rtnetlink.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/veth.h>
#include "queue.h"

#define DEVICE_NAME "emulator"
#define IOCTL_PING _IOR('N', 2, u64)
#define IOCTL_ATTACH_IF _IOR('N', 3, u64)
#define IOCTL_PING_ALL _IOR('N', 4, u64)

struct ring_info {
  u64 rx_ring_size;
  u64 rx_ring_num;
  u64 tx_ring_size;
  u64 tx_ring_num;
};

#define ENTRIES 4096
#define CORE_NUM 28
#define MASK_32 0xFFFFFFFF
static struct xsk_queue *rx_ring1[CORE_NUM];
static struct xsk_queue *tx_ring1[CORE_NUM];
static struct xsk_queue *rx_ring2[CORE_NUM];
static struct xsk_queue *tx_ring2[CORE_NUM];
static struct ring_info dev1_info;
static struct ring_info dev2_info;
static struct net_device *veth1;
static struct net_device *veth2;

#define RX_RING_1_OFFSET 0
#define TX_RING_1_OFFSET 0x80000000
#define RX_RING_2_OFFSET 0x100000000ULL
#define TX_RING_2_OFFSET 0x180000000ULL

static int major;
static struct class *netdev_class;
static struct cdev netdev_cdev;

// pakcet recoder
static DEFINE_MUTEX(recorder_lock);
u64 packet_recoder[ENTRIES * CORE_NUM];
u64 packet_recoder_index = 0;
u64 packet_recoder_size = 0;

bool record_packet(u64 packet) {
  bool ret;
  mutex_lock(&recorder_lock);
  if (packet_recoder_size < ENTRIES * CORE_NUM) {
    u64 i = packet_recoder_index;
    do {
      if (packet_recoder[i] == 0) {
        packet_recoder[i] = packet;
        packet_recoder_size++;
        packet_recoder_index = (i == packet_recoder_index)
                                   ? packet_recoder_size + 1
                                   : packet_recoder_size;
        break;
      }
      i = (i + 1) % (ENTRIES * CORE_NUM);
    } while (i != packet_recoder_index);
    if (packet_recoder[i] != packet) {
      printk("record packet failed\n");
    }
    ret = true;
  } else {
    ret = false;
  }
  mutex_unlock(&recorder_lock);
  return ret;
}

bool search_and_remove_packet(u64 packet) {
  bool ret = false;
  mutex_lock(&recorder_lock);
  for (u64 i = 0; i < ENTRIES * CORE_NUM; i++) {
    if (packet_recoder[i] == packet) {
      packet_recoder[i] = 0;
      ret = true;
      packet_recoder_size--;
      break;
    }
  }
  mutex_unlock(&recorder_lock);
  return ret;
}

static inline u32 xskq_cons_read_desc_batch(struct xsk_queue *q, u32 max,
                                            int tar) {
  u32 cached_cons = q->cached_cons, nb_entries = 0;
  while (cached_cons != q->cached_prod && nb_entries < max) {
    struct xdp_umem_ring *ring = (struct xdp_umem_ring *)q->ring;
    u32 idx = cached_cons & q->ring_mask;
    u64 msg = ring->desc[idx];
    nb_entries++;
    cached_cons++;

    struct sk_buff *skb = (struct sk_buff *)msg;
    if (IS_ERR(skb)) {
      printk("errro packet %lld\n", (u64)skb);
      continue;
    }

    if (refcount_read(&skb->users) != 1) {
      printk("skb %lld ref %d error\n", (u64)skb, refcount_read(&skb->users));
      continue;
    }

    if (tar == 0) {
      skb->dev = veth1;
    } else {
      skb->dev = veth2;
    }
    if (netpoll_tx_running(skb->dev)) {
      printk("fail xmit: dev busy\n");
      consume_skb(skb);
      continue;
    }
    if (!is_skb_forwardable(skb->dev, skb)) {
      printk("fail forward: \n");
      consume_skb(skb);
      continue;
    }
    skb_push(skb, ETH_HLEN);
    int ret = dev_queue_xmit(skb);
    if (ret == NETDEV_TX_BUSY) {
      consume_skb(skb);
      continue;
    }
  }

  /* Release valid plus any invalid entries */
  xskq_cons_release_n(q, cached_cons - q->cached_cons);

  return nb_entries;
}

struct attach_if {
  struct ring_info dev1_info;
  struct ring_info dev2_info;
};

static long netdev_ioctl(struct file *file, unsigned int cmd,
                         unsigned long arg) {
  unsigned long flags;
  switch (cmd) {
  case IOCTL_ATTACH_IF:
    struct attach_if *if_info = (struct attach_if *)arg;
    struct ring_info *to_dev1_info = (struct ring_info *)if_info;
    struct ring_info *to_dev2_info = to_dev1_info + 1;
    int ret = copy_to_user(to_dev1_info, &dev1_info, sizeof(struct ring_info));
    if (ret) {
      printk("copy_to_user failed %d \n", ret);
      return -EFAULT;
    }
    ret = copy_to_user(to_dev2_info, &dev2_info, sizeof(struct ring_info));
    if (ret) {
      printk("copy_to_user failed %d \n", ret);
      return -EFAULT;
    }
    break;
  case IOCTL_PING:
    unsigned tar = (arg >> 32) & MASK_32;
    unsigned tx_i = (arg)&MASK_32;
    struct xsk_queue *q = NULL;
    u32 nb_pkts = 4096;
    if (tar == 0) {
      q = tx_ring1[tx_i];
    } else {
      q = tx_ring2[tx_i];
    }
    nb_pkts = xskq_cons_nb_entries(q, nb_pkts);
    xskq_cons_read_desc_batch(q, nb_pkts, tar);
    break;
  case IOCTL_PING_ALL:
    // disable preemption
    for (int i = 0; i < CORE_NUM; i++) {
      nb_pkts = xskq_cons_nb_entries(tx_ring1[i], 4096);
      xskq_cons_read_desc_batch(tx_ring1[i], nb_pkts, 0);
    }
    for (int i = 0; i < CORE_NUM; i++) {
      nb_pkts = xskq_cons_nb_entries(tx_ring2[i], 4096);
      xskq_cons_read_desc_batch(tx_ring2[i], 4096, 1);
    }
    break;

  default:
    return -EINVAL;
  }

  return 0;
}

static int netdev_open(struct inode *inode, struct file *file) { return 0; }

static int netdev_release(struct inode *inode, struct file *file) { return 0; }

static int netdev_mmap(struct file *filp, struct vm_area_struct *vma) {
  printk("start mmap\n");
  loff_t offset = (loff_t)vma->vm_pgoff << PAGE_SHIFT;
  unsigned long size = vma->vm_end - vma->vm_start;
  struct xsk_queue *q = NULL;

  if (offset >= RX_RING_1_OFFSET && offset < TX_RING_1_OFFSET) {
    int index = (offset - RX_RING_1_OFFSET) >> PAGE_SHIFT;
    printk("RX_RING1 offset: %lld shift %d map index: %d\n",
           (offset - RX_RING_1_OFFSET), PAGE_SHIFT, index);
    q = rx_ring1[index];
  } else if (offset >= TX_RING_1_OFFSET && offset < RX_RING_2_OFFSET) {
    int index = (offset - TX_RING_1_OFFSET) >> PAGE_SHIFT;
    printk("TX_RING1 offset: %lld shift %d map index: %d\n",
           (offset - TX_RING_1_OFFSET), PAGE_SHIFT, index);
    q = tx_ring1[index];
  } else if (offset >= RX_RING_2_OFFSET && offset < TX_RING_2_OFFSET) {
    int index = (offset - RX_RING_2_OFFSET) >> PAGE_SHIFT;
    printk("RX_RING2 offset: %lld shift %d map index: %d\n",
           (offset - RX_RING_2_OFFSET), PAGE_SHIFT, index);
    q = rx_ring2[index];
  } else if (offset >= TX_RING_2_OFFSET) {
    int index = (offset - TX_RING_2_OFFSET) >> PAGE_SHIFT;
    printk("TX_RING2 offset: %lld shift %d map index: %d\n",
           (offset - TX_RING_2_OFFSET), PAGE_SHIFT, index);
    q = tx_ring2[index];
  }

  if (!q) {
    return -EINVAL;
  }

  /* Matches the smp_wmb() in xsk_init_queue */
  smp_rmb();
  if (size > q->ring_vmalloc_size)
    return -EINVAL;

  int ret = remap_vmalloc_range(vma, q->ring, 0);
  printk("remap_vmalloc_range: %d\n", ret);
  return ret;
}

static rx_handler_result_t veth_handle_frame(struct sk_buff **pskb) {
  int cpu_id;
  cpu_id = smp_processor_id();

  struct sk_buff *skb = *pskb;

  if (skb->dev == veth1 || skb->dev ==veth2) {
    skb = skb_share_check(skb, GFP_ATOMIC);
    if (!skb)
      return RX_HANDLER_CONSUMED;
  } else {
    printk("PASS\n");
    return RX_HANDLER_PASS;
  }

  if (skb->dev == veth1) {
    if (xskq_prod_reserve_addr(rx_ring1[cpu_id], (u64)skb) != 0) {
      consume_skb(skb);
    } else {
      xskq_prod_submit(rx_ring1[cpu_id]);
    }
  } else if (skb->dev == veth2) {
    if (xskq_prod_reserve_addr(rx_ring2[cpu_id], (u64)skb) != 0) {
      consume_skb(skb);
    } else {
      xskq_prod_submit(rx_ring2[cpu_id]);
    }
  } else {
    printk("Never reach here");
  }

  return RX_HANDLER_CONSUMED;
}

static struct file_operations fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = netdev_ioctl,
    .open = netdev_open,
    .release = netdev_release,
    .mmap = netdev_mmap,
};

static void destory_queue(struct xsk_queue **q, int num) {
  if (q) {
    for (int i = 0; i < num; i++) {
      if (q[i]) {
        xskq_destroy(q[i]);
      }
    }
  }
}

static int __init netdev_init(void) {
  dev_t dev;
  int ret;

  // Create character device
  ret = alloc_chrdev_region(&dev, 0, 1, DEVICE_NAME);
  if (ret < 0) {
    printk(KERN_ERR "Failed to allocate chrdev region\n");
    return ret;
  }
  major = MAJOR(dev);
  netdev_class = class_create(DEVICE_NAME);
  if (IS_ERR(netdev_class)) {
    unregister_chrdev_region(MKDEV(major, 0), 1);
    return PTR_ERR(netdev_class);
  }
  cdev_init(&netdev_cdev, &fops);
  netdev_cdev.owner = THIS_MODULE;
  ret = cdev_add(&netdev_cdev, MKDEV(major, 0), 1);
  if (ret) {
    class_destroy(netdev_class);
    unregister_chrdev_region(MKDEV(major, 0), 1);
    return ret;
  }
  device_create(netdev_class, NULL, MKDEV(major, 0), NULL, DEVICE_NAME);

  // Create queue
  for (int i = 0; i < CORE_NUM; i++) {
    rx_ring1[i] = xskq_create(ENTRIES, 1);
    if (!rx_ring1[i]) {
      ret = -ENOMEM;
      printk(KERN_ERR "Failed to create rx_ring1\n");
      goto err;
    }
    tx_ring1[i] = xskq_create(ENTRIES, 2);
    if (!tx_ring1[i]) {
      ret = -ENOMEM;
      printk(KERN_ERR "Failed to create tx_ring1\n");
      goto err;
    }
    rx_ring2[i] = xskq_create(ENTRIES, 3);
    if (!rx_ring2[i]) {
      ret = -ENOMEM;
      printk(KERN_ERR "Failed to create rx_ring2\n");
      goto err;
    }
    tx_ring2[i] = xskq_create(ENTRIES, 4);
    if (!tx_ring2[i]) {
      ret = -ENOMEM;
      printk(KERN_ERR "Failed to create tx_ring2\n");
      goto err;
    }
  }

  // Register rx handler
  veth1 = dev_get_by_name(&init_net, "veth1-br");
  veth2 = dev_get_by_name(&init_net, "veth2-br");
  if (!veth1 || !veth2) {
    ret = -ENODEV;
    printk(KERN_ERR "Cannot find veth interfaces\n");
    goto err;
  }
  rtnl_lock();
  netdev_rx_handler_register(veth1, veth_handle_frame, NULL);
  netdev_rx_handler_register(veth2, veth_handle_frame, NULL);
  rtnl_unlock();

  // Set ring info
  dev1_info.tx_ring_size = 0;
  dev1_info.tx_ring_num = CORE_NUM;
  dev1_info.rx_ring_size = 0;
  dev1_info.rx_ring_num = CORE_NUM;
  dev2_info.tx_ring_size = 0;
  dev2_info.tx_ring_num = CORE_NUM;
  dev2_info.rx_ring_size = 0;
  dev2_info.rx_ring_num = CORE_NUM;
  for (int i = 0; i < CORE_NUM; i++) {
    dev1_info.rx_ring_size += rx_ring1[i]->ring_vmalloc_size;
    dev1_info.tx_ring_size += tx_ring1[i]->ring_vmalloc_size;
    dev2_info.rx_ring_size += rx_ring2[i]->ring_vmalloc_size;
    dev2_info.tx_ring_size += tx_ring2[i]->ring_vmalloc_size;
  }

  mutex_init(&recorder_lock);
  for (int i = 0; i < ENTRIES * CORE_NUM; i++) {
    packet_recoder[i] = 0;
  }

  printk(KERN_INFO "netdev_ioctl module loaded successfully\n");

  return 0;
err:
  device_destroy(netdev_class, MKDEV(major, 0));
  class_destroy(netdev_class);
  cdev_del(&netdev_cdev);
  unregister_chrdev_region(MKDEV(major, 0), 1);
  destory_queue(rx_ring1, CORE_NUM);
  destory_queue(rx_ring2, CORE_NUM);
  destory_queue(tx_ring1, CORE_NUM);
  destory_queue(tx_ring2, CORE_NUM);
  return ret;
}

static void __exit netdev_exit(void) {
  mutex_destroy(&recorder_lock);

  device_destroy(netdev_class, MKDEV(major, 0));
  class_destroy(netdev_class);
  cdev_del(&netdev_cdev);
  unregister_chrdev_region(MKDEV(major, 0), 1);

  // Destroy queue
  destory_queue(rx_ring1, CORE_NUM);
  destory_queue(rx_ring2, CORE_NUM);
  destory_queue(tx_ring1, CORE_NUM);
  destory_queue(tx_ring2, CORE_NUM);

  rtnl_lock();
  netdev_rx_handler_unregister(veth1);
  netdev_rx_handler_unregister(veth2);
  rtnl_unlock();

  if (veth1) {
    dev_put(veth1);
  }

  if (veth2) {
    dev_put(veth2);
  }

  printk(KERN_INFO "netdev_ioctl module unloaded\n");
}

module_init(netdev_init);
module_exit(netdev_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("A simple ioctl example for getting net device index");
MODULE_VERSION("1.0");
