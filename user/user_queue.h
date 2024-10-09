#ifndef _USER_QUEUE_H
#define _USER_QUEUE_H

#include <stdio.h>
#define u32 unsigned
#define u64 unsigned long long

#define smp_rmb() asm volatile("" : : : "memory")
#define smp_wmb() asm volatile("" : : : "memory")
#define smp_mb() asm volatile("lock; addl $0,-4(%%rsp)" : : : "memory", "cc")
#define smp_rwmb() asm volatile("" : : : "memory")

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

/* Used for the fill and completion queues for buffers */
struct xsp_ring_buffer {
  struct xsp_ring ptrs;
  u64 addrs[] __attribute__((__aligned__((1 << (6)))));
};

struct xsp_queue {
  u32 cached_prod;
  u32 cached_cons;
  u32 mask;
  u32 nentries;
  u32 *producer;
  u32 *consumer;
  void *addrs;
};

static void init_xsp_queue(struct xsp_queue *queue,
                           struct xsp_ring_buffer *ring) {
  queue->cached_prod = ring->ptrs.producer;
  queue->cached_cons = ring->ptrs.consumer;
  queue->mask = ring->ptrs.nentries - 1;
  queue->nentries = ring->ptrs.nentries;
  queue->producer = &ring->ptrs.producer;
  queue->consumer = &ring->ptrs.consumer;
  queue->addrs = ring->addrs;
}

static inline u64 *xsp_ring_prod__fill_addr(struct xsp_queue *fill, u32 idx) {
  u64 *addrs = (u64 *)fill->addrs;

  return &addrs[idx & fill->mask];
}

static inline u32 xsp_prod_nb_free(struct xsp_queue *r, u32 nb) {
  u32 free_entries = r->nentries - (r->cached_cons - r->cached_prod);
  if (free_entries >= nb)
    return nb;

  // # TODO
  // READ_ONCE here?
  r->cached_cons = *r->consumer;
  free_entries = r->nentries - (r->cached_cons - r->cached_prod);

  return free_entries >= nb ? nb : free_entries;
}

static inline size_t xsp_ring_prod__reserve(struct xsp_queue *prod, size_t nb,
                                            u32 *idx) {
  if (xsp_prod_nb_free(prod, nb) < nb)
    return 0;

  *idx = prod->cached_prod;
  prod->cached_prod += nb;

  return nb;
}

static inline void xsp_ring_prod__submit(struct xsp_queue *prod, size_t nb) {
  /* Make sure everything has been written to the ring before indicating
   * this to the kernel by writing the producer pointer.
   */
  smp_wmb();

  *prod->producer += nb;
}

static inline const u64 *xsp_ring_cons__comp_addr(const struct xsp_queue *comp,
                                                  u32 idx) {
  smp_rmb();

  const u64 *addrs = (const u64 *)comp->addrs;

  return &addrs[idx & comp->mask];
}

static inline u32 xsp_cons_nb_avail(struct xsp_queue *r, u32 nb) {
  u32 entries = r->cached_prod - r->cached_cons;

  if (entries == 0) {
    smp_rmb();
    r->cached_prod = *r->producer;
    entries = r->cached_prod - r->cached_cons;
  }

  return (entries > nb) ? nb : entries;
}

static inline size_t xsp_ring_cons__peek(struct xsp_queue *cons, size_t nb,
                                         u32 *idx) {
  size_t entries = xsp_cons_nb_avail(cons, nb);

  if (entries > 0) {
    /* Make sure we do not speculatively read the data before
     * we have received the packet buffers from the ring.
     */
    smp_rmb();

    *idx = cons->cached_cons;
    cons->cached_cons += entries;
  }

  return entries;
}

static inline void xsp_ring_cons__release(struct xsp_queue *cons, size_t nb) {
  /* Make sure data has been read before indicating we are done
   * with the entries by updating the consumer pointer.
   */
  smp_rwmb();

  *cons->consumer += nb;
}

#endif