#include "../common_config.h"
#include "user_queue.h"
#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

void copy_dev_info(struct bind_dev_info *dst, struct bind_dev_info *src) {
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

void print_bind_dev_result(struct bind_dev_result *result) {
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
int bind_dev(int fd, struct bind_dev_result *result,
             struct bind_dev_info *dev_info) {
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

  struct xsp_ring_buffer *ring_buffer = NULL;
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

void print_mac_address(u64 mac_addr, char *prefix) {
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
  if ((size_t)reserve < max_size) {
    printf("WARN: reserve %d < max %lu\n", reserve, max_size);
  }
  for (int i = 0; i < reserve; i++) {
    u64 addr = buffer[start_idx + i];
    struct ring_entry *entry = xsp_ring_prod__fill_addr(queue, idx + i);
    entry->addr = addr;
  }
  xsp_ring_prod__submit(queue, reserve);
  return reserve;
}

static inline void forward_once(uint64_t *buffer, uint64_t buffer_size,
                                struct bind_dev_result *dev_src_result,
                                struct bind_dev_result *dev_dst_result) {
  uint64_t receive_end = 0;
  uint64_t send_end = 0;
  int idx = 0;
  int reserve = 0;

  for (uint64_t i = 0; i < dev_src_result->rx_queue_num; i++) {
    // printf("%s rx id: %lu, prod: %u cons: %u\n",
    //        dev_src_result->dev_info.dev_name, i,
    //        *dev_src_result->rx_queue[i]->producer,
    //        *dev_src_result->rx_queue[i]->consumer);
    reserve = receive_pkt(buffer, receive_end, buffer_size - receive_end,
                          dev_src_result->rx_queue[i]);
    receive_end += reserve;
  }

  // if (reserve > 0) {
  //   printf("%s receive %d\n", dev_src_result->dev_info.dev_name, reserve);
  // }

  while (send_end < receive_end) {
    int sent = send_pkt(buffer, send_end, receive_end - send_end,
                        dev_dst_result->tx_queue[idx]);
    send_end += sent;
    idx++;
  }

  // printf("==============forward_once end\n");
}

void simple_forward(int fd, struct bind_dev_result *dev1_result,
                    struct bind_dev_result *dev2_result) {
  // Make sure valid result
  assert(dev1_result->rx_queue_num > 0 && dev1_result->rx_queue);
  assert(dev1_result->tx_queue_num > 0 && dev1_result->tx_queue);
  assert(dev2_result->rx_queue_num > 0 && dev2_result->rx_queue);
  assert(dev2_result->tx_queue_num > 0 && dev2_result->tx_queue);
  assert(dev1_result->rx_queue_num == dev2_result->rx_queue_num);
  assert(dev1_result->tx_queue_num == dev2_result->tx_queue_num);

  // Allocate buffer (no free, buffer will be free when progrom exit)
  u32 buffer_size = dev1_result->rx_queue[0]->nentries;
  buffer_size = buffer_size * dev1_result->rx_queue_num * 2;
  uint64_t *buffer = (uint64_t *)malloc(buffer_size);
  assert(buffer);
  printf("buffser size: %u\n", buffer_size);

  while (1) {
    forward_once(buffer, buffer_size, dev1_result, dev2_result);
    forward_once(buffer, buffer_size, dev2_result, dev1_result);
    ioctl(fd, IOCTL_SEND_ALL, 0);
  }
}

int main(int argc, char *argv[]) {
  if (argc != 3) {
    printf("Usage: %s <dev1_name> <dev2_name>\n", argv[0]);
    exit(EXIT_FAILURE);
  }

  int fd;
  fd = open("/dev/" DEVICE_NAME, O_RDWR);
  if (fd < 0) {
    perror("Failed to open device");
    exit(EXIT_FAILURE);
  }

  struct bind_dev_info dev1_info;
  struct bind_dev_info dev2_info;
  strcpy(dev1_info.dev_name, argv[1]);
  strcpy(dev2_info.dev_name, argv[2]);

  printf("bind dev1: %s\n", dev1_info.dev_name);
  printf("bind dev2: %s\n", dev2_info.dev_name);

  struct bind_dev_result dev1_result;
  struct bind_dev_result dev2_result;
  bind_dev(fd, &dev1_result, &dev1_info);
  bind_dev(fd, &dev2_result, &dev2_info);

  print_bind_dev_result(&dev1_result);
  print_bind_dev_result(&dev2_result);

  simple_forward(fd, &dev1_result, &dev2_result);

  return 0;
}