#ifndef _USER_QUEUE_H
#define _USER_QUEUE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#define u32 uint32_t
#define u64 unsigned long long

#define smp_rmb() asm volatile("" : : : "memory")
#define smp_wmb() asm volatile("" : : : "memory")
#define smp_mb() asm volatile("lock; addl $0,-4(%%rsp)" : : : "memory", "cc")
#define smp_rwmb() asm volatile("" : : : "memory")

struct xdp_ring {
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
struct xdp_umem_ring {
  struct xdp_ring ptrs;
  u64 desc[] __attribute__((__aligned__((1 << (6)))));
};

struct xsk_ring_prod {
  u32 cached_prod;
  u32 cached_cons;
  u32 mask;
  u32 size;
  u32 *producer;
  u32 *consumer;
  void *ring;
};

static void init_ring_prod(struct xsk_ring_prod *prod,
                           struct xdp_umem_ring *ring) {
  prod->cached_prod = ring->ptrs.producer;
  prod->cached_cons = ring->ptrs.consumer;
  prod->mask = ring->ptrs.nentries - 1;
  prod->size = ring->ptrs.nentries;
  prod->producer = &ring->ptrs.producer;
  prod->consumer = &ring->ptrs.consumer;
  prod->ring = ring->desc;
}

static inline u64 *xsk_ring_prod__fill_addr(struct xsk_ring_prod *fill,
                                            u32 idx) {
  u64 *addrs = (u64 *)fill->ring;

  return &addrs[idx & fill->mask];
}

static inline u32 xsk_prod_nb_free(struct xsk_ring_prod *r, u32 nb) {
  u32 free_entries = r->size - (r->cached_cons - r->cached_prod);
  if (free_entries >= nb)
    return nb;

  // # TODO
  // READ_ONCE here?
  r->cached_cons = *r->consumer;
  free_entries = r->size - (r->cached_cons - r->cached_prod);

  return free_entries >= nb ? nb : free_entries;
}

static inline size_t xsk_ring_prod__reserve(struct xsk_ring_prod *prod,
                                            size_t nb, u32 *idx) {
  if (xsk_prod_nb_free(prod, nb) < nb)
    return 0;

  *idx = prod->cached_prod;
  prod->cached_prod += nb;

  return nb;
}

static inline void xsk_ring_prod__submit(struct xsk_ring_prod *prod,
                                         size_t nb) {
  /* Make sure everything has been written to the ring before indicating
   * this to the kernel by writing the producer pointer.
   */
  smp_wmb();

  *prod->producer += nb;
}

struct xsk_ring_cons {
  u32 cached_prod;
  u32 cached_cons;
  u32 mask;
  u32 size;
  u32 *producer;
  u32 *consumer;
  void *ring;
};

static void init_ring_cons(struct xsk_ring_cons *cons,
                           struct xdp_umem_ring *ring) {
  cons->cached_prod = ring->ptrs.producer;
  cons->cached_cons = ring->ptrs.consumer;
  cons->mask = ring->ptrs.nentries - 1;
  cons->size = ring->ptrs.nentries;
  cons->producer = &ring->ptrs.producer;
  cons->consumer = &ring->ptrs.consumer;
  cons->ring = ring->desc;
}

static inline const u64 *
xsk_ring_cons__comp_addr(const struct xsk_ring_cons *comp, u32 idx) {
  smp_rmb();

  const u64 *addrs = (const u64 *)comp->ring;

  return &addrs[idx & comp->mask];
}

static inline u32 xsk_cons_nb_avail(struct xsk_ring_cons *r, u32 nb) {
  u32 entries = r->cached_prod - r->cached_cons;

  if (entries == 0) {
	smp_rmb();
    r->cached_prod = *r->producer;
    entries = r->cached_prod - r->cached_cons;
  }

  return (entries > nb) ? nb : entries;
}

static inline size_t xsk_ring_cons__peek(struct xsk_ring_cons *cons, size_t nb,
                                         u32 *idx) {
  size_t entries = xsk_cons_nb_avail(cons, nb);

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

static inline void xsk_ring_cons__release(struct xsk_ring_cons *cons,
                                          size_t nb) {
  /* Make sure data has been read before indicating we are done
   * with the entries by updating the consumer pointer.
   */
  smp_rwmb();

  *cons->consumer += nb;
}

#endif