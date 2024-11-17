#ifndef _USER_QUEUE_H
#define _USER_QUEUE_H

#include <stdint.h>

#define size_t uint64_t

#define smp_rmb() asm volatile("" : : : "memory")
#define smp_wmb() asm volatile("" : : : "memory")
#define smp_mb() asm volatile("lock; addl $0,-4(%%rsp)" : : : "memory", "cc")
#define smp_rwmb() asm volatile("" : : : "memory")

struct xsp_ring {
  uint32_t producer __attribute__((__aligned__((1 << (6)))));
  /* Hinder the adjacent cache prefetcher to prefetch the consumer
   * pointer if the producer pointer is touched and vice versa.
   */
  uint32_t pad1 __attribute__((__aligned__((1 << (6)))));
  uint32_t consumer __attribute__((__aligned__((1 << (6)))));
  uint32_t pad2 __attribute__((__aligned__((1 << (6)))));
  uint32_t nentries;
  uint32_t flag;
  uint32_t pad3 __attribute__((__aligned__((1 << (6)))));
};

struct ring_entry {
  uint64_t addr;
  uint64_t src_mac;
  uint64_t dst_mac;
};

/* Used for the fill and completion queues for buffers */
struct xsp_ring_buffer {
  struct xsp_ring ptrs;
  struct ring_entry addrs[] __attribute__((__aligned__((1 << (6)))));
};

struct xsp_queue {
  uint32_t cached_prod;
  uint32_t cached_cons;
  uint32_t mask;
  uint32_t nentries;
  uint32_t *producer;
  uint32_t *consumer;
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

static inline struct ring_entry *xsp_ring_prod__fill_addr(struct xsp_queue *fill, uint32_t idx) {
  struct ring_entry *addrs = (struct ring_entry *)fill->addrs;

  return &addrs[idx & fill->mask];
}

static inline uint32_t xsp_prod_nb_free(struct xsp_queue *r, uint32_t nb) {
  uint32_t free_entries = r->nentries - (r->cached_cons - r->cached_prod);
  if (free_entries >= nb)
    return nb;

  // # TODO
  // READ_ONCE here?
  r->cached_cons = *r->consumer;
  free_entries = r->nentries - (r->cached_cons - r->cached_prod);

  return free_entries >= nb ? nb : free_entries;
}

static inline size_t xsp_ring_prod__reserve(struct xsp_queue *prod, size_t nb,
                                            uint32_t *idx) {
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

static inline const struct ring_entry *xsp_ring_cons__comp_addr(const struct xsp_queue *comp,
                                                  uint32_t idx) {
  smp_rmb();

  const struct ring_entry *addrs = (const struct ring_entry *)comp->addrs;

  return &addrs[idx & comp->mask];
}

static inline uint32_t xsp_cons_nb_avail(struct xsp_queue *r, uint32_t nb) {
  uint32_t entries = r->cached_prod - r->cached_cons;

  if (entries == 0) {
    smp_rmb();
    r->cached_prod = *r->producer;
    entries = r->cached_prod - r->cached_cons;
  }

  return (entries > nb) ? nb : entries;
}

static inline size_t xsp_ring_cons__peek(struct xsp_queue *cons, size_t nb,
                                         uint32_t *idx) {
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