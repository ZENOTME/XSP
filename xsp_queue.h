/* SPDX-License-Identifier: GPL-2.0 */
/* Originally from xdp queue
 * XSP ring structure
 * Copyright(c) 2018 Intel Corporation.
 */

#ifndef _LINUX_XSP_QUEUE_H
#define _LINUX_XSP_QUEUE_H

#include <linux/if_xdp.h>
#include <linux/mm.h>
#include <linux/overflow.h>
#include <linux/smp.h>
#include <linux/types.h>
#include <linux/vmalloc.h>

struct xsp_ring {
  u32 producer __attribute__((__aligned__((1 << (6)))));
  /* Hinder the adjacent cache prefetcher to prefetch the consumer
   * pointer if the producer pointer is touched and vice versa.
   */
  u32 pad1 __attribute__((__aligned__((1 << (6)))));
  u32 consumer __attribute__((__aligned__((1 << (6)))));
  u32 pad2 __attribute__((__aligned__((1 << (6)))));
  u32 nentries;
  u32 flag;
  u32 pad3 __attribute__((__aligned__((1 << (6)))));
};

struct xsp_ring_buffer {
  struct xsp_ring ptrs;
  u64 addrs[] __attribute__((__aligned__((1 << (6)))));
};

struct xsp_queue {
  u32 ring_mask;
  u32 nentries;
  u32 cached_prod;
  u32 cached_cons;
  struct xsp_ring *addrs;
  u64 invalid_descs;
  u64 queue_empty_descs;
  size_t ring_vmalloc_size;
};

/* The structure of the shared state of the rings are a simple
 * circular buffer, as outlined in
 * Documentation/core-api/circular-buffers.rst. For the Rx and
 * completion ring, the kernel is the producer and user space is the
 * consumer. For the Tx and fill rings, the kernel is the consumer and
 * user space is the producer.
 *
 * producer                         consumer
 *
 * if (LOAD ->consumer) {  (A)      LOAD.acq ->producer  (C)
 *    STORE $data                   LOAD $data
 *    STORE.rel ->producer (B)      STORE.rel ->consumer (D)
 * }
 *
 * (A) pairs with (D), and (B) pairs with (C).
 *
 * Starting with (B), it protects the data from being written after
 * the producer pointer. If this barrier was missing, the consumer
 * could observe the producer pointer being set and thus load the data
 * before the producer has written the new data. The consumer would in
 * this case load the old data.
 *
 * (C) protects the consumer from speculatively loading the data before
 * the producer pointer actually has been read. If we do not have this
 * barrier, some architectures could load old data as speculative loads
 * are not discarded as the CPU does not know there is a dependency
 * between ->producer and data.
 *
 * (A) is a control dependency that separates the load of ->consumer
 * from the stores of $data. In case ->consumer indicates there is no
 * room in the buffer to store $data we do not. The dependency will
 * order both of the stores after the loads. So no barrier is needed.
 *
 * (D) protects the load of the data to be observed to happen after the
 * store of the consumer pointer. If we did not have this memory
 * barrier, the producer could observe the consumer pointer being set
 * and overwrite the data with a new value before the consumer got the
 * chance to read the old value. The consumer would thus miss reading
 * the old entry and very likely read the new entry twice, once right
 * now and again after circling through the ring.
 */

/* The operations on the rings are the following:
 *
 * producer                           consumer
 *
 * RESERVE entries                    PEEK in the ring for entries
 * WRITE data into the ring           READ data from the ring
 * SUBMIT entries                     RELEASE entries
 *
 * The producer reserves one or more entries in the ring. It can then
 * fill in these entries and finally submit them so that they can be
 * seen and read by the consumer.
 *
 * The consumer peeks into the ring to see if the producer has written
 * any new entries. If so, the consumer can then read these entries
 * and when it is done reading them release them back to the producer
 * so that the producer can use these slots to fill in new entries.
 *
 * The function names below reflect these operations.
 */

/* Functions for consumers */

static inline void __xspq_cons_read_addr_unchecked(struct xsp_queue *q,
                                                   u32 cached_cons, u64 *addr) {
  struct xsp_ring_buffer *ring = (struct xsp_ring_buffer *)q->addrs;
  u32 idx = cached_cons & q->ring_mask;

  *addr = ring->addrs[idx];
}

static inline bool xspq_cons_read_addr_unchecked(struct xsp_queue *q,
                                                 u64 *addr) {
  __xspq_cons_read_addr_unchecked(q, q->cached_cons, addr);
  return true;
}

static inline bool xspq_cons_read_addr_unchecked_inc(struct xsp_queue *q,
                                                     u64 *addr) {
  bool ret = xspq_cons_read_addr_unchecked(q, addr);
  q->cached_cons++;
  return ret;
}

static inline void __xspq_cons_release(struct xsp_queue *q) {
  smp_store_release(&q->addrs->consumer, q->cached_cons); /* D, matchees A */
}

static inline void xspq_cons_release(struct xsp_queue *q) {
  __xspq_cons_release(q);
}

static inline void __xspq_cons_peek(struct xsp_queue *q) {
  /* Refresh the local pointer */
  q->cached_prod = smp_load_acquire(&q->addrs->producer); /* C, matches B */
}

static inline u32 xspq_cons_nb_entries(struct xsp_queue *q, u32 max) {
  u32 entries = q->cached_prod - q->cached_cons;

  if (entries >= max)
    return max;

  __xspq_cons_peek(q);
  entries = q->cached_prod - q->cached_cons;

  return entries >= max ? max : entries;
}

/* Functions for producers */

static inline u32 xspq_prod_nb_free(struct xsp_queue *q, u32 max) {
  u32 free_entries = q->nentries - (q->cached_prod - q->cached_cons);

  if (free_entries >= max)
    return max;

  /* Refresh the local tail pointer */
  q->cached_cons = READ_ONCE(q->addrs->consumer);
  free_entries = q->nentries - (q->cached_prod - q->cached_cons);

  return free_entries >= max ? max : free_entries;
}

static inline bool xspq_prod_is_full(struct xsp_queue *q) {
  return xspq_prod_nb_free(q, 1) ? false : true;
}

static inline int xspq_prod_reserve_addr(struct xsp_queue *q, u64 addr) {
  struct xsp_ring_buffer *ring = (struct xsp_ring_buffer *)q->addrs;

  if (xspq_prod_is_full(q))
    return -ENOSPC;

  /* A, matches D */
  ring->addrs[q->cached_prod++ & q->ring_mask] = addr;
  return 0;
}

static inline void __xspq_prod_submit(struct xsp_queue *q, u32 idx) {
  smp_store_release(&q->addrs->producer, idx); /* B, matches C */
}

static inline void xspq_prod_submit(struct xsp_queue *q) {
  __xspq_prod_submit(q, q->cached_prod);
}

static inline size_t xspq_prod_num(struct xsp_queue *q) {
  return READ_ONCE(q->addrs->producer) - READ_ONCE(q->addrs->consumer);
}

/* For both producers and consumers */
struct xsp_queue *xspq_create(u32 nentries);
void xspq_destroy(struct xsp_queue *q);

static size_t xspq_get_ring_size(struct xsp_queue *q) {
  struct xsp_ring_buffer *ring_buffer;

  return struct_size(ring_buffer, addrs, q->nentries);
}

struct xsp_queue *xspq_create(u32 nentries) {
  if (!is_power_of_2(nentries)) {
    return NULL;
  }

  struct xsp_queue *q;
  size_t size;

  q = kzalloc(sizeof(*q), GFP_KERNEL);
  if (!q)
    return NULL;

  q->nentries = nentries;
  q->ring_mask = nentries - 1;

  size = xspq_get_ring_size(q);

  /* size which is overflowing or close to SIZE_MAX will become 0 in
   * PAGE_ALIGN(), checking SIZE_MAX is enough due to the previous
   * is_power_of_2(), the rest will be handled by vmalloc_user()
   */
  if (unlikely(size == SIZE_MAX)) {
    kfree(q);
    return NULL;
  }

  size = PAGE_ALIGN(size);

  q->addrs = vmalloc_user(size);
  q->addrs->nentries = nentries;
  if (!q->addrs) {
    kfree(q);
    return NULL;
  }

  q->ring_vmalloc_size = size;
  return q;
}

void xspq_destroy(struct xsp_queue *q) {
  if (!q)
    return;

  vfree(q->addrs);
  kfree(q);
}

#endif /* _LINUX_XSP_QUEUE_H */
