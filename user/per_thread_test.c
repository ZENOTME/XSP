#include "../common_config.h"
#include "user_dev.h"
#include "user_queue.h"
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

// Test:
// [ packet thread * num ] -> forward thread
// <->
// forward thread <- [ packet thread * num ]

#define NUM_PACKET_THREAD 8

struct forward_task {
  int fd;
  struct xsp_queue **rx_queues;
  uint32_t rx_queue_size;
  struct xsp_queue *tx_queue;
  loff_t tx_index;
};

static inline void execute_forward_task(struct forward_task *task) {
  assert(task->rx_queue_size > 0 && task->rx_queues);
  assert(task->tx_queue);

  printf("Task\nrx_queue_size: %u\ntx_index: %lu\n", task->rx_queue_size,
         task->tx_index);

  // Allocate buffer (no free, buffer will be free when progrom exit)
  uint32_t buffer_size = task->rx_queues[0]->nentries * task->rx_queue_size * 2;
  uint64_t *buffer = (uint64_t *)malloc(buffer_size);
  assert(buffer);
  printf("buffser size: %u\n", buffer_size);

  struct xsp_queue *rx_queue = NULL;

  while (1) {
    uint64_t receive_end = 0;
    uint64_t send_end = 0;

    // receive
    int reserve = 0;
    for (int i = 0; i < task->rx_queue_size; i++) {
      rx_queue = task->rx_queues[i];
      reserve =
          receive_pkt(buffer, receive_end, buffer_size - receive_end, rx_queue);
      receive_end += reserve;
    }

    // send
    while (send_end < receive_end) {
      int sent =
          send_pkt(buffer, send_end, receive_end - send_end, task->tx_queue);
      send_end += sent;
      if (send_end < receive_end) {
        ioctl(task->fd, IOCTL_SEND, task->tx_index);
      }
    }
    ioctl(task->fd, IOCTL_SEND, task->tx_index);
  }
}

static void *thread_func(void *arg) {
  assert(arg);
  execute_forward_task(arg);
  return NULL;
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

  // partition task
  struct forward_task *dev1_tasks = (struct forward_task *)malloc(
      sizeof(struct forward_task) * NUM_PACKET_THREAD);
  assert(dev1_tasks);
  struct forward_task *dev2_tasks = (struct forward_task *)malloc(
      sizeof(struct forward_task) * NUM_PACKET_THREAD);
  assert(dev2_tasks);
  for (int i = 0; i < NUM_PACKET_THREAD; i++) {
    dev1_tasks[i].rx_queues = (struct xsp_queue **)malloc(
        sizeof(struct xsp_queue *) *
        (dev1_result.rx_queue_num / NUM_PACKET_THREAD + 1));
    assert(dev1_tasks[i].rx_queues);
    dev1_tasks[i].rx_queue_size = 0;
    assert(i < dev2_result.tx_queue_num && dev2_result.tx_queue[i]);
    dev1_tasks[i].tx_queue = dev2_result.tx_queue[i];
    dev1_tasks[i].tx_index =
        dev2_result.dev_info.tx_start_offset + i * dev2_result.dev_info.step;
    dev1_tasks[i].fd = fd;

    dev2_tasks[i].rx_queues = (struct xsp_queue **)malloc(
        sizeof(struct xsp_queue *) *
        (dev2_result.rx_queue_num / NUM_PACKET_THREAD + 1));
    assert(dev2_tasks[i].rx_queues);
    dev2_tasks[i].rx_queue_size = 0;
    assert(i < dev1_result.tx_queue_num && dev1_result.tx_queue[i]);
    dev2_tasks[i].tx_queue = dev1_result.tx_queue[i];
    dev2_tasks[i].tx_index =
        dev1_result.dev_info.tx_start_offset + i * dev1_result.dev_info.step;
    dev2_tasks[i].fd = fd;
  }
  for (int i = 0; i < dev1_result.rx_queue_num;) {
    for (int j = 0; j < NUM_PACKET_THREAD && i < dev2_result.rx_queue_num;
         j++) {
      dev1_tasks[j].rx_queues[dev1_tasks[j].rx_queue_size++] =
          dev1_result.rx_queue[i++];
    }
  }
  for (int i = 0; i < dev2_result.rx_queue_num;) {
    for (int j = 0; j < NUM_PACKET_THREAD && i < dev2_result.rx_queue_num;
         j++) {
      dev2_tasks[j].rx_queues[dev2_tasks[j].rx_queue_size++] =
          dev2_result.rx_queue[i++];
    }
  }

  pthread_t dev1_thread[NUM_PACKET_THREAD];
  pthread_t dev2_thread[NUM_PACKET_THREAD];
  for (int i = 0; i < NUM_PACKET_THREAD; i++) {
    pthread_create(&dev1_thread[i], NULL, thread_func, &dev1_tasks[i]);
    pthread_create(&dev2_thread[i], NULL, thread_func, &dev2_tasks[i]);
  }

  for (int i = 0; i < NUM_PACKET_THREAD; i++) {
    pthread_join(dev1_thread[i], NULL);
    pthread_join(dev2_thread[i], NULL);
  }

  return 0;
}
