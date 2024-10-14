#!/bin/bash

ACTION=$1

case $ACTION in
  up)
    # Create network namespace
    ip netns add ns1
    ip netns add ns2

    ip link add veth1 type veth peer name veth1-brr
    ip link add veth2 type veth peer name veth2-brr
    ip link set veth1 netns ns1
    ip link set veth2 netns ns2
    ip netns exec ns1 ip addr add 10.10.0.1/24 dev veth1
    ip netns exec ns1 ip link set veth1 up
    ip netns exec ns2 ip addr add 10.10.0.2/24 dev veth2
    ip netns exec ns2 ip link set veth2 up
    ip link set veth1-brr up
    ip link set veth2-brr up
    
    #sudo ip netns exec ns1 ethtool --offload veth1 rx off tx off
    #sudo ip netns exec ns2 ethtool --offload veth2 rx off tx off

    ip netns exec ns1 ip link set lo up
    ip netns exec ns2 ip link set lo up

    echo "Create test env successfully!"
    ;;
  
  down)
    ip link del veth1-brr
    ip link del veth2-brr

    ip netns del ns1
    ip netns del ns2

    echo "Delete test env successfully!"
    ;;
  
  *)
    echo "Usage: $0 {up|down}"
    exit 1
    ;;
esac

