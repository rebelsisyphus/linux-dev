/*
 * qdisc_uaf_repro_bridge_gso.c
 *
 * Variant of the bridge reproducer that disables TSO on the egress veth
 * to force software GSO segmentation on the forwarded frame.  This tests
 * whether the current "unset transport_header" fix leaves the GSO path
 * without a valid transport header, which would make tcp4_gso_segment()
 * dereference the ~0U sentinel.
 *
 *   tun_get_user -> __netif_receive_skb_core -> br_handle_frame
 *   -> br_forward -> br_forward_finish -> __dev_queue_xmit
 *   -> validate_xmit_skb -> skb_gso_segment -> tcp4_gso_segment
 *
 * Topology:
 *   /dev/net/tun (syzq0) -> ip6gretap (gt1) -> bridge (br0) -> veth0 (TSO off)
 *
 * The frame is an IPv6/GRE/TEB packet whose Ethernet payload is a plain
 * broadcast IPv4 frame (ethertype 0x0800).  After GRE/TEB decap the skb
 * carries a stale transport_header left over from the outer IPv6/GRE/TCP
 * GSO metadata.  The bridge forwards the frame at L2; the forwarded clone
 * keeps the stale transport header.  With TSO disabled on the egress veth,
 * __dev_queue_xmit() calls validate_xmit_skb() -> skb_gso_segment(), which
 * dereferences the unset transport_header and crashes.
 *
 * This reproducer is intentionally built on top of the existing bridge
 * reproducer so it exercises the same decap/forward path.
 */

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <linux/if_ether.h>
#include <linux/if_link.h>
#include <linux/if_tun.h>
#include <linux/if_tunnel.h>
#include <linux/ipv6.h>
#include <linux/ip.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/tcp.h>
#include <linux/virtio_net.h>
#include <linux/ethtool.h>
#include <linux/sockios.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef NEXTHDR_DEST
#define NEXTHDR_DEST 60
#endif

#define TUN_IFNAME "syzq0"
#define GRETAP_IFNAME "gt1"
#define BR_IFNAME "br0"
#define BR_SLAVE1 "gt1"
#define BR_SLAVE2 "veth0"
#define PKT_LEN 1400
#define OUTER_DEST_OPTS_BYTES 256
#define IPV4_DF_FLAG 0x4000

#define TUN_LOCAL_IPV6 "2001:db8:1::1"
#define TUN_REMOTE_IPV6 "2001:db8:1::2"

/* Kernel feature bits are not exposed to userspace.  The positions below
 * match the current NETIF_F_* enum layout (NETIF_F_GSO_SHIFT == 16).
 */
#define R_TSO       (1ULL << 16)
#define R_TSO_ECN   (1ULL << 18)
#define R_TSO_MANGLEID (1ULL << 19)
#define R_TSO6      (1ULL << 20)
#define R_ALL_TSO   (R_TSO | R_TSO_ECN | R_TSO_MANGLEID | R_TSO6)

struct tun_pi_hdr {
	uint16_t flags;
	uint16_t proto;
} __attribute__((packed));

struct gre_base_hdr_uapi {
	uint16_t flags;
	uint16_t protocol;
} __attribute__((packed));

static void die(const char *msg)
{
	perror(msg);
	exit(EXIT_FAILURE);
}

static void run_cmd(const char *fmt, ...)
{
	char cmd[1024];
	va_list ap;
	int ret;

	va_start(ap, fmt);
	vsnprintf(cmd, sizeof(cmd), fmt, ap);
	va_end(ap);

	ret = system(cmd);
	if (ret != 0) {
		fprintf(stderr, "command failed (%d): %s\n", ret, cmd);
		exit(EXIT_FAILURE);
	}
}

/* ETHTOOL_DEV_FEATURE_WORDS for this kernel layout (NETDEV_FEATURE_COUNT = 64) */
#define R_SF_WORDS 2

static int disable_tso(const char *ifname)
{
	struct ethtool_sfeatures *sf;
	struct ifreq ifr;
	int fd, ret;
	size_t sf_size = sizeof(*sf) + R_SF_WORDS * sizeof(sf->features[0]);

	sf = calloc(1, sf_size);
	if (!sf)
		return -1;

	sf->cmd = ETHTOOL_SFEATURES;
	sf->size = R_SF_WORDS;
	sf->features[0].valid = R_ALL_TSO;
	sf->features[0].requested = 0;

	memset(&ifr, 0, sizeof(ifr));
	snprintf(ifr.ifr_name, IFNAMSIZ, "%s", ifname);
	ifr.ifr_data = (caddr_t)sf;

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		free(sf);
		return -1;
	}

	ret = ioctl(fd, SIOCETHTOOL, &ifr);
	if (ret < 0)
		perror("disable_tso(SIOCETHTOOL)");
	else
		printf("disabled TSO on %s\n", ifname);

	close(fd);
	free(sf);
	return ret;
}

	/* NETIF_F_GRO_BIT is 14 */
#define R_GRO (1ULL << 14)

static int disable_gro(const char *ifname)
{
	struct ethtool_sfeatures *sf;
	struct ifreq ifr;
	int fd, ret;
	size_t sf_size = sizeof(*sf) + R_SF_WORDS * sizeof(sf->features[0]);

	sf = calloc(1, sf_size);
	if (!sf)
		return -1;

	sf->cmd = ETHTOOL_SFEATURES;
	sf->size = R_SF_WORDS;
	sf->features[0].valid = R_GRO;
	sf->features[0].requested = 0;

	memset(&ifr, 0, sizeof(ifr));
	snprintf(ifr.ifr_name, IFNAMSIZ, "%s", ifname);
	ifr.ifr_data = (caddr_t)sf;

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		free(sf);
		return -1;
	}

	ret = ioctl(fd, SIOCETHTOOL, &ifr);
	if (ret < 0)
		perror("disable_gro(SIOCETHTOOL)");
	else
		printf("disabled GRO on %s\n", ifname);

	close(fd);
	free(sf);
	return ret;
}

static void set_link_up(const char *ifname)
{
	run_cmd("ip link set %s up", ifname);
}

static int open_tun(const char *ifname)
{
	struct ifreq ifr;
	int fd;

	fd = open("/dev/net/tun", O_RDWR);
	if (fd < 0)
		die("open(/dev/net/tun)");

	memset(&ifr, 0, sizeof(ifr));
	snprintf(ifr.ifr_name, IFNAMSIZ, "%s", ifname);
	ifr.ifr_flags = IFF_TUN | IFF_VNET_HDR | IFF_TUN_EXCL;
	if (ioctl(fd, TUNSETIFF, &ifr) < 0)
		die("ioctl(TUNSETIFF)");

	return fd;
}

static int setup_topology(void)
{
	int tun_fd;

	tun_fd = open_tun(TUN_IFNAME);

	run_cmd("ip -6 addr flush dev %s 2>/dev/null || true", TUN_IFNAME);
	run_cmd("ip -6 addr replace %s/64 dev %s nodad",
		TUN_LOCAL_IPV6, TUN_IFNAME);
	run_cmd("ip link set %s mtu 1500", TUN_IFNAME);
	set_link_up(TUN_IFNAME);

	run_cmd("ip link del %s >/dev/null 2>&1 || true", BR_IFNAME);
	run_cmd("ip link del %s >/dev/null 2>&1 || true", GRETAP_IFNAME);
	run_cmd("ip link del %s >/dev/null 2>&1 || true", BR_SLAVE2);

	run_cmd("ip link add %s type ip6gretap local %s remote %s dev %s ttl 64 encaplimit 4",
		GRETAP_IFNAME, TUN_LOCAL_IPV6, TUN_REMOTE_IPV6, TUN_IFNAME);
	run_cmd("ip link set %s mtu 1500", GRETAP_IFNAME);
	set_link_up(GRETAP_IFNAME);

	run_cmd("ip link add %s type veth peer name %s", BR_SLAVE2, "veth1");
	set_link_up(BR_SLAVE2);
	set_link_up("veth1");

	run_cmd("modprobe -r br_netfilter 2>/dev/null || true");

	if (disable_tso(BR_SLAVE2) < 0) {
		fprintf(stderr, "warning: failed to disable TSO on %s, "
			"the test may not force software GSO\n", BR_SLAVE2);
	}
	if (disable_gro(GRETAP_IFNAME) < 0) {
		fprintf(stderr, "warning: failed to disable GRO on %s, "
			"the transport header may be set by GRO\n", GRETAP_IFNAME);
	}

	run_cmd("ip link add %s type bridge nf_call_iptables 0 nf_call_ip6tables 0 nf_call_arptables 0 forward_delay 0 stp_state 0 mcast_snooping 0", BR_IFNAME);
	run_cmd("ip link set dev %s master %s", GRETAP_IFNAME, BR_IFNAME);
	run_cmd("ip link set dev %s master %s", BR_SLAVE2, BR_IFNAME);
	set_link_up(BR_IFNAME);

	return tun_fd;
}

static size_t build_inner_packet(uint8_t *buf, size_t pkt_len)
{
	struct ipv6hdr *outer6 = (struct ipv6hdr *)buf;
	uint8_t *outer_opts = buf + sizeof(*outer6);
	struct gre_base_hdr_uapi *gre;
	uint8_t *inner_payload;
	size_t inner_payload_len;
	size_t outer_payload_len;

	if (pkt_len < sizeof(*outer6) + OUTER_DEST_OPTS_BYTES + sizeof(*gre) +
	    sizeof(struct ethhdr) + 64) {
		fprintf(stderr, "packet length too small: %zu\n", pkt_len);
		exit(EXIT_FAILURE);
	}

	memset(buf, 0, pkt_len);

	gre = (struct gre_base_hdr_uapi *)(outer_opts + OUTER_DEST_OPTS_BYTES);
	inner_payload = (uint8_t *)gre + sizeof(*gre) + sizeof(struct ethhdr);

	outer_payload_len = pkt_len - sizeof(*outer6);
	inner_payload_len = pkt_len - sizeof(*outer6) - OUTER_DEST_OPTS_BYTES -
		    sizeof(*gre) - sizeof(struct ethhdr);

	outer6->version = 6;
	outer6->flow_lbl[2] = 0x01;
	outer6->payload_len = htons((uint16_t)outer_payload_len);
	outer6->nexthdr = NEXTHDR_DEST;
	outer6->hop_limit = 64;

	if (inet_pton(AF_INET6, TUN_REMOTE_IPV6, &outer6->saddr) != 1 ||
	    inet_pton(AF_INET6, TUN_LOCAL_IPV6, &outer6->daddr) != 1) {
		fprintf(stderr, "inet_pton(AF_INET6) failed\n");
		exit(EXIT_FAILURE);
	}

	outer_opts[0] = IPPROTO_GRE;
	outer_opts[1] = (uint8_t)((OUTER_DEST_OPTS_BYTES >> 3) - 1);
	outer_opts[2] = 0x1e;
	outer_opts[3] = (uint8_t)(OUTER_DEST_OPTS_BYTES - 4);

	gre->flags = 0;
	gre->protocol = htons(ETH_P_TEB);

	{
		struct ethhdr *eth = (struct ethhdr *)((uint8_t *)gre + sizeof(*gre));
		uint8_t src[ETH_ALEN] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };
		uint8_t dst[ETH_ALEN] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
		memcpy(eth->h_source, src, ETH_ALEN);
		memcpy(eth->h_dest, dst, ETH_ALEN);
		eth->h_proto = htons(ETH_P_IP); /* normal IP ethertype; bridge forwarding still keeps stale transport_header */
	}

	for (size_t i = 0; i < inner_payload_len; i++)
		inner_payload[i] = (uint8_t)(i & 0xff);

	return pkt_len;
}

static size_t build_write_buffer(uint8_t **out)
{
	struct tun_pi_hdr *pi;
	struct virtio_net_hdr *vhdr;
	uint8_t *buf;
	size_t total_len = sizeof(*pi) + sizeof(*vhdr) + PKT_LEN;

	buf = calloc(1, total_len);
	if (!buf)
		die("calloc");

	pi = (struct tun_pi_hdr *)buf;
	vhdr = (struct virtio_net_hdr *)(buf + sizeof(*pi));

	pi->flags = 0;
	pi->proto = htons(ETH_P_IPV6);

	vhdr->flags = 0;
	vhdr->gso_type = VIRTIO_NET_HDR_GSO_TCPV4;
	vhdr->hdr_len = htole16(17);
	vhdr->gso_size = htole16(4);
	vhdr->csum_start = htole16(0);
	vhdr->csum_offset = htole16(0x0ca6);

	build_inner_packet(buf + sizeof(*pi) + sizeof(*vhdr), PKT_LEN);

	*out = buf;
	return total_len;
}

int main(void)
{
	uint8_t *buf = NULL;
	size_t total_len;
	int tun_fd;

	if (getuid() != 0) {
		fprintf(stderr, "this reproducer must run as root\n");
		return EXIT_FAILURE;
	}

	tun_fd = setup_topology();
	total_len = build_write_buffer(&buf);

	printf("packet_len: %d\n", PKT_LEN);
	printf("write_len: %zu\n", total_len);
	printf("topology: tun(%s) -> ip6gretap(%s) -> bridge(%s) -> %s\n",
	       TUN_IFNAME, GRETAP_IFNAME, BR_IFNAME, BR_SLAVE2);
	printf("outer: IPv6/dst-opts(%d)/GRE(TEB)\n", OUTER_DEST_OPTS_BYTES);
	printf("inner: Ethernet(broadcast)/IPv4 (0x0800)\n");
	printf("sending via /dev/net/tun bound to %s...\n", TUN_IFNAME);

	for (int i = 0; i < 10; i++) {
		if (write(tun_fd, buf, total_len) != (ssize_t)total_len)
			die("write tun");
		printf("write[%d] ok\n", i);
	}

	run_cmd("ip -s link show %s", GRETAP_IFNAME);
	run_cmd("ip -s link show %s", BR_SLAVE2);

	printf("linger: 5 seconds\n");
	sleep(5);

	free(buf);
	close(tun_fd);
	return 0;
}
