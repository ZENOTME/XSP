# XSP (Fast Skb Forwarding Kernel Module)

XSP is a kernel module that provides a fast method for forwarding skb buffers between virtual Ethernet (veth) interfaces on a local machine. It is primarily used in network emulation environments to efficiently forward packets between different network namespaces. User can use it to replace with similar tool like tap.

# How to use it

1. Bind device with device name
   The module creates fixed-size ring buffers for each CPU core. It then returns information about these ring buffers.

2. Memory map (mmap) RX and TX ring buffers
   The ring buffer information includes offsets for RX and TX ring buffers. Users can call `mmap` with these offsets to map the ring buffers into their address space.

3. Process incoming packets
   After mapping, users can receive incoming skb_buffers using the RX ring and push them into the TX ring of another device.

4. Trigger packet transmission
   Call the 'send' or 'send_all' ioctl to instruct the kernel to consume and transmit the skb_buffers in the TX ring buffer.

See simple_** in user for more detail example.

# TODO

