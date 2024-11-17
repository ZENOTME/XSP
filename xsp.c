#define pr_fmt(fmt) "AF_XSP: %s: " fmt, __func__

#include "common_config.h"
#include "map.h"
#include "queue_array.h"
#include "xsp_queue.h"
#include <linux/fs.h>
#include <linux/if_ether.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/rtnetlink.h>
#include <linux/veth.h>

struct queue_array_list global_queue_array_list;
struct offset_queue_table global_offset_queue_table;
struct dev_queue_table global_dev_queue_table;

static rx_handler_result_t xsp_handle_frame(struct sk_buff **pskb) {
  struct sk_buff *skb = *pskb;
  struct queue_array *rx_queue_array = NULL;
  struct net_device *dev = skb->dev;

  // todo:
  // do we relly need this?
  if (dev_queue_table_lookup(&global_dev_queue_table, skb->dev) == NULL) {
    pr_warn("pass none found dev skb");
    return RX_HANDLER_PASS;
  }

  // Try to get rx_queue from dev->rx_handler_data.
  // If not set, return RX_HANDLER_PASS.
  void *data = rcu_dereference(dev->rx_handler_data);
  if (!data) {
    pr_warn("rx_queue_array not set in this dev, pass\n");
    return RX_HANDLER_PASS;
  }

  // Get queue using the cpu id
  rx_queue_array = (struct queue_array *)data;
  int cpu_id;
  cpu_id = smp_processor_id();
  BUG_ON(cpu_id >= CORE_NUM);
  struct xsp_queue *queue = rx_queue_array->queue[cpu_id];

  skb = skb_share_check(skb, GFP_ATOMIC);
  if (!skb) {
    pr_warn("fail to share check skb");
    return RX_HANDLER_CONSUMED;
  }

  u64 src_mac = 0;
  u64 dst_mac = 0;

  if (unlikely(!pskb_may_pull(skb, sizeof(struct ethhdr)))) {
    pr_warn("skb does not contain a complete ethernet header, pass\n");
    return RX_HANDLER_PASS;
  }
  struct ethhdr *eth = eth_hdr(skb);
  memcpy(&src_mac, eth->h_source, ETH_ALEN);
  memcpy(&dst_mac, eth->h_dest, ETH_ALEN);

  if (xspq_prod_reserve_addr(queue, (u64)skb, src_mac, dst_mac) != 0) {
    pr_warn("fail to reserve addr, drop skb");
    consume_skb(skb);
  } else {
    xspq_prod_submit(queue);
  }

  return RX_HANDLER_CONSUMED;
}

// XSP present as a character device to the user space.
static long xspdev_ioctl(struct file *file, unsigned int cmd,
                         unsigned long arg);
static int xspdev_open(struct inode *inode, struct file *file);
static int xspdev_release(struct inode *inode, struct file *file);
static int xspdev_mmap(struct file *filp, struct vm_area_struct *vma);
static int major;
static struct class *xspdev_class;
static struct cdev xspdev_cdev;
static struct file_operations xsp_fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = xspdev_ioctl,
    .open = xspdev_open,
    .release = xspdev_release,
    .mmap = xspdev_mmap,
};

int xspdev_open(struct inode *inode, struct file *file) {
  pr_info("xspdev_open\n");
  return 0;
}

static int xspdev_release(struct inode *inode, struct file *file) {
  pr_info("xspdev_release\n");
  return 0;
}

static int xspdev_mmap(struct file *filp, struct vm_area_struct *vma) {
  loff_t offset = (loff_t)vma->vm_pgoff << PAGE_SHIFT;
  unsigned long size = vma->vm_end - vma->vm_start;
  struct offset_queue_entry *entry = NULL;
  struct xsp_queue *q = NULL;

  entry = offset_queue_table_lookup(&global_offset_queue_table, offset);

  if (!entry || !entry->queue) {
    pr_err("invalid entry with offset: %llu", offset);
    return -EINVAL;
  }
  q = entry->queue;
  pr_info("offset: %llu size: %lu queue size: %lu", offset, size,
          q->ring_vmalloc_size);

  /* Matches the smp_wmb() in xsk_init_queue */
  smp_rmb();
  if (size > q->ring_vmalloc_size)
    return -EINVAL;

  int ret = remap_vmalloc_range(vma, q->addrs, 0);
  if (ret) {
    pr_err("failed to mmap with offset %lld \n", offset);
  }

  return ret;
}

static int bind_dev(void *user_info_addr) {
  struct bind_dev_info info;
  int ret;
  if (copy_from_user(&info, (struct bind_dev_info *)user_info_addr,
                     sizeof(info))) {
    pr_err("copy_from_user failed\n");
    return -EFAULT;
  }
  // Get the device by name
  struct net_device *dev = dev_get_by_name(&init_net, info.dev_name);
  if (!dev) {
    pr_err("Device not found by name: %s\n", info.dev_name);
    return -ENODEV;
  }

  // Check if the device is already binded in dev queue table
  if (dev_queue_table_lookup(&global_dev_queue_table, dev) != NULL) {
    pr_err("Device is already binded in dev queue table\n");
    return -EBUSY;
  }

  // Create queue array for tx and rx
  struct queue_array *tx_queue_array = queue_array_create(CORE_NUM);
  struct queue_array *rx_queue_array = queue_array_create(CORE_NUM);
  if (!tx_queue_array || !rx_queue_array) {
    pr_err("Failed to create queue array\n");
    return -ENOMEM;
  }

  // Add queue array to queue array list
  queue_array_list_insert(&global_queue_array_list, tx_queue_array);
  queue_array_list_insert(&global_queue_array_list, rx_queue_array);

  // Insert queue array to dev queue table
  dev_queue_table_insert(&global_dev_queue_table, dev, tx_queue_array,
                         rx_queue_array);

  // Assign offset to each queue and add to offset queue table
  loff_t offset =
      offset_queue_fetch_next(&global_offset_queue_table, CORE_NUM * 2);
  loff_t tx_offset_start = offset;
  loff_t rx_offset_start = offset;
  struct xsp_queue *queue = NULL;
  FOR_EACH_QUEUE(tx_queue_array, i) {
    queue = tx_queue_array->queue[i];
    offset_queue_table_insert(&global_offset_queue_table, offset, dev, queue);
    offset += PAGE_SIZE;
  }
  rx_offset_start = offset;
  FOR_EACH_QUEUE(rx_queue_array, i) {
    queue = rx_queue_array->queue[i];
    offset_queue_table_insert(&global_offset_queue_table, offset, dev, queue);
    offset += PAGE_SIZE;
  }

  // Set rx handler for the device
  rtnl_lock();
  ret = netdev_rx_handler_register(dev, xsp_handle_frame, rx_queue_array);
  rtnl_unlock();
  if (ret) {
    pr_err("register %s rx handle, result: %d", info.dev_name, ret);
    return ret;
  }

  // Copy out argruments into info
  info.step = PAGE_SIZE;
  info.rx_start_offset = rx_offset_start;
  info.rx_queue_num = CORE_NUM;
  info.rx_queue_size = rx_queue_array->queue[0]->ring_vmalloc_size;
  info.tx_start_offset = tx_offset_start;
  info.tx_queue_num = CORE_NUM;
  info.tx_queue_size = tx_queue_array->queue[0]->ring_vmalloc_size;
  ret = copy_to_user(user_info_addr, &info, sizeof(struct bind_dev_info));
  if (ret) {
    pr_err("copy_to_user failed %d \n", ret);
  }

  pr_info("bind dev %s successfully\n", info.dev_name);
  return ret;
}

static inline int handle_send(struct net_device *dev, struct xsp_queue *queue) {
  if (!dev || !queue) {
    pr_err("Error in offset table");
    return -EINVAL;
  }
  u32 nb_pkts = xspq_cons_nb_entries(queue, 4096);
  for (u32 i = 0; i < nb_pkts; i++) {
    u64 addr;
    bool ret = xspq_cons_read_addr_unchecked_inc(queue, &addr);
    BUG_ON(!ret);

    // xmit the packet to dev
    struct sk_buff *skb = (struct sk_buff *)addr;
    if (IS_ERR(skb) || refcount_read(&skb->users) != 1) {
      pr_err("Err in send packet");
      continue;
    }
    skb->dev = dev;
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
    if (dev_queue_xmit(skb) == NETDEV_TX_BUSY) {
      consume_skb(skb);
    }
  }
  xspq_cons_release(queue);
  return 0;
}

static long xspdev_ioctl(struct file *file, unsigned int cmd,
                         unsigned long arg) {
  struct offset_queue_entry *offset_entry = NULL;
  struct dev_queue_entry *dev_queue_entry = NULL;
  switch (cmd) {
  case IOCTL_BIND_DEV:
    return bind_dev((void *)arg);
  case IOCTL_SEND:
    offset_entry = offset_queue_table_lookup(&global_offset_queue_table, arg);
    if (!offset_entry) {
      pr_err("Failed to lookup queue by offset %lld\n", (u64)arg);
      return -EINVAL;
    }
    return handle_send(offset_entry->dev, offset_entry->queue);
  case IOCTL_SEND_ALL:
    for (int i = 0; i < DEV_QUEUE_TABLE_SIZE; i++) {
      hlist_for_each_entry_rcu(
          dev_queue_entry, &(global_dev_queue_table.buckets[i]), hlist_node) {
        FOR_EACH_QUEUE(dev_queue_entry->tx_queue_array, j) {
          handle_send(dev_queue_entry->dev,
                      dev_queue_entry->tx_queue_array->queue[j]);
        }
      }
    }
    break;
  default:
    pr_err("Unknown ioctl cmd: %u", cmd);
    return -EINVAL;
  }
  return 0;
}

static int __init xsp_init(void) {
  dev_t dev;
  int ret;

  // Create character device
  ret = alloc_chrdev_region(&dev, 0, 1, DEVICE_NAME);
  if (ret < 0) {
    pr_err("Failed to allocate chrdev region\n");
    return ret;
  }
  major = MAJOR(dev);
  xspdev_class = class_create(DEVICE_NAME);
  if (IS_ERR(xspdev_class)) {
    unregister_chrdev_region(MKDEV(major, 0), 1);
    return PTR_ERR(xspdev_class);
  }
  cdev_init(&xspdev_cdev, &xsp_fops);
  xspdev_cdev.owner = THIS_MODULE;
  ret = cdev_add(&xspdev_cdev, MKDEV(major, 0), 1);
  if (ret) {
    class_destroy(xspdev_class);
    unregister_chrdev_region(MKDEV(major, 0), 1);
    return ret;
  }
  device_create(xspdev_class, NULL, MKDEV(major, 0), NULL, DEVICE_NAME);

  // Initialize related data structure
  queue_array_list_init(&global_queue_array_list);
  dev_queue_table_init(&global_dev_queue_table);
  offset_queue_table_init(&global_offset_queue_table);

  pr_info("xsp module initialized\n");

  return 0;
}

static void __exit xsp_exit(void) {
  // # TODO
  // Prevent other operation to execute here so that we can destory resource
  // safely.

  // Unregister rx handler
  struct dev_queue_entry *entry = NULL;
  struct hlist_node *tmp;
  for (int i = 0; i < DEV_QUEUE_TABLE_SIZE; i++) {
    hlist_for_each_entry_safe(entry, tmp, &global_dev_queue_table.buckets[i],
                              hlist_node) {
      rtnl_lock();
      netdev_rx_handler_unregister(entry->dev);
      rtnl_unlock();
      dev_put(entry->dev);
      pr_info("Unregister device rx handler%s\n", entry->dev->name);
    }
  }

  // Destory device
  device_destroy(xspdev_class, MKDEV(major, 0));
  class_destroy(xspdev_class);
  cdev_del(&xspdev_cdev);
  unregister_chrdev_region(MKDEV(major, 0), 1);

  // Destroy queue
  queue_array_list_destroy(&global_queue_array_list);

  // Clear table
  dev_queue_table_clear(&global_dev_queue_table);
  offset_queue_table_clear(&global_offset_queue_table);

  pr_info("xsp module exit\n");
}

module_init(xsp_init);
module_exit(xsp_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("ZENOTME");
MODULE_DESCRIPTION("XSP module");
MODULE_VERSION("1.0");
