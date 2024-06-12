#include "user_queue.h"
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#define DEVICE_NAME "/dev/emulator"
#define IOCTL_PING _IOR('N', 2, u64)
#define IOCTL_ATTACH_IF _IOR('N', 3, u64)

#define RX_RING_1_OFFSET 0
#define TX_RING_1_OFFSET 0x80000000
#define RX_RING_2_OFFSET 0x100000000ULL
#define TX_RING_2_OFFSET 0x180000000ULL

#define ENTRIES 4096
#define CORE_NUM 28

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

void simple_forward(struct xsk_ring_cons cons_arr[CORE_NUM],
                    struct xsk_ring_prod *prod, int fd, int tar) {
  uint32_t idx = 0;
  static uint64_t buffer[4096];
  // receive
  struct xsk_ring_cons *cons = NULL;
  for (int i = 0; i < CORE_NUM; i++) {
    cons = &cons_arr[i];
    int avail_receive = xsk_cons_nb_avail(cons, 4096);
    int avail_send = xsk_prod_nb_free(prod, 4096);
    // printf("rx cons: %d prod %d\n",*cons->consumer,*cons->producer);
    // printf("tx prod: %d cons %d\n",*prod->consumer,*prod->producer);
    // printf("avil: %d %d\n",avail_receive,avail_send);
    int ret = avail_receive < avail_send ? avail_receive : avail_send;
    if (ret > 0) {
      int reserve = xsk_ring_cons__peek(cons, ret, &idx);
      if (reserve != ret) {
        printf("reserver!=ret\n");
        exit(-1);
      }
      for (int i = 0; i < ret; i++) {
        u64 addr = *xsk_ring_cons__comp_addr(cons, idx + i);
        // while (addr == 0) {
        //   addr = *xsk_ring_cons__comp_addr(cons, idx + i);
        // }
        buffer[i] = addr;
        // printf("tar: %d receive %lld\n",tar, addr);
      }
      xsk_ring_cons__release(cons, ret);

      int reserver = xsk_ring_prod__reserve(prod, ret, &idx);
      if (reserve != ret) {
        printf("tx reserver!=ret\n");
        exit(-1);
      }
      for (int i = 0; i < ret; i++) {
        u64 addr = buffer[i];
        u64 *addr_send = xsk_ring_prod__fill_addr(prod, idx + i);
        // printf("tar: %d send %lld\n",tar,addr);
        *addr_send = addr;
      }
      xsk_ring_prod__submit(prod, ret);

      if (ioctl(fd, IOCTL_PING, tar) < 0) {
        perror("Failed to get interface index");
        close(fd);
        exit(EXIT_FAILURE);
      } else {
        // printf("Call  ping\n");
      }
    } else {
      // printf("tar: %d empty\n",tar);
    }
  }
}

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
  struct xdp_umem_ring *tx_ring1 = (struct xdp_umem_ring *)mmap(
      NULL, attach.dev1_info.tx_ring_size, PROT_READ | PROT_WRITE,
      MAP_SHARED | MAP_POPULATE, fd, TX_RING_1_OFFSET);
  if (tx_ring1 == MAP_FAILED) {
    perror("Failed to mmap tx ring 1");
    close(fd);
    exit(EXIT_FAILURE);
  } else {
    printf("tx_ring1: %d\n", tx_ring1->ptrs.nentries);
  }
  struct xdp_umem_ring *tx_ring2 = (struct xdp_umem_ring *)mmap(
      NULL, attach.dev2_info.tx_ring_size, PROT_READ | PROT_WRITE,
      MAP_SHARED | MAP_POPULATE, fd, TX_RING_2_OFFSET);
  if (tx_ring2 == MAP_FAILED) {
    perror("Failed to mmap tx ring 1");
    close(fd);
    exit(EXIT_FAILURE);
  } else {
    printf("tx_ring2: %d\n", tx_ring2->ptrs.nentries);
  }
  struct xdp_umem_ring *rx_ring1[CORE_NUM];
  int ring_size = attach.dev1_info.rx_ring_size / attach.dev1_info.rx_ring_num;
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

  struct xsk_ring_prod tx_ring1_prod;
  struct xsk_ring_prod tx_ring2_prod;
  init_ring_prod(&tx_ring1_prod, tx_ring1);
  init_ring_prod(&tx_ring2_prod, tx_ring2);
  struct xsk_ring_cons rx_ring1_cons[CORE_NUM];
  struct xsk_ring_cons rx_ring2_cons[CORE_NUM];
  for (int i = 0; i < CORE_NUM; i++) {
    init_ring_cons(&rx_ring1_cons[i], rx_ring1[i]);
    init_ring_cons(&rx_ring2_cons[i], rx_ring2[i]);
  }

  /*

  // while(1) {
    // sleep(1);
    // // printf("rx_ring1 prod: %d cons:
  %d\n",rx_ring1->ptrs.producer,rx_ring1->ptrs.consumer);
    // // printf("rx_ring1 prod: %d cons:
  %d\n",*rx_ring1_cons.producer,*rx_ring1_cons.consumer);
    // simple_forward(&rx_ring1_cons, &tx_ring2_prod, fd, 1);
    // printf("rx_ring1 prod: %d cons:
  %d\n",rx_ring1->ptrs.producer,rx_ring1->ptrs.consumer);
    // simple_forward(&rx_ring2_cons, &tx_ring1_prod, fd, 0);
  // }
  */
  while (1) {
    simple_forward(rx_ring1_cons, &tx_ring2_prod, fd, 1);
    simple_forward(rx_ring2_cons, &tx_ring1_prod, fd, 0);
  }
  

  close(fd);

  return 0;
}
