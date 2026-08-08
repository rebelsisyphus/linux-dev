/* Veth-based reproducer for the IPv4 SSRR broadcast-ICMP issue.
 *
 * Creates a veth pair in a new network namespace.  A raw packet is injected on
 * veth1 with destination MAC matching veth0.  The IPv4 packet has TTL=1, a
 * unicast destination (10.0.1.2), and a strict source route option whose first
 * hop is 255.255.255.255.  The kernel expires the TTL and calls __icmp_send(),
 * which echoes the SSRR options, so the ICMP Time Exceeded reply is addressed to
 * 255.255.255.255.  On an unpatched kernel the reply is emitted on veth0 (and
 * appears on veth1), demonstrating that the kernel sends an ICMP error to a
 * broadcast address.  The fix rejects this broadcast/multicast route.
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <linux/ip.h>
#include <linux/tcp.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

static int write_file(const char *path, const char *val)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC);
	ssize_t n;
	if (fd < 0) return -1;
	n = write(fd, val, strlen(val));
	close(fd);
	return n;
}

static int systemf(const char *fmt, ...)
{
	char cmd[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(cmd, sizeof(cmd), fmt, ap);
	va_end(ap);
	int r = system(cmd);
	if (r != 0) fprintf(stderr, "cmd failed (%d): %s\n", r, cmd);
	return r;
}

static unsigned short csum_ipv4(const void *buf, unsigned int len)
{
	const unsigned short *p = buf;
	unsigned int sum = 0;
	while (len > 1) { sum += *p++; len -= 2; }
	if (len) sum += htons(*(const unsigned char *)p << 8);
	while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
	return ~sum;
}

static int send_packet(int sock, int ifindex, const unsigned char *dst_mac, const unsigned char *src_mac)
{
	unsigned char pkt[128], *p = pkt;
	struct iphdr *ip;
	struct tcphdr *tcp;
	unsigned char *opt;

	memcpy(p, dst_mac, 6); p += 6;
	memcpy(p, src_mac, 6); p += 6;
	*(unsigned short *)p = htons(0x0800); p += 2;

	ip = (struct iphdr *)p;
	ip->version = 4; ip->ihl = 9; ip->tos = 0;
	ip->tot_len = htons(56); ip->id = 0; ip->frag_off = 0;
	ip->ttl = 1; ip->protocol = IPPROTO_TCP; ip->check = 0;
	inet_pton(AF_INET, "10.0.0.3", &ip->saddr);
	inet_pton(AF_INET, "10.0.1.2", &ip->daddr);
	p += 20;

	opt = p;
	opt[0] = 0x89; opt[1] = 7; opt[2] = 0xa2;
	*(unsigned int *)(opt + 3) = htonl(0xffffffff);
	opt[7] = 0x86; opt[8] = 6;
	*(unsigned int *)(opt + 9) = htonl(1);
	opt[13] = 0; opt[14] = 0; opt[15] = 0;
	p += 16;
	ip->check = csum_ipv4(ip, 36);

	tcp = (struct tcphdr *)p;
	memset(tcp, 0, sizeof(*tcp));
	tcp->doff = 5; tcp->syn = 1;
	p += 20;

	struct sockaddr_ll sa = {0};
	sa.sll_family = AF_PACKET;
	sa.sll_ifindex = ifindex;
	sa.sll_protocol = htons(ETH_P_ALL);
	sa.sll_halen = 6;
	memcpy(sa.sll_addr, dst_mac, 6);
	return sendto(sock, pkt, p - pkt, 0, (struct sockaddr *)&sa, sizeof(sa));
}

static int drain(int sock, int timeout_ms)
{
	unsigned char buf[2048];
	int found = 0;
	while (timeout_ms > 0) {
		fd_set rfds;
		struct timeval tv;
		FD_ZERO(&rfds); FD_SET(sock, &rfds);
		tv.tv_sec = 0; tv.tv_usec = 100000;
		int r = select(sock + 1, &rfds, NULL, NULL, &tv);
		if (r > 0 && FD_ISSET(sock, &rfds)) {
			int n = recvfrom(sock, buf, sizeof(buf), 0, NULL, NULL);
			if (n > 14) {
				struct iphdr *ip = (struct iphdr *)(buf + 14);
				if (ntohs(*(unsigned short *)(buf + 12)) == 0x0800 && n >= 14 + ip->ihl * 4) {
					char src[16], dst[16];
					inet_ntop(AF_INET, &ip->saddr, src, sizeof(src));
					inet_ntop(AF_INET, &ip->daddr, dst, sizeof(dst));
					printf("reply: %s -> %s TTL=%d proto=%d ihl=%d\n", src, dst, ip->ttl, ip->protocol, ip->ihl);
					if (ip->daddr == htonl(0xffffffff)) {
						printf("REPRODUCED: ICMP reply sent to 255.255.255.255\n");
						found = 1;
					}
				}
			}
		}
		timeout_ms -= 100;
	}
	return found;
}

int main(void)
{
	unshare(CLONE_NEWNET);
	unshare(CLONE_NEWNS);
	mount("none", "/proc", "proc", 0, NULL);

	write_file("/proc/sys/net/ipv4/ip_forward", "1");
	write_file("/proc/sys/net/ipv4/conf/all/accept_source_route", "1");
	write_file("/proc/sys/net/ipv4/conf/default/accept_source_route", "1");
	write_file("/proc/sys/net/ipv4/conf/all/rp_filter", "0");
	write_file("/proc/sys/net/ipv4/conf/default/rp_filter", "0");
	write_file("/proc/sys/net/ipv4/conf/all/log_martians", "1");

	systemf("/usr/sbin/ip link add veth0 type veth peer name veth1");
	systemf("/usr/sbin/ip link set veth0 address 02:00:00:00:00:01 up");
	systemf("/usr/sbin/ip link set veth1 up");
	systemf("/usr/sbin/ip addr add 10.0.0.1/24 dev veth0");
	systemf("/usr/sbin/ip route add 255.255.255.255/32 dev veth0 2>&1 || true");
	systemf("/usr/sbin/ip link set veth0 promisc on");

	/* let the veth pair settle */
	usleep(200000);

	write_file("/proc/sys/net/ipv4/conf/veth0/accept_source_route", "1");
	write_file("/proc/sys/net/ipv4/conf/veth1/accept_source_route", "1");

	int sock = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
	if (sock < 0) { perror("socket"); return 1; }
	int ifindex = if_nametoindex("veth1");
	struct sockaddr_ll sa = {0};
	sa.sll_family = AF_PACKET;
	sa.sll_ifindex = ifindex;
	sa.sll_protocol = htons(ETH_P_ALL);
	if (bind(sock, (struct sockaddr *)&sa, sizeof(sa)) < 0) { perror("bind"); return 1; }

	const unsigned char veth0_mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
	const unsigned char src_mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x02};
	int any = 0;
	for (int i = 0; i < 20; i++) {
		if (send_packet(sock, ifindex, veth0_mac, src_mac) < 0) {
			perror("send_packet");
			return 1;
		}
	}

	printf("Sent 20 crafted SSRR packets on veth1; waiting for ICMP reply on veth1...\n");
	any = drain(sock, 2000);
	if (!any) printf("No IPv4 reply captured.\n");
	close(sock);
	return 0;
}
