#include "map.h"
#include <linux/module.h>

static struct dev_queue_table dev_table;
static struct offset_queue_table offset_table;

static void test_dev_queue_table(void) {
  struct net_device *mock_dev = (struct net_device *)0x12345678;
  struct net_device *nonexistent_dev = (struct net_device *)0x12345679;
  struct queue_array *mock_rx_queue = (struct queue_array *)0x87654321;
  struct queue_array *mock_tx_queue = (struct queue_array *)0x87654322;
  struct dev_queue_entry *dev_entry;

  // 初始化表
  dev_queue_table_init(&dev_table);

  // 插入到 dev_queue_table
  dev_queue_table_insert(&dev_table, mock_dev, mock_rx_queue, mock_tx_queue);
  dev_entry = dev_queue_table_lookup(&dev_table, mock_dev);
  if (dev_entry) {
    if (dev_entry->rx_queue_array == mock_rx_queue &&
        dev_entry->tx_queue_array == mock_tx_queue) {
      pr_info("Device found in dev_queue_table with correct content\n");
    } else {
      pr_err("Device found in dev_queue_table with incorrect content\n");
    }
  } else {
    pr_err("Device not found in dev_queue_table\n");
  }

  // 尝试查找不存在的 entry
  dev_entry = dev_queue_table_lookup(&dev_table, nonexistent_dev);
  if (!dev_entry) {
    pr_info("Nonexistent device correctly not found in dev_queue_table\n");
  } else {
    pr_err("Nonexistent device incorrectly found in dev_queue_table\n");
  }

  // 从表中移除
  dev_queue_table_remove(&dev_table, mock_dev);
}

static void test_offset_queue_table(void) {
  loff_t mock_offset = 12345;
  loff_t nonexistent_offset = 54321;
  struct xsp_queue *mock_offset_queue = (struct xsp_queue *)0x87654323;
  struct net_device *mock_netdev = (struct net_device *)0x123;
  struct offset_queue_entry *q;

  // 初始化表
  offset_queue_table_init(&offset_table);

  // 插入到 offset_queue_table
  offset_queue_table_insert(&offset_table, mock_offset, mock_netdev,
                            mock_offset_queue);
  q = offset_queue_table_lookup(&offset_table, mock_offset);
  if (q) {
    pr_info("Offset found in offset_queue_table\n");
  } else {
    pr_err("Offset not found in offset_queue_table\n");
  }

  // 尝试查找不存在的 entry
  q = offset_queue_table_lookup(&offset_table, nonexistent_offset);
  if (!q) {
    pr_info("Nonexistent offset correctly not found in offset_queue_table\n");
  } else {
    pr_err("Nonexistent offset incorrectly found in offset_queue_table\n");
  }

  offset_queue_table_clear(&offset_table);
}

static int __init map_test_init(void) {
  test_dev_queue_table();
  test_offset_queue_table();

  return 0;
}

static void __exit map_test_exit(void) {}

module_init(map_test_init);
module_exit(map_test_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("ZENOTME");
MODULE_DESCRIPTION("Test module for dev_queue_table and offset_queue_table");
MODULE_VERSION("1.0");