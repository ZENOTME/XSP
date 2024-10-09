#define DEVICE_NAME "xsp"
#define CORE_NUM 28
#define IOCTL_BIND_DEV _IOW('x', 1, struct bind_dev_info)
#define IOCTL_SEND _IOW('x', 2, u64)
#define IOCTL_SEND_ALL _IOW('x', 4, u64)

struct bind_dev_info {
    // in argument
    char dev_name[256];
    // common out argument
    unsigned long step;
    // rx out argument
    unsigned long rx_start_offset;
    unsigned long rx_queue_num;
    unsigned long rx_queue_size;
    // tx out argument
    unsigned long tx_start_offset;
    unsigned long tx_queue_num;
    unsigned long tx_queue_size;
};
