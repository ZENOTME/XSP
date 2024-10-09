#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include "queue_array.h"

static int __init test_queue_array_init(void) {
    struct queue_array *q_array;
    struct queue_array_list q_array_list;

    printk(KERN_INFO "Initializing test for queue_array and queue_array_list\n");

    // 创建 queue_array
    q_array = queue_array_create(10);
    if (!q_array) {
        printk(KERN_ERR "Failed to create queue_array\n");
        return -ENOMEM;
    }
    printk(KERN_INFO "queue_array created with size: %zu\n", q_array->size);

    // 初始化 queue_array_list
    queue_array_list_init(&q_array_list);
    printk(KERN_INFO "queue_array_list initialized\n");

    // 插入 queue_array 到 queue_array_list
    queue_array_list_insert(&q_array_list, q_array);
    printk(KERN_INFO "queue_array inserted into queue_array_list\n");

    // 销毁 queue_array_list
    queue_array_list_destroy(&q_array_list);
    printk(KERN_INFO "queue_array_list destroyed\n");

    // 销毁 queue_array
    queue_array_destroy(q_array);
    printk(KERN_INFO "queue_array destroyed\n");

    return 0;
}

static void __exit test_queue_array_exit(void) {
    printk(KERN_INFO "Exiting test for queue_array and queue_array_list\n");
}

module_init(test_queue_array_init);
module_exit(test_queue_array_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("Test module for queue_array and queue_array_list");