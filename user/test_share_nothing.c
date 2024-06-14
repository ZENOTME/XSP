#include "user_queue.h"
#include <fcntl.h>
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
#include <pthread.h>

#define DEVICE_NAME "/dev/emulator"
#define IOCTL_PING _IOR('N', 2, u64)
#define IOCTL_ATTACH_IF _IOR('N', 3, u64)
#define IOCTL_PING_ALL _IOR('N', 4, u64)

#define RX_RING_1_OFFSET 0
#define TX_RING_1_OFFSET 0x80000000
#define RX_RING_2_OFFSET 0x100000000ULL
#define TX_RING_2_OFFSET 0x180000000ULL

#define ENTRIES 4096
#define CORE_NUM 28
#define TX_NUM 10

struct ring_info {
  u64 rx_ring_size;
  u64 rx_ring_num;
  u64 tx_ring_size;
  u64 tx_ring_num;
};

struct attach_if {
  struct ring_info dev1_info;
  struct ring_info dev2_info;
};

struct forward_arg {
  struct xsk_ring_cons *cons_arr;
  int start;
  int len;
  struct xsk_ring_prod *prod;
  int fd;
  unsigned long send_index;
};

void simple_forward(struct xsk_ring_cons cons_arr[CORE_NUM], int start, int len,
                    struct xsk_ring_prod *prod, int fd,
                    unsigned long send_index) {
  uint32_t idx = 0;
  uint64_t buffer[4096];
  uint32_t buffer_idx = 0;
  // receive
  int avail_send = xsk_prod_nb_free(prod, 4096);
  struct xsk_ring_cons *cons = NULL;
  for (int i = start; i < start + len; i++) {
    cons = &cons_arr[i];
    int avail_receive = xsk_cons_nb_avail(cons, avail_send);
    if (avail_receive > 0) {
      int reserve = xsk_ring_cons__peek(cons, avail_receive, &idx);
      for (int i = 0; i < reserve; i++) {
        u64 addr = *xsk_ring_cons__comp_addr(cons, idx + i);
        buffer[buffer_idx++] = addr;
      }
      xsk_ring_cons__release(cons, reserve);
      avail_send -= reserve;
    }
    if (avail_send == 0) {
      break;
    }
  }
  // send
  if (buffer_idx > 0) {
    int reserve = xsk_ring_prod__reserve(prod, buffer_idx, &idx);
    if (reserve < buffer_idx) {
      printf("tx reserve < buffer idx\n");
      exit(-1);
    }
    for (int i = 0; i < buffer_idx; i++) {
      u64 addr = buffer[i];
      u64 *addr_send = xsk_ring_prod__fill_addr(prod, idx + i);
      *addr_send = addr;
    }
    xsk_ring_prod__submit(prod, reserve);
    if (ioctl(fd, IOCTL_PING,send_index) < 0) {
        perror("Failed to get interface index");
        close(fd);
        exit(EXIT_FAILURE);
    }
  }
}

void* thread_rx_func(void *arg) {
  struct forward_arg *farg = (struct forward_arg *)arg;
  int tx_id = farg->send_index & 0xFFFFFFFF;
  int tar = farg->send_index >> 32;
  printf("tar: %d tx_id: %d start: %d, len: %d\n", tar, tx_id,farg->start,farg->len);
  while(1) {
    simple_forward(farg->cons_arr, farg->start, farg->len, farg->prod, farg->fd, farg->send_index);
  }
}

struct single_arg {
  struct forward_arg* arg1;
  struct forward_arg* arg2;
};

struct attach_if attach;

int main(int argc, char *argv[]) {
  int fd;

  fd = open(DEVICE_NAME, O_RDWR);
  if (fd < 0) {
    perror("Failed to open device");
    exit(EXIT_FAILURE);
  }

  if (ioctl(fd, IOCTL_ATTACH_IF, &attach) < 0) {
    perror("Failed to attach interface");
    close(fd);
    exit(EXIT_FAILURE);
  } else {
    printf("dev1 rx_ring_size: %lld\n", attach.dev1_info.rx_ring_size);
    printf("dev1 rx_ring_num: %lld\n", attach.dev1_info.rx_ring_num);
    printf("dev1 tx_ring_size: %lld\n", attach.dev1_info.tx_ring_size);
    printf("dev1 tx_ring_num: %lld\n", attach.dev1_info.tx_ring_num);
    printf("dev2 rx_ring_size: %lld\n", attach.dev2_info.rx_ring_size);
    printf("dev2 rx_ring_num: %lld\n", attach.dev2_info.rx_ring_num);
    printf("dev2 tx_ring_size: %lld\n", attach.dev2_info.tx_ring_size);
    printf("dev2 tx_ring_num: %lld\n", attach.dev2_info.tx_ring_num);
  }

  // mmap the ring buffers
  struct xdp_umem_ring *tx_ring1[CORE_NUM];
  int ring_size = attach.dev1_info.tx_ring_size / attach.dev1_info.tx_ring_num;
  for (int i = 0; i < attach.dev1_info.tx_ring_num; i++) {
    tx_ring1[i] = (struct xdp_umem_ring *)mmap(
        NULL, ring_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd,
        TX_RING_1_OFFSET + i * 4096);
    if (tx_ring1[i] == MAP_FAILED) {
      perror("Failed to mmap rx ring 1");
      close(fd);
      exit(EXIT_FAILURE);
    } else {
      printf("tx_ring1[%d]: %d\n", i, tx_ring1[i]->ptrs.nentries);
    }
  }
  struct xdp_umem_ring *tx_ring2[CORE_NUM];
  ring_size = attach.dev2_info.tx_ring_size / attach.dev2_info.tx_ring_num;
  for (int i = 0; i < attach.dev2_info.tx_ring_num; i++) {
    tx_ring2[i] = (struct xdp_umem_ring *)mmap(
        NULL, ring_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd,
        TX_RING_2_OFFSET + i * 4096);
    if (tx_ring2[i] == MAP_FAILED) {
      perror("Failed to mmap rx ring 1");
      close(fd);
      exit(EXIT_FAILURE);
    } else {
      printf("tx_ring2[%d]: %d\n", i, tx_ring2[i]->ptrs.nentries);
    }
  }
  struct xdp_umem_ring *rx_ring1[CORE_NUM];
  ring_size = attach.dev1_info.rx_ring_size / attach.dev1_info.rx_ring_num;
  for (int i = 0; i < attach.dev1_info.rx_ring_num; i++) {
    rx_ring1[i] = (struct xdp_umem_ring *)mmap(
        NULL, ring_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd,
        RX_RING_1_OFFSET + i * 4096);
    if (rx_ring1[i] == MAP_FAILED) {
      perror("Failed to mmap rx ring 1");
      close(fd);
      exit(EXIT_FAILURE);
    } else {
      printf("rx_ring1[%d]: %d\n", i, rx_ring1[i]->ptrs.nentries);
    }
  }
  struct xdp_umem_ring *rx_ring2[CORE_NUM];
  ring_size = attach.dev2_info.rx_ring_size / attach.dev2_info.rx_ring_num;
  for (int i = 0; i < attach.dev2_info.rx_ring_num; i++) {
    rx_ring2[i] = (struct xdp_umem_ring *)mmap(
        NULL, ring_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd,
        RX_RING_2_OFFSET + i * 4096);
    if (rx_ring2[i] == MAP_FAILED) {
      perror("Failed to mmap rx ring 1");
      close(fd);
      exit(EXIT_FAILURE);
    } else {
      printf("rx_ring1[%d]: %d\n", i, rx_ring2[i]->ptrs.nentries);
    }
  }

  struct xsk_ring_prod tx_ring1_prod[CORE_NUM];
  struct xsk_ring_prod tx_ring2_prod[CORE_NUM];
  struct xsk_ring_cons rx_ring1_cons[CORE_NUM];
  struct xsk_ring_cons rx_ring2_cons[CORE_NUM];
  for (int i = 0; i < CORE_NUM; i++) {
    init_ring_prod(&tx_ring1_prod[i], tx_ring1[i]);
    init_ring_prod(&tx_ring2_prod[i], tx_ring2[i]);
    init_ring_cons(&rx_ring1_cons[i], rx_ring1[i]);
    init_ring_cons(&rx_ring2_cons[i], rx_ring2[i]);
  }

  pthread_t threads_1_2[TX_NUM];
  struct forward_arg farg_1_2[TX_NUM];
  int start = 0;
  int len = CORE_NUM / TX_NUM;
  int remain = CORE_NUM % TX_NUM;
  for (int i = 0; i < TX_NUM; i++) {
    farg_1_2[i].cons_arr = rx_ring1_cons;
    farg_1_2[i].start = start;
    farg_1_2[i].len = len + (i < remain ? 1 : 0);
    farg_1_2[i].prod = &tx_ring2_prod[i];
    farg_1_2[i].fd = fd;
    farg_1_2[i].send_index = ((unsigned long)1<<32) + i;
    pthread_create(&threads_1_2[i], NULL, thread_rx_func, &farg_1_2[i]);
    start += farg_1_2[i].len;
  }
  pthread_t threads_2_1[TX_NUM];
  struct forward_arg farg_2_1[TX_NUM];
  start = 0;
  len = CORE_NUM / TX_NUM;
  remain = CORE_NUM % TX_NUM;
  for (int i = 0; i < TX_NUM; i++) {
    farg_2_1[i].cons_arr = rx_ring2_cons;
    farg_2_1[i].start = start;
    farg_2_1[i].len = len + (i < remain ? 1 : 0);
    farg_2_1[i].prod = &tx_ring1_prod[i];
    farg_2_1[i].fd = fd;
    farg_2_1[i].send_index = ((unsigned long)0<<32) + i;
    pthread_create(&threads_2_1[i], NULL, thread_rx_func, &farg_2_1[i]);
    start += farg_2_1[i].len;
  }
  pthread_t threads_tx;

  // struct single_arg sarg;
  // sarg.arg1 = farg_1_2;
  // sarg.arg2 = farg_2_1;
  // pthread_t thread_single;
  // pthread_create(&thread_single, NULL, thread_func_single, &sarg);
  // pthread_join(thread_single, NULL);

  for (int i = 0; i < TX_NUM; i++) {
    pthread_join(threads_1_2[i], NULL);
    pthread_join(threads_2_1[i], NULL);
  }
  pthread_join(threads_tx, NULL);

  close(fd);

  return 0;
}
