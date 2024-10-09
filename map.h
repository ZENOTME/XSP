#include "xsp_queue.h"
#include <linux/cdev.h>
#include <linux/etherdevice.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/netpoll.h>
#include <linux/rculist.h>
#include <linux/rtnetlink.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/veth.h>

/// # NOTE
/// All operation of map is thread safe guarded by rcu and spinlock.

#define DEV_QUEUE_TABLE_SIZE 1 << 10
#define OFFSET_QUEUE_TABLE_SIZE 1 << 10

struct dev_queue_entry {
  struct net_device *dev;
  struct queue_array *tx_queue_array;
  struct queue_array *rx_queue_array;
  struct hlist_node hlist_node;
  struct rcu_head rcu;
};

struct dev_queue_table {
  struct hlist_head buckets[DEV_QUEUE_TABLE_SIZE];
  spinlock_t lock;
};

void dev_queue_table_init(struct dev_queue_table *table);
void dev_queue_table_insert(struct dev_queue_table *table,
                            struct net_device *dev,
                            struct queue_array *tx_queue_array,
                            struct queue_array *rx_queue_array);
struct dev_queue_entry *dev_queue_table_lookup(struct dev_queue_table *table,
                                               struct net_device *dev);
void dev_queue_table_remove(struct dev_queue_table *table,
                            struct net_device *dev);
void dev_queue_table_clear(struct dev_queue_table *table);

static inline int hash_func(void *key) {
  return (int)(((unsigned long)key) % DEV_QUEUE_TABLE_SIZE);
}

void dev_queue_table_init(struct dev_queue_table *table) {
  int i;
  for (i = 0; i < DEV_QUEUE_TABLE_SIZE; i++) {
    INIT_HLIST_HEAD(&table->buckets[i]);
  }
  spin_lock_init(&table->lock);
}

void dev_queue_table_insert(struct dev_queue_table *table,
                            struct net_device *dev,
                            struct queue_array *tx_queue_array,
                            struct queue_array *rx_queue_array) {
  int hash = hash_func(dev);
  struct dev_queue_entry *new_entry =
      kmalloc(sizeof(struct dev_queue_entry), GFP_KERNEL);
  new_entry->dev = dev;
  new_entry->tx_queue_array = tx_queue_array;
  new_entry->rx_queue_array = rx_queue_array;

  spin_lock(&table->lock);
  hlist_add_head_rcu(&new_entry->hlist_node, &table->buckets[hash]);
  spin_unlock(&table->lock);
}

struct dev_queue_entry *dev_queue_table_lookup(struct dev_queue_table *table,
                                               struct net_device *dev) {
  struct dev_queue_entry *entry = NULL;
  int hash = hash_func(dev);

  rcu_read_lock();
  hlist_for_each_entry_rcu(entry, &table->buckets[hash], hlist_node) {
    if (entry->dev == dev) {
      break;
    }
  }
  rcu_read_unlock();

  return entry;
}

void dev_queue_table_remove(struct dev_queue_table *table,
                            struct net_device *dev) {
  struct dev_queue_entry *entry;
  int hash = hash_func(dev);

  spin_lock(&table->lock);
  hlist_for_each_entry_rcu(entry, &table->buckets[hash], hlist_node) {
    if (entry->dev == dev) {
      hlist_del_rcu(&entry->hlist_node);
      kfree_rcu(entry, rcu);
      break;
    }
  }
  spin_unlock(&table->lock);
}

void dev_queue_table_clear(struct dev_queue_table *table) {
  struct dev_queue_entry *entry;
  int i;

  spin_lock(&table->lock);
  for (i = 0; i < DEV_QUEUE_TABLE_SIZE; i++) {
    hlist_for_each_entry_rcu(entry, &table->buckets[i], hlist_node) {
      hlist_del_rcu(&entry->hlist_node);
      kfree_rcu(entry, rcu);
    }
  }
  spin_unlock(&table->lock);
}

struct offset_queue_entry {
  struct net_device *dev;
  struct xsp_queue *queue;
};

struct offset_queue_table {
  atomic64_t next_index;
  u64 queue_capacity;
  u64 queue_num;
  struct offset_queue_entry *queue_array;
  spinlock_t lock;
};

int offset_queue_table_init(struct offset_queue_table *table);
loff_t offset_queue_next_offset(struct offset_queue_table *table);
int offset_queue_table_insert(struct offset_queue_table *table, loff_t offset,
                              struct net_device *dev, struct xsp_queue *queue);
struct offset_queue_entry *
offset_queue_table_lookup(struct offset_queue_table *table, loff_t offset);
void offset_queue_table_clear(struct offset_queue_table *table);

static inline loff_t offset_queue_fetch_next(struct offset_queue_table *table,
                                             size_t queue_num) {
  return atomic64_fetch_add(queue_num, &table->next_index) << PAGE_SHIFT;
}

static inline u64 offset_to_index(loff_t offset) {
  return offset >> PAGE_SHIFT;
}

int offset_queue_table_init(struct offset_queue_table *table) {
  table->queue_capacity = 4096;
  table->queue_num = 0;
  table->queue_array = kmalloc_array(table->queue_capacity,
                                     sizeof(struct xsp_queue *), GFP_KERNEL);
  if (!table->queue_array) {
    printk(KERN_ERR "Failed to allocate memory for offset queue array\n");
    return -1;
  }
  atomic64_set(&table->next_index, 0);
  spin_lock_init(&table->lock);
  return 0;
}

int offset_queue_table_insert(struct offset_queue_table *table, loff_t offset,
                              struct net_device *dev, struct xsp_queue *queue) {
  BUG_ON(table->queue_array == NULL);
  BUG_ON(table->queue_num > table->queue_capacity);

  // If the queue array is full, we need to expand it.
  spin_lock(&table->lock);
  if (unlikely(table->queue_num == table->queue_capacity)) {
    struct offset_queue_entry *new_array =
        kmalloc_array(table->queue_capacity * 2,
                      sizeof(struct offset_queue_entry), GFP_KERNEL);
    if (!new_array) {
      printk(KERN_ERR
             "Failed to allocate memory for offset queue array expansion\n");
      return -1;
    }
    for (int i = 0; i < table->queue_capacity; i++) {
      new_array[i].queue = table->queue_array[i].queue;
      new_array[i].dev = table->queue_array[i].dev;
    }
    kfree(table->queue_array);
    table->queue_array = new_array;
    table->queue_capacity *= 2;
  }

  u64 idx = table->queue_num++;
  table->queue_array[idx].queue = queue;
  table->queue_array[idx].dev = dev;
  spin_unlock(&table->lock);
  return 0;
}

struct offset_queue_entry *
offset_queue_table_lookup(struct offset_queue_table *table, loff_t offset) {
  BUG_ON(table->queue_array == NULL);
  u64 index = offset_to_index(offset);
  if (index >= table->queue_num) {
    return NULL;
  }

  return &table->queue_array[index];
}

void offset_queue_table_clear(struct offset_queue_table *table) {
  if (table->queue_array) {
    kfree(table->queue_array);
    table->queue_array = NULL;
  }
  table->queue_num = 0;
  table->queue_capacity = 0;
}

struct ptr_vector {
  void **data;
  size_t size;
  size_t capacity;
};

int vector_init(struct ptr_vector *vec);
int vector_insert(struct ptr_vector *vec, void *value);
void vector_clear(struct ptr_vector *vec);

#define INITIAL_CAPACITY 1024

int vector_init(struct ptr_vector *vec) {
  vec->data = kmalloc_array(INITIAL_CAPACITY, sizeof(void *), GFP_KERNEL);
  if (!vec->data) {
    printk(KERN_ERR "Failed to allocate memory for vector\n");
    return -ENOMEM;
  }
  vec->size = 0;
  vec->capacity = INITIAL_CAPACITY;
  return 0;
}

int vector_insert(struct ptr_vector *vec, void *value) {
  if (vec->size == vec->capacity) {
    size_t new_capacity = vec->capacity * 2;
    void **new_data =
        krealloc(vec->data, new_capacity * sizeof(void *), GFP_KERNEL);
    if (!new_data) {
      printk(KERN_ERR "Failed to reallocate memory for vector\n");
      return -ENOMEM;
    }
    vec->data = new_data;
    vec->capacity = new_capacity;
  }
  BUG_ON(vec->size >= vec->capacity);
  vec->data[vec->size++] = value;
  return 0;
}

void vector_clear(struct ptr_vector *vec) {
  kfree(vec->data);
  vec->data = NULL;
  vec->size = 0;
  vec->capacity = 0;
}