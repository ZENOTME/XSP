#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/skbuff.h>
#include <linux/veth.h>
#include <linux/rtnetlink.h>
#include <linux/etherdevice.h>
#include "queue.h"

#define DEVICE_NAME "emulator"
#define IOCTL_PING _IOR('N', 2, u64)

#define ENTRIES 32768
static struct xsk_queue *rx_ring1;
static struct xsk_queue *tx_ring1;
static struct xsk_queue *rx_ring2;
static struct xsk_queue *tx_ring2;
static struct net_device *veth1;
static struct net_device *veth2;

#define RX_RING_1_OFFSET 0
#define TX_RING_1_OFFSET 0x80000000
#define RX_RING_2_OFFSET 0x100000000ULL
#define TX_RING_2_OFFSET 0x180000000ULL

static int major;
static struct class *netdev_class;
static struct cdev netdev_cdev;

static inline u32 xskq_cons_read_desc_batch(struct xsk_queue *q, u32 max, int tar) {
  u32 cached_cons = q->cached_cons, nb_entries = 0;

  while (cached_cons != q->cached_prod && nb_entries < max) {
    struct xdp_umem_ring *ring = (struct xdp_umem_ring *)q->ring;
    u32 idx = cached_cons & q->ring_mask;

    // #TODO: handle msg here
    u64 msg = ring->desc[idx];
    struct sk_buff *skb = (struct sk_buff *)msg;
    if(skb!=NULL) {
      if(tar == 0) {
        // printk("to veth1\n");
        skb->dev = veth1;
      } else {
        // printk("to veth2\n");
        skb->dev = veth2;
      }
      skb_push(skb, ETH_HLEN);
      dev_queue_xmit(skb);
      // printk("dev queue xmit: %d\n",ret);
    } else {
      printk("There is a null skb\n");
    }

    nb_entries++;
    cached_cons++;
  }

  /* Release valid plus any invalid entries */
  xskq_cons_release_n(q, cached_cons - q->cached_cons);
  return nb_entries;
}

static long netdev_ioctl(struct file *file, unsigned int cmd,
                         unsigned long arg) {
  switch (cmd) {
  case IOCTL_PING:
    // printk("Enter ping\n");
    struct xsk_queue *q = NULL;
    u32 nb_pkts = 4096;
    if (arg == 0) {
      // printk("arg == 0\n");
      q = tx_ring1;
    } else {
      // printk("arg != 0\n");
      q = tx_ring2;
    }
    nb_pkts = xskq_cons_nb_entries(q, nb_pkts);
    // printk("nb_pkts: %d\n", nb_pkts);
    // printk("produce: %d\n", q->ring->producer);
    xskq_cons_read_desc_batch(q, nb_pkts, arg);
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

  if (offset == RX_RING_1_OFFSET) {
    q = rx_ring1;
  } else if (offset == TX_RING_1_OFFSET) {
    printk("TX_RING_1_OFFSET\n");
    q = tx_ring1;
  } else if (offset == RX_RING_2_OFFSET) {
    q = rx_ring2;
  } else if (offset == TX_RING_2_OFFSET) {
    printk("TX_RING_2_OFFSET\n");
    q = tx_ring2;
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
    struct sk_buff *skb = *pskb;

    if (skb->dev == veth1) {
        if(xskq_prod_reserve_addr(rx_ring1,(u64)skb)!=0) {
            // printk("veth1 full\n");
            kfree_skb(skb);
        } else {
            // printk("pass %lld to veth1\n",(u64)skb);
            xskq_prod_submit(rx_ring1);
        }
    } else if (skb->dev == veth2) {
        if(xskq_prod_reserve_addr(rx_ring2,(u64)skb)!=0) {
            // printk("veth2 full\n");
            kfree_skb(skb);
        } else {
            // printk("pass %lld to veth2\n",(u64)skb);
            xskq_prod_submit(rx_ring2);
        }
    } else {
        printk("PASS\n");
        return RX_HANDLER_PASS;
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

static int __init netdev_init(void) {
  dev_t dev;
  int ret;

  // 分配字符设备编号
  ret = alloc_chrdev_region(&dev, 0, 1, DEVICE_NAME);
  if (ret < 0) {
    printk(KERN_ERR "Failed to allocate chrdev region\n");
    return ret;
  }

  major = MAJOR(dev);

  // 创建字符设备类
  netdev_class = class_create(DEVICE_NAME);
  if (IS_ERR(netdev_class)) {
    unregister_chrdev_region(MKDEV(major, 0), 1);
    return PTR_ERR(netdev_class);
  }

  // 创建字符设备
  cdev_init(&netdev_cdev, &fops);
  netdev_cdev.owner = THIS_MODULE;
  ret = cdev_add(&netdev_cdev, MKDEV(major, 0), 1);
  if (ret) {
    class_destroy(netdev_class);
    unregister_chrdev_region(MKDEV(major, 0), 1);
    return ret;
  }

  // 创建设备节点
  device_create(netdev_class, NULL, MKDEV(major, 0), NULL, DEVICE_NAME);

  // Create queue
  rx_ring1 = xskq_create(ENTRIES,1);
  if (!rx_ring1) {
    printk(KERN_ERR "Failed to create rx_ring1\n");
    return -ENOMEM;
  }
  tx_ring1 = xskq_create(ENTRIES,2);
  if (!tx_ring1) {
    printk(KERN_ERR "Failed to create tx_ring1\n");
    return -ENOMEM;
  }
  rx_ring2 = xskq_create(ENTRIES,3);
  if (!rx_ring2) {
    printk(KERN_ERR "Failed to create rx_ring2\n");
    return -ENOMEM;
  }
  tx_ring2 = xskq_create(ENTRIES,4);
  if (!tx_ring2) {
    printk(KERN_ERR "Failed to create tx_ring2\n");
    return -ENOMEM;
  }
  printk("Create queue succesfully!");
  // Create veth
  veth1 = dev_get_by_name(&init_net, "veth1-br");
  veth2 = dev_get_by_name(&init_net, "veth2-br");
  if (!veth1 || !veth2) {
    printk(KERN_ERR "Cannot find veth interfaces\n");
    return -ENODEV;
  }
  rtnl_lock();
  netdev_rx_handler_register(veth1, veth_handle_frame, NULL);
  netdev_rx_handler_register(veth2, veth_handle_frame, NULL);
  rtnl_unlock();
  printk(KERN_INFO "veth driver loaded\n");

  printk(KERN_INFO "netdev_ioctl module loaded\n");
  return 0;
}

static void __exit netdev_exit(void) {
  device_destroy(netdev_class, MKDEV(major, 0));
  class_destroy(netdev_class);
  cdev_del(&netdev_cdev);
  unregister_chrdev_region(MKDEV(major, 0), 1);

  // Destroy queue
  if (rx_ring1)
    xskq_destroy(rx_ring1);
  if (tx_ring1)
    xskq_destroy(tx_ring1);
  if (rx_ring2)
    xskq_destroy(rx_ring2);
  if (tx_ring2)
    xskq_destroy(tx_ring2);

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
