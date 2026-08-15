#define _GNU_SOURCE

#include <arpa/inet.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/if.h>
#include <linux/if_ether.h>
#include <linux/if_link.h>
#include <linux/if_tun.h>
#include <linux/if_tunnel.h>
#include <linux/ipv6.h>
#include <linux/ip.h>
#include <linux/netlink.h>
#include <linux/pkt_sched.h>
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

#ifndef TCA_KIND
#define TCA_KIND 1
#endif

#ifndef NEXTHDR_DEST
#define NEXTHDR_DEST 60
#endif

#ifndef IFLA_HSR_SLAVE1
#define IFLA_HSR_SLAVE1 1
#endif
#ifndef IFLA_HSR_SLAVE2
#define IFLA_HSR_SLAVE2 2
#endif
#ifndef IFLA_HSR_INTERLINK
#define IFLA_HSR_INTERLINK 8
#endif

#define REPRO_IFNAME "syzq0"
#define REPRO_GRETAP_IFNAME "gt1"
#define REPRO_HSR_IFNAME "hsr0"
#define REPRO_HSR_SLAVE_IFNAME "hsrdummy0"
#define REPRO_PKT_LEN 1400
#define REPRO_WRITES 1
#define REPRO_LINGER_SECS 5
#define OUTER_DEST_OPTS_BYTES 256
#define IPV4_DF_FLAG 0x4000

#define TUN_LOCAL_IPV6 "2001:db8:1::1"
#define TUN_REMOTE_IPV6 "2001:db8:1::2"
#define GRETAP_LOCAL_IPV6 "2001:db8:1::3"
#define GRETAP_REMOTE_IPV6 "2001:db8:1::4"

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
	struct sockaddr_nl nladdr = {
		.nl_family = AF_NETLINK,
	};
	char buf[4096];
	struct iovec iov = {
		.iov_base = nlh,
		.iov_len = nlh->nlmsg_len,
	};
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

static void add_ingress_qdisc(int ifindex)
{
	char reqbuf[512];
	struct nlmsghdr *nlh = (struct nlmsghdr *)reqbuf;
	struct tcmsg *tcm;
	int fd;

	memset(reqbuf, 0, sizeof(reqbuf));
	nlh->nlmsg_len = NLMSG_LENGTH(sizeof(*tcm));
	nlh->nlmsg_type = RTM_NEWQDISC;
	nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE | NLM_F_EXCL;
	tcm = NLMSG_DATA(nlh);
	tcm->tcm_family = AF_UNSPEC;
	tcm->tcm_ifindex = ifindex;
	tcm->tcm_parent = TC_H_INGRESS;
	tcm->tcm_handle = TC_H_MAKE(TC_H_INGRESS, 0);
	addattr_l(nlh, sizeof(reqbuf), TCA_KIND, "ingress",
		  strlen("ingress") + 1);

	fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	if (fd < 0)
		die("socket(NETLINK_ROUTE)");

	if (nl_talk(fd, nlh) < 0 && errno != EEXIST)
		die("RTM_NEWQDISC ingress");

	close(fd);
}

static void set_link_mtu_up(const char *ifname, int mtu)
{
	struct ifreq ifr;
	int fd;

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
		die("socket(AF_INET)");

	memset(&ifr, 0, sizeof(ifr));
	snprintf(ifr.ifr_name, IFNAMSIZ, "%s", ifname);
	ifr.ifr_mtu = mtu;
	if (ioctl(fd, SIOCSIFMTU, &ifr) < 0)
		die("ioctl(SIOCSIFMTU)");

	memset(&ifr, 0, sizeof(ifr));
	snprintf(ifr.ifr_name, IFNAMSIZ, "%s", ifname);
	if (ioctl(fd, SIOCGIFFLAGS, &ifr) < 0)
		die("ioctl(SIOCGIFFLAGS)");
	ifr.ifr_flags |= IFF_UP;
	if (ioctl(fd, SIOCSIFFLAGS, &ifr) < 0)
		die("ioctl(SIOCSIFFLAGS)");

	close(fd);
}

static int get_ifindex(const char *ifname)
{
	struct ifreq ifr;
	int fd;

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
		die("socket(AF_INET)");

	memset(&ifr, 0, sizeof(ifr));
	snprintf(ifr.ifr_name, IFNAMSIZ, "%s", ifname);
	if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0)
		die("ioctl(SIOCGIFINDEX)");

	close(fd);
	return ifr.ifr_ifindex;
}

static void add_ip6gretap_link(const char *ifname, int link_ifindex)
{
	char reqbuf[1024];
	struct nlmsghdr *nlh = (struct nlmsghdr *)reqbuf;
	struct ifinfomsg *ifm;
	struct rtattr *linkinfo, *infodata;
	struct in6_addr local, remote;
	int fd;
	uint8_t ttl = 64, encap_limit = 4;

	if (inet_pton(AF_INET6, TUN_LOCAL_IPV6, &local) != 1 ||
	    inet_pton(AF_INET6, TUN_REMOTE_IPV6, &remote) != 1) {
		fprintf(stderr, "inet_pton(AF_INET6) failed for ip6gretap\n");
		exit(EXIT_FAILURE);
	}

	memset(reqbuf, 0, sizeof(reqbuf));
	nlh->nlmsg_len = NLMSG_LENGTH(sizeof(*ifm));
	nlh->nlmsg_type = RTM_NEWLINK;
	nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE | NLM_F_EXCL;
	ifm = NLMSG_DATA(nlh);
	ifm->ifi_family = AF_UNSPEC;

	addattr_l(nlh, sizeof(reqbuf), IFLA_IFNAME, ifname, strlen(ifname) + 1);
	linkinfo = addattr_nest(nlh, sizeof(reqbuf), IFLA_LINKINFO);
	addattr_l(nlh, sizeof(reqbuf), IFLA_INFO_KIND, "ip6gretap",
		  strlen("ip6gretap") + 1);
	infodata = addattr_nest(nlh, sizeof(reqbuf), IFLA_INFO_DATA);
	addattr_l(nlh, sizeof(reqbuf), IFLA_GRE_LINK, &link_ifindex,
		  sizeof(link_ifindex));
	addattr_l(nlh, sizeof(reqbuf), IFLA_GRE_LOCAL, &local, sizeof(local));
	addattr_l(nlh, sizeof(reqbuf), IFLA_GRE_REMOTE, &remote, sizeof(remote));
	addattr_l(nlh, sizeof(reqbuf), IFLA_GRE_TTL, &ttl, sizeof(ttl));
	addattr_l(nlh, sizeof(reqbuf), IFLA_GRE_ENCAP_LIMIT,
		  &encap_limit, sizeof(encap_limit));
	addattr_nest_end(nlh, infodata);
	addattr_nest_end(nlh, linkinfo);

	fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	if (fd < 0)
		die("socket(NETLINK_ROUTE)");

	if (nl_talk(fd, nlh) < 0 && errno != EEXIST)
		die("RTM_NEWLINK ip6gretap");

	close(fd);
}

static void write_sysctl(const char *path, const char *value)
{
	int fd = open(path, O_WRONLY);
	if (fd < 0)
		return;
	if (write(fd, value, strlen(value)) < 0) {
		close(fd);
		return;
	}
	close(fd);
}

static void setup_ip6gretap(const char *underlay_ifname, const char *tunnel_ifname,
			    int mtu)
{
	char sysctl_path[128];

	snprintf(sysctl_path, sizeof(sysctl_path),
		 "/proc/sys/net/ipv6/conf/%s/accept_dad", underlay_ifname);
	write_sysctl(sysctl_path, "0");
	snprintf(sysctl_path, sizeof(sysctl_path),
		 "/proc/sys/net/ipv6/conf/%s/dad_transmits", underlay_ifname);
	write_sysctl(sysctl_path, "0");

	run_cmd("ip -6 addr flush dev %s 2>/dev/null || true", underlay_ifname);
	run_cmd("ip -6 addr replace %s/64 dev %s nodad",
		TUN_LOCAL_IPV6, underlay_ifname);
	run_cmd("ip -6 addr replace %s/64 dev %s nodad",
		GRETAP_LOCAL_IPV6, underlay_ifname);
	run_cmd("ip link del %s >/dev/null 2>&1 || true", tunnel_ifname);
	run_cmd("ip link add %s type ip6gretap local %s remote %s dev %s ttl 64 encaplimit 4",
		tunnel_ifname, GRETAP_LOCAL_IPV6, GRETAP_REMOTE_IPV6, underlay_ifname);
	run_cmd("ip -d link show %s", tunnel_ifname);
	run_cmd("ip -6 addr show dev %s", underlay_ifname);

	snprintf(sysctl_path, sizeof(sysctl_path),
		 "/proc/sys/net/ipv6/conf/%s/accept_dad", tunnel_ifname);
	write_sysctl(sysctl_path, "0");
	snprintf(sysctl_path, sizeof(sysctl_path),
		 "/proc/sys/net/ipv6/conf/%s/dad_transmits", tunnel_ifname);
	write_sysctl(sysctl_path, "0");

	set_link_mtu_up(tunnel_ifname, mtu);
}

static void add_dummy_link(const char *ifname)
{
	char reqbuf[512];
	struct nlmsghdr *nlh = (struct nlmsghdr *)reqbuf;
	struct ifinfomsg *ifm;
	struct rtattr *linkinfo;
	int fd;

	memset(reqbuf, 0, sizeof(reqbuf));
	nlh->nlmsg_len = NLMSG_LENGTH(sizeof(*ifm));
	nlh->nlmsg_type = RTM_NEWLINK;
	nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE | NLM_F_EXCL;
	ifm = NLMSG_DATA(nlh);
	ifm->ifi_family = AF_UNSPEC;

	addattr_l(nlh, sizeof(reqbuf), IFLA_IFNAME, ifname, strlen(ifname) + 1);
	linkinfo = addattr_nest(nlh, sizeof(reqbuf), IFLA_LINKINFO);
	addattr_l(nlh, sizeof(reqbuf), IFLA_INFO_KIND, "dummy",
		  strlen("dummy") + 1);
	addattr_nest_end(nlh, linkinfo);

	fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	if (fd < 0)
		die("socket(NETLINK_ROUTE)");

	if (nl_talk(fd, nlh) < 0 && errno != EEXIST)
		die("RTM_NEWLINK dummy");

	close(fd);
}

static void add_hsr_link(const char *ifname, int slave1_ifindex,
			 int slave2_ifindex, int interlink_ifindex)
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
	addattr_l(nlh, sizeof(reqbuf), IFLA_HSR_INTERLINK, &interlink_ifindex,
		  sizeof(interlink_ifindex));
	addattr_nest_end(nlh, infodata);
	addattr_nest_end(nlh, linkinfo);

	fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	if (fd < 0)
		die("socket(NETLINK_ROUTE)");

	if (nl_talk(fd, nlh) < 0 && errno != EEXIST)
		die("RTM_NEWLINK hsr");

	close(fd);
}

static void setup_hsr(const char *interlink_ifname,
		      const char *slave1_ifname,
		      const char *slave2_ifname,
		      const char *hsr_ifname)
{
	int interlink_ifindex = get_ifindex(interlink_ifname);
	int slave1_ifindex, slave2_ifindex;

	run_cmd("ip link del %s >/dev/null 2>&1 || true", hsr_ifname);
	run_cmd("ip link del %s >/dev/null 2>&1 || true", slave1_ifname);
	run_cmd("ip link del %s >/dev/null 2>&1 || true", slave2_ifname);
	add_dummy_link(slave1_ifname);
	add_dummy_link(slave2_ifname);
	set_link_mtu_up(slave1_ifname, 1500);
	set_link_mtu_up(slave2_ifname, 1500);
	slave1_ifindex = get_ifindex(slave1_ifname);
	slave2_ifindex = get_ifindex(slave2_ifname);
	add_hsr_link(hsr_ifname, slave1_ifindex, slave2_ifindex,
		     interlink_ifindex);
	set_link_mtu_up(hsr_ifname, 1494);
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

static size_t build_packet(uint8_t *buf, size_t pkt_len)
{
	struct ipv6hdr *outer6 = (struct ipv6hdr *)buf;
	uint8_t *outer_opts = buf + sizeof(*outer6);
	struct gre_base_hdr_uapi *gre;
	struct iphdr *inner4;
	struct tcphdr *tcp;
	uint8_t *payload;
	size_t inner_payload_len;
	size_t outer_payload_len;
	size_t l2_len = sizeof(struct ethhdr);

	if (pkt_len < sizeof(*outer6) + OUTER_DEST_OPTS_BYTES + sizeof(*gre) +
	    l2_len + sizeof(*inner4) + sizeof(*tcp) + 64) {
		fprintf(stderr, "packet length too small: %zu\n", pkt_len);
		exit(EXIT_FAILURE);
	}

	memset(buf, 0, pkt_len);

	gre = (struct gre_base_hdr_uapi *)(outer_opts + OUTER_DEST_OPTS_BYTES);
	inner4 = (struct iphdr *)((uint8_t *)gre + sizeof(*gre) + l2_len);
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

	if (inet_pton(AF_INET6, GRETAP_REMOTE_IPV6, &outer6->saddr) != 1 ||
	    inet_pton(AF_INET6, GRETAP_LOCAL_IPV6, &outer6->daddr) != 1) {
		fprintf(stderr, "inet_pton(AF_INET6) failed\n");
		exit(EXIT_FAILURE);
	}

	outer_opts[0] = IPPROTO_GRE;
	outer_opts[1] = (uint8_t)((OUTER_DEST_OPTS_BYTES >> 3) - 1);
	outer_opts[2] = 0x1e;
	outer_opts[3] = (uint8_t)(OUTER_DEST_OPTS_BYTES - 4);

	gre->flags = 0;
	gre->protocol = htons(ETH_P_TEB);

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
	size_t total_len = sizeof(*pi) + sizeof(*vhdr) + REPRO_PKT_LEN;

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

	build_packet(buf + sizeof(*pi) + sizeof(*vhdr), REPRO_PKT_LEN);

	*out = buf;
	return total_len;
}

int main(void)
{
	const char *ifname = REPRO_IFNAME;
	const char *qdisc_ifname = REPRO_IFNAME;
	int tun_fd;
	int mtu = 1500;
	size_t total_len;
	uint8_t *buf = NULL;
	struct ifreq ifr;
	ssize_t ret;

	if (getuid() != 0) {
		fprintf(stderr, "this reproducer must run as root\n");
		return EXIT_FAILURE;
	}

	tun_fd = open("/dev/net/tun", O_RDWR);
	if (tun_fd < 0)
		die("open(/dev/net/tun)");

	memset(&ifr, 0, sizeof(ifr));
	snprintf(ifr.ifr_name, IFNAMSIZ, "%s", ifname);
	ifr.ifr_flags = IFF_TUN | IFF_VNET_HDR | IFF_TUN_EXCL;
	if (ioctl(tun_fd, TUNSETIFF, &ifr) < 0)
		die("ioctl(TUNSETIFF)");
	ifname = ifr.ifr_name;

	set_link_mtu_up(ifname, mtu);
	setup_ip6gretap(ifname, REPRO_GRETAP_IFNAME, mtu);
	setup_hsr(REPRO_GRETAP_IFNAME, REPRO_HSR_SLAVE_IFNAME "1",
		  REPRO_HSR_SLAVE_IFNAME "2", REPRO_HSR_IFNAME);

	total_len = build_write_buffer(&buf);

	printf("device: %s\n", ifname);
	printf("writes: %d\n", REPRO_WRITES);
	printf("packet_len: %d\n", REPRO_PKT_LEN);
	printf("write_len: %zu\n", total_len);
	printf("topology: tun -> ip6gretap -> hsr(interlink) -> dummy slave\n");
	printf("inner_eth_mode: zero_eth\n");
	printf("tun_mode: legacy\n");
	printf("tun_flags: %#x\n", ifr.ifr_flags);
	printf("ip6gretap: %s local=%s remote=%s\n",
	       REPRO_GRETAP_IFNAME, TUN_LOCAL_IPV6, TUN_REMOTE_IPV6);
	printf("hsr: %s interlink=%s slaves=%s1,%s2\n",
	       REPRO_HSR_IFNAME, REPRO_GRETAP_IFNAME,
	       REPRO_HSR_SLAVE_IFNAME, REPRO_HSR_SLAVE_IFNAME);
	printf("vnet_hdr: gso_type=TCPV4 hdr_len=17 gso_size=4 csum_offset=0x0ca6\n");
	printf("outer: IPv6/dst-opts(%d)/GRE(TEB)\n", OUTER_DEST_OPTS_BYTES);
	printf("inner: Ethernet/IPv4/TCP\n");

	for (int i = 0; i < REPRO_WRITES; i++) {
		ret = write(tun_fd, buf, total_len);
		if (ret < 0)
			die("write");
		if (ret != (ssize_t)total_len) {
			fprintf(stderr, "short write[%d]: %zd/%zu\n", i, ret, total_len);
			return EXIT_FAILURE;
		}
		printf("write[%d] ok\n", i);
	}

	printf("linger: %d seconds\n", REPRO_LINGER_SECS);
	sleep(REPRO_LINGER_SECS);

	free(buf);
	close(tun_fd);
	return 0;
}
