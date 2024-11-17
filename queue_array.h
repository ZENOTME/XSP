#include "xsp_queue.h"
#include <linux/spinlock.h>

#define QUEUE_ENTRY_NUM 4096

#define FOR_EACH_QUEUE(queue_array, i)                                         \
  for (size_t i = 0; i < queue_array->size; i++)

struct queue_array {
  size_t size;
  struct xsp_queue *queue[0];
};

struct queue_array *queue_array_create(size_t size);
void queue_array_destroy(struct queue_array *queue_array);

struct queue_array *queue_array_create(size_t size) {
  // Allocate memory for queue array
  struct queue_array *queue_array =
      kmalloc(sizeof(struct queue_array) + sizeof(struct xsp_queue *) * size,
              GFP_KERNEL);
  if (!queue_array) {
    return NULL;
  }
  // Allocate memory for each queue
  size_t cur_queue_idx = 0;
  for (; cur_queue_idx < size; cur_queue_idx++) {
    queue_array->queue[cur_queue_idx] = xspq_create(QUEUE_ENTRY_NUM);
    if (!queue_array->queue[cur_queue_idx]) {
      goto err;
    }
  }
  // Allocate successfully, return queue array
  queue_array->size = size;
  return queue_array;
err:
  // Allocate failed, free the allocated queue array and return NULL
  for (size_t i = 0; i < cur_queue_idx; i++) {
    xspq_destroy(queue_array->queue[i]);
  }
  kfree(queue_array);
  return NULL;
}

void queue_array_destroy(struct queue_array *queue_array) {
  // Free each queue
  for (size_t i = 0; i < queue_array->size; i++) {
    xspq_destroy(queue_array->queue[i]);
  }
  // Free queue array
  kfree(queue_array);
}

// Each binded device has multiple queue array(one for rx, one for tx), all
// queue array is stored in queue_array_list, the queue_array_list is
// reponsible for freeing the queue array when the module exit.
struct queue_array_list_entry {
  struct list_head list;
  struct queue_array *queue;
};

struct queue_array_list {
  struct list_head list;
  spinlock_t lock;
};

inline void queue_array_list_init(struct queue_array_list *array_list);
inline void queue_array_list_insert(struct queue_array_list *array_list,
                                    struct queue_array *queue_array);
inline void queue_array_list_destroy(struct queue_array_list *array_list);

inline void queue_array_list_init(struct queue_array_list *array_list) {
  INIT_LIST_HEAD(&(array_list->list));
  spin_lock_init(&(array_list->lock));
}

inline void queue_array_list_insert(struct queue_array_list *array_list,
                                    struct queue_array *queue_array) {
  struct queue_array_list_entry *entry =
      kmalloc(sizeof(struct queue_array_list_entry), GFP_KERNEL);
  if (!entry) {
    return;
  }
  entry->queue = queue_array;
  spin_lock(&(array_list->lock));
  list_add_tail(&(entry->list), &(array_list->list));
  spin_unlock(&(array_list->lock));
}

inline void queue_array_list_destroy(struct queue_array_list *array_list) {
  struct queue_array_list_entry *entry, *tmp;
  LIST_HEAD(to_free_list);

  // move out all elements in atomic context
  spin_lock(&(array_list->lock));
  list_for_each_entry_safe(entry, tmp, &(array_list->list), list) {
    list_del(&(entry->list));
    list_add_tail(&(entry->list), &to_free_list);
  }
  spin_unlock(&(array_list->lock));

  // destory them
  list_for_each_entry_safe(entry, tmp, &to_free_list, list) {
    queue_array_destroy(entry->queue);
    kfree(entry);
  }
}
