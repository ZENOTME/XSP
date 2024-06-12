#!/bin/bash

ACTION=$1

case $ACTION in
  up)
    # 创建命名空间
    ip netns add ns1
    ip netns add ns2

    # 创建 veth 对
    ip link add veth1 type veth peer name veth1-br
    ip link add veth2 type veth peer name veth2-br

    # 将 veth 对的一端移动到命名空间
    ip link set veth1 netns ns1
    ip link set veth2 netns ns2

    # 设置命名空间内的 veth 接口
    ip netns exec ns1 ip addr add 10.0.0.1/24 dev veth1
    ip netns exec ns1 ip link set veth1 up

    ip netns exec ns2 ip addr add 10.0.0.2/24 dev veth2
    ip netns exec ns2 ip link set veth2 up
    
    # sudo ip netns exec ns1 ethtool --offload veth1 rx off tx off
    # sudo ip netns exec ns2 ethtool --offload veth2 rx off tx off

    # 创建网桥
    # ip link add name br0 type bridge
    # ip link set br0 up
    #
    # # 将 veth 设备添加到网桥
    # ip link set veth1-br master br0
    # ip link set veth2-br master br0
    ip link set veth1-br up
    ip link set veth2-br up

    # 启动命名空间的 loopback 接口
    ip netns exec ns1 ip link set lo up
    ip netns exec ns2 ip link set lo up

    echo "网络命名空间和 veth 对已成功创建并连接到网桥。"
    ;;
  
  down)
    # 删除 veth 接口
    ip link del veth1-br
    ip link del veth2-br

    # 删除网桥
    # ip link del br0

    # 删除命名空间
    ip netns del ns1
    ip netns del ns2

    echo "网络命名空间、veth 对和网桥已成功删除。"
    ;;
  
  *)
    echo "用法: $0 {up|down}"
    exit 1
    ;;
esac

