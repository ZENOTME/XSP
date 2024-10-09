#include "xsp_queue.h"
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>

#define TEST_ENTRIES 16

static int __init queue_test_init(void) {
  struct xsp_queue *queue;
  u64 addr1, addr2;
  int ret;

  printk(KERN_INFO "Initializing queue test module\n");

  queue = xspq_create(TEST_ENTRIES);
  if (!queue) {
    printk(KERN_ERR "Failed to create queue\n");
    return -ENOMEM;
  }

  ret = xspq_prod_reserve_addr(queue, 0x12345678);
  if (ret) {
    printk(KERN_ERR "Failed to reserve first address in queue\n");
    xspq_destroy(queue);
    return ret;
  }
  xspq_prod_submit(queue);

  ret = xspq_prod_reserve_addr(queue, 0x87654321);
  if (ret) {
    printk(KERN_ERR "Failed to reserve second address in queue\n");
    xspq_destroy(queue);
    return ret;
  }
  xspq_prod_submit(queue);

  if (xspq_cons_nb_entries(queue, 4) != 2) {
    printk(KERN_ERR "Consumer count is not 2\n");
    xspq_destroy(queue);
    return -EINVAL;
  }

  if (xspq_cons_read_addr_unchecked_inc(queue, &addr1)) {
    printk(KERN_INFO "Read first address from queue: 0x%llx\n", addr1);
  } else {
    printk(KERN_ERR "Failed to read first address from queue\n");
  }
  if (xspq_cons_read_addr_unchecked_inc(queue, &addr2)) {
    printk(KERN_INFO "Read second address from queue: 0x%llx\n", addr2);
  } else {
    printk(KERN_ERR "Failed to read second address from queue\n");
  }

  xspq_cons_release(queue);

  u32 consumer_count = xspq_cons_nb_entries(queue, 4);
  if (consumer_count != 0) {
    printk(KERN_ERR "Consumer count: %u != 0\n", consumer_count);
  }
  u32 producer_count = xspq_prod_nb_free(queue, TEST_ENTRIES);
  if (producer_count != TEST_ENTRIES) {
    printk(KERN_ERR "Producer count: %u != %u\n", producer_count, TEST_ENTRIES);
  }

  xspq_destroy(queue);

  printk(KERN_INFO "Queue test module initialized successfully\n");
  return 0;
}

static void __exit queue_test_exit(void) {
  printk(KERN_INFO "Exiting queue test module\n");
}

module_init(queue_test_init);
module_exit(queue_test_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("ZENOTME");
MODULE_DESCRIPTION("A simple test module for xsp_queue");