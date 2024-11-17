#include "../common_config.h"
#include "user_dev.h"
#include <fcntl.h>


static inline void forward_once(uint64_t *buffer, uint64_t buffer_size,
                                struct bind_dev_result *dev_src_result,
                                struct bind_dev_result *dev_dst_result) {
  uint64_t receive_end = 0;
  uint64_t send_end = 0;
  int idx = 0;
  int reserve = 0;

  for (uint64_t i = 0; i < dev_src_result->rx_queue_num; i++) {
    reserve = receive_pkt(buffer, receive_end, buffer_size - receive_end,
                          dev_src_result->rx_queue[i]);
    receive_end += reserve;
  }

  while (send_end < receive_end) {
    int sent = send_pkt(buffer, send_end, receive_end - send_end,
                        dev_dst_result->tx_queue[idx]);
    send_end += sent;
    idx++;
  }
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
  uint32_t buffer_size = dev1_result->rx_queue[0]->nentries;
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