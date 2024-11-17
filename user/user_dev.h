#ifndef _DEV_H
#define _DEV_H

#include "../common_config.h"
#include "user_queue.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <unistd.h>

static void copy_dev_info(struct bind_dev_info *dst, struct bind_dev_info *src) {
  strcpy(dst->dev_name, src->dev_name);
  dst->step = src->step;
  dst->rx_start_offset = src->rx_start_offset;
  dst->rx_queue_num = src->rx_queue_num;
  dst->rx_queue_size = src->rx_queue_size;
  dst->tx_start_offset = src->tx_start_offset;
  dst->tx_queue_num = src->tx_queue_num;
  dst->tx_queue_size = src->tx_queue_size;
}

#define PRINT_BIND_DEV_INFO(print, info)                                       \
  print("Device Name: %s\n", info->dev_name);                                  \
  print("  Step: %lu\n", info->step);                                          \
  print("  RX Start Offset: %lu\n", info->rx_start_offset);                    \
  print("  RX Queue Number: %lu\n", info->rx_queue_num);                       \
  print("  RX Queue Size: %lu\n", info->rx_queue_size);                        \
  print("  TX Start Offset: %lu\n", info->tx_start_offset);                    \
  print("  TX Queue Number: %lu\n", info->tx_queue_num);                       \
  print("  TX Queue Size: %lu\n", info->tx_queue_size);

struct bind_dev_result {
  int success;
  struct bind_dev_info dev_info;
  struct xsp_queue **rx_queue;
  uint64_t rx_queue_num;
  struct xsp_queue **tx_queue;
  uint64_t tx_queue_num;
};

static void print_bind_dev_result(struct bind_dev_result *result) {
  printf("dev: %s\n", result->dev_info.dev_name);
  printf("success: %d\n", result->success);

  printf("rx_queue_num: %lu\n", result->rx_queue_num);
  assert(result->rx_queue);
  for (uint64_t i = 0; i < result->rx_queue_num; i++) {
    printf("  tx_queue[%lu](nentry: %u)\n", i, result->tx_queue[i]->nentries);
  }

  printf("tx_queue_num: %lu\n", result->tx_queue_num);
  assert(result->tx_queue);
  for (uint64_t i = 0; i < result->tx_queue_num; i++) {
    printf("  tx_queue[%lu](nentry: %u)\n", i, result->tx_queue[i]->nentries);
  }
}

/// Bind queue to device, use should make sure the fd is opened on "/dev/xsp".
static int bind_dev(int fd, struct bind_dev_result *result,
             struct bind_dev_info *dev_info) {
  struct xsp_ring_buffer *ring_buffer = NULL;
  if (!result || !dev_info) {
    perror("result or dev_info is NULL");
    return -1;
  }
  if (ioctl(fd, IOCTL_BIND_DEV, dev_info) < 0) {
    perror("Failed to attach interface");
    close(fd);
    return -1;
  }
  printf("ioctl successly\n");

  PRINT_BIND_DEV_INFO(printf, dev_info);

  result->success = 1;
  copy_dev_info(&result->dev_info, dev_info);
  result->rx_queue_num = dev_info->rx_queue_num;
  result->tx_queue_num = dev_info->tx_queue_num;
  result->rx_queue = (struct xsp_queue **)malloc(sizeof(struct xsp_queue *) *
                                                 dev_info->rx_queue_num);

  result->tx_queue = (struct xsp_queue **)malloc(sizeof(struct xsp_queue *) *
                                                 dev_info->tx_queue_num);
  if (!result->rx_queue || !result->tx_queue) {
    perror("Failed to malloc rx_queue or tx_queue");
    goto err;
  }
  printf("create queue arrray successly\n");

  for (uint64_t i = 0; i < dev_info->rx_queue_num; i++) {
    ring_buffer = (struct xsp_ring_buffer *)mmap(
        NULL, dev_info->rx_queue_size, PROT_READ | PROT_WRITE,
        MAP_SHARED | MAP_POPULATE, fd,
        dev_info->rx_start_offset + i * dev_info->step);
    if (ring_buffer == MAP_FAILED) {
      perror("Failed to mmap rx ring 1");
      goto err;
    }
    result->rx_queue[i] = (struct xsp_queue *)malloc(sizeof(struct xsp_queue));
    if (!result->rx_queue[i]) {
      perror("Failed to malloc rx_queue");
      // TODO free allocated xsp_queue
      goto err;
    }
    init_xsp_queue(result->rx_queue[i], ring_buffer);
  }
  printf("rx_queue mmap successly\n");

  for (uint64_t i = 0; i < dev_info->tx_queue_num; i++) {
    ring_buffer = (struct xsp_ring_buffer *)mmap(
        NULL, dev_info->tx_queue_size, PROT_READ | PROT_WRITE,
        MAP_SHARED | MAP_POPULATE, fd,
        dev_info->tx_start_offset + i * dev_info->step);
    if (ring_buffer == MAP_FAILED) {
      perror("Failed to mmap rx ring 1");
      goto err;
    }
    result->tx_queue[i] = (struct xsp_queue *)malloc(sizeof(struct xsp_queue));
    if (!result->tx_queue[i]) {
      perror("Failed to malloc rx_queue");
      // TODO free allocated xsp_queue
      goto err;
    }
    init_xsp_queue(result->tx_queue[i], ring_buffer);
  }
  printf("tx_queue mmap successly\n");

  return 0;
err:
  if (result->rx_queue) {
    free(result->rx_queue);
  }
  if (result->tx_queue) {
    free(result->tx_queue);
  }
  return -1;
}

static void print_mac_address(uint64_t mac_addr, char *prefix) {
  unsigned char mac[6];
  for (int i = 0; i < 6; i++) {
    mac[i] = (mac_addr >> (5 - i) * 8) & 0xFF;
  }
  printf("%s MAC Address: %02x:%02x:%02x:%02x:%02x:%02x\n", prefix, mac[0],
         mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static inline int receive_pkt(uint64_t *buffer, uint64_t start_idx,
                              uint64_t max_size, struct xsp_queue *queue) {
  uint32_t idx = 0;
  int reserve = 0;
  int avail_receive = xsp_cons_nb_avail(queue, max_size);
  if (avail_receive > 0) {
    reserve = xsp_ring_cons__peek(queue, avail_receive, &idx);
    for (int i = 0; i < reserve; i++) {
      const struct ring_entry *entry = xsp_ring_cons__comp_addr(queue, idx + i);
      buffer[start_idx++] = entry->addr;
      // print_mac_address(entry->src_mac, "src");
      // print_mac_address(entry->dst_mac, "dst");
    }
    xsp_ring_cons__release(queue, reserve);
  }
  return reserve;
}

static inline int send_pkt(uint64_t *buffer, uint64_t start_idx,
                           uint64_t max_size, struct xsp_queue *queue) {
  uint32_t idx = 0;
  int reserve = xsp_ring_prod__reserve(queue, max_size, &idx);
  // if ((size_t)reserve < max_size) {
  //   printf("WARN: reserve %d < max %lu\n", reserve, max_size);
  // }
  for (int i = 0; i < reserve; i++) {
    uint64_t addr = buffer[start_idx + i];
    struct ring_entry *entry = xsp_ring_prod__fill_addr(queue, idx + i);
    entry->addr = addr;
  }
  xsp_ring_prod__submit(queue, reserve);
  return reserve;
}

#endif
