
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <linux/etherdevice.h>
#include <net/vxlan.h>

// 打印以太网头信息
void print_eth_header(struct ethhdr *eth) {
    printk(KERN_INFO "Ethernet Header:\n");
    printk(KERN_INFO "   |-Destination Address : %pM \n", eth->h_dest);
    printk(KERN_INFO "   |-Source Address      : %pM \n", eth->h_source);
    printk(KERN_INFO "   |-Protocol            : 0x%04x \n", ntohs(eth->h_proto));
}

// 打印 IP 头信息
void print_ip_header(struct iphdr *ip) {
    printk(KERN_INFO "IP Header:\n");
    printk(KERN_INFO "   |-IP Version        : %d\n", (unsigned int)ip->version);
    printk(KERN_INFO "   |-IP Header Length  : %d DWORDS or %d Bytes\n", (unsigned int)ip->ihl, ((unsigned int)(ip->ihl)) * 4);
    printk(KERN_INFO "   |-Type Of Service   : %d\n", (unsigned int)ip->tos);
    printk(KERN_INFO "   |-IP Total Length   : %d  Bytes(Size of Packet)\n", ntohs(ip->tot_len));
    printk(KERN_INFO "   |-Identification    : %d\n", ntohs(ip->id));
    printk(KERN_INFO "   |-TTL      : %d\n", (unsigned int)ip->ttl);
    printk(KERN_INFO "   |-Protocol : %d\n", (unsigned int)ip->protocol);
    printk(KERN_INFO "   |-Checksum : %d\n", ntohs(ip->check));
    printk(KERN_INFO "   |-Source IP        : %pI4\n", &ip->saddr);
    printk(KERN_INFO "   |-Destination IP   : %pI4\n", &ip->daddr);
}

// 打印 UDP 头信息
void print_udp_header(struct udphdr *udp) {
    printk(KERN_INFO "UDP Header:\n");
    printk(KERN_INFO "   |-Source Port      : %d\n", ntohs(udp->source));
    printk(KERN_INFO "   |-Destination Port : %d\n", ntohs(udp->dest));
    printk(KERN_INFO "   |-UDP Length       : %d\n", ntohs(udp->len));
    printk(KERN_INFO "   |-UDP Checksum     : %d\n", ntohs(udp->check));
}

// 打印 VXLAN 头信息
void print_vxlan_header(struct vxlanhdr *vxh) {
    printk(KERN_INFO "VXLAN Header:\n");
    printk(KERN_INFO "   |-VXLAN VNI : %d\n", ntohl(vxh->vx_vni) >> 8);
}

void print_skb(struct sk_buff *skb) {
    struct ethhdr *eth;
    struct iphdr *ip;
    struct udphdr *udp;
    struct vxlanhdr *vxh;

    printk(KERN_INFO "Packet size: %u bytes\n", skb->len);

    // 解析以太网头
    eth = eth_hdr(skb);
    if (!eth) {
        printk(KERN_INFO "No Ethernet header found.\n");
        return;
    }
    print_eth_header(eth);

    // 解析 IP 头
    if (eth->h_proto == htons(ETH_P_IP)) {
        ip = ip_hdr(skb);
        if (!ip) {
            printk(KERN_INFO "No IP header found.\n");
            return;
        }
        print_ip_header(ip);

        // 解析 UDP 头
        if (ip->protocol == IPPROTO_UDP) {
            udp = udp_hdr(skb);
            if (!udp) {
                printk(KERN_INFO "No UDP header found.\n");
                return;
            }
            print_udp_header(udp);

            // 解析 VXLAN 头
            vxh = (struct vxlanhdr *)(udp + 1);
            if (!vxh) {
                printk(KERN_INFO "No VXLAN header found.\n");
                return;
            }
            print_vxlan_header(vxh);
        }
    }
}