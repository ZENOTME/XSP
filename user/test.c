#include "user_queue.h"
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>
#include <stddef.h>

#define DEVICE_NAME "/dev/emulator"
#define IOCTL_GET_IFINDEX _IOR('N', 1, char *)
#define IOCTL_PING _IOR('N', 2, u64)
#define IOCTL_START _IO('N', 3)
#define IFNAMSIZ 20

#define RX_RING_1_OFFSET 0
#define TX_RING_1_OFFSET 0x80000000
#define RX_RING_2_OFFSET 0x100000000ULL
#define TX_RING_2_OFFSET 0x180000000ULL


void simple_forward(struct xsk_ring_cons* cons, struct xsk_ring_prod* prod, int fd, int tar) {
  uint32_t idx = 0;
  static uint64_t buffer[4096];
  // receive
  int avail_receive = xsk_cons_nb_avail(cons, 4096);
  int avail_send = xsk_prod_nb_free(prod, 4096);
  // printf("rx cons: %d prod %d\n",*cons->consumer,*cons->producer);
  // printf("tx prod: %d cons %d\n",*prod->consumer,*prod->producer);
  // printf("avil: %d %d\n",avail_receive,avail_send);
  int ret = avail_receive < avail_send ? avail_receive : avail_send;
  if (ret > 0) {
    int reserve = xsk_ring_cons__peek(cons, ret, &idx);
    if (reserve!=ret) {
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

    int reserver = xsk_ring_prod__reserve(prod,ret,&idx);
     if (reserve!=ret) {
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
      printf("Call  ping\n");
    }
  } else {
    // printf("tar: %d empty\n",tar);
  }
}

int main(int argc, char *argv[]) {
  int fd;
  char ifname[IFNAMSIZ];
  int ifindex;

  if (argc != 2) {
    fprintf(stderr, "Usage: %s <interface_name>\n", argv[0]);
    exit(EXIT_FAILURE);
  }

  strncpy(ifname, argv[1], IFNAMSIZ - 1);
  ifname[IFNAMSIZ - 1] = '\0';

  fd = open(DEVICE_NAME, O_RDWR);
  if (fd < 0) {
    perror("Failed to open device");
    exit(EXIT_FAILURE);
  }

  // mmap the ring buffers
  struct xdp_umem_ring *tx_ring1 = (struct xdp_umem_ring *)mmap(
      NULL, sizeof(struct xdp_ring) + sizeof(uint64_t) * 32768,
      PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, TX_RING_1_OFFSET);
  if (tx_ring1 == MAP_FAILED) {
    perror("Failed to mmap tx ring 1");
    close(fd);
    exit(EXIT_FAILURE);
  } else {
    printf("tx_ring1: %d\n",tx_ring1->ptrs.flag);
  }
  struct xdp_umem_ring *rx_ring1 = (struct xdp_umem_ring *)mmap(
      NULL, sizeof(struct xdp_ring) + sizeof(uint64_t) * 32768,
      PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, RX_RING_1_OFFSET);
  if (rx_ring1 == MAP_FAILED) {
    perror("Failed to mmap tx ring 1");
    close(fd);
    exit(EXIT_FAILURE);
  } else {
    printf("rx_ring1: %d\n",rx_ring1->ptrs.flag);
  }
  struct xdp_umem_ring *tx_ring2 = (struct xdp_umem_ring *)mmap(
      NULL, sizeof(struct xdp_ring) + sizeof(uint64_t) * 32768,
      PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, TX_RING_2_OFFSET);
  if (tx_ring2 == MAP_FAILED) {
    perror("Failed to mmap tx ring 1");
    close(fd);
    exit(EXIT_FAILURE);
  } else {
    printf("tx_ring2: %d\n",tx_ring2->ptrs.flag);
  }
  struct xdp_umem_ring *rx_ring2 = (struct xdp_umem_ring *)mmap(
      NULL, sizeof(struct xdp_ring) + sizeof(uint64_t) * 32768,
      PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, RX_RING_2_OFFSET);
  if (rx_ring2 == MAP_FAILED) {
    perror("Failed to mmap tx ring 1");
    close(fd);
    exit(EXIT_FAILURE);
  } else {
    printf("rx_ring2: %d\n",rx_ring2->ptrs.flag);
  }

  struct xsk_ring_prod tx_ring1_prod;
  struct xsk_ring_prod tx_ring2_prod;
  struct xsk_ring_cons rx_ring1_cons;
  struct xsk_ring_cons rx_ring2_cons;
  init_ring_prod(&tx_ring1_prod, tx_ring1);
  init_ring_prod(&tx_ring2_prod, tx_ring2);
  init_ring_cons(&rx_ring1_cons, rx_ring1);
  init_ring_cons(&rx_ring2_cons, rx_ring2);

  // while(1) {
    // sleep(1);
    // // printf("rx_ring1 prod: %d cons: %d\n",rx_ring1->ptrs.producer,rx_ring1->ptrs.consumer);
    // // printf("rx_ring1 prod: %d cons: %d\n",*rx_ring1_cons.producer,*rx_ring1_cons.consumer);
    // simple_forward(&rx_ring1_cons, &tx_ring2_prod, fd, 1);
    // printf("rx_ring1 prod: %d cons: %d\n",rx_ring1->ptrs.producer,rx_ring1->ptrs.consumer);
    // simple_forward(&rx_ring2_cons, &tx_ring1_prod, fd, 0);
  // }

  while (1) {
    simple_forward(&rx_ring1_cons, &tx_ring2_prod, fd, 1);
    simple_forward(&rx_ring2_cons, &tx_ring1_prod, fd, 0);
  }

  close(fd);

  return 0;
}
