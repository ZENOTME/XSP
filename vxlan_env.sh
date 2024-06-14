#!/bin/bash

ACTION=$1

CLIENT_IP=
SERVER_IP=
ETH_NAME=

case $ACTION in
  cup)
    # 创建vxlan
    ip link add vxlan-demo type vxlan id 5001 remote $SERVER_IP local $CLIENT_IP dev $ETH_NAME dstport 4789
    ip addr add 192.168.100.1/24 dev vxlan-demo
    ip link set up dev vxlan-demo

    ip netns add ns1
    ip link add veth1 type veth peer name veth1-br
    ip link set veth1 netns ns1
    ip netns exec ns1 ip addr add 10.0.0.1/24 dev veth1
    ip netns exec ns1 ip link set veth1 up
    ip netns exec ns1 ethtool --offload veth1 rx off tx off
    ip link set veth1-br up

    echo "网络命名空间和 veth 对已成功创建并连接到网桥。"
    ;;
  
  cdown)
    # 删除 veth 接口
    ip link del veth1-br
    ip netns del ns1
    ip link del vxlan-demo

    echo "网络命名空间、veth 对和网桥已成功删除。"
    ;;
  
  sup)
    # 创建vxlan
    ip link add vxlan-demo type vxlan id 5001 remote $CLIENT_IP local $SERVER_IP dev $ETH_NAME dstport 4789
    ip addr add
    ip link set up dev vxlan-demo

    ip netns add ns2
    ip link add veth2 type veth peer name veth2-br
    ip link set veth2 netns ns2
    ip netns exec ns2 ip addr add 10.0.0.2/24 dev veth2
    ip netns exec ns2 ip link set veth2 up
    ip netns exec ns2 ethtool --offload veth2 rx off tx off
    ip link set veth2-br up

    echo "网络命名空间和 veth 对已成功创建并连接到网桥。"
    ;;

  sdown)
    # 删除 veth 接口
    ip link del veth2-br
    ip netns del ns2
    ip link del vxlan-demo

    echo "网络命名空间、veth 对和网桥已成功删除。"
    ;;

  *)
    echo "用法: $0 {up|down}"
    exit 1
    ;;
esac

