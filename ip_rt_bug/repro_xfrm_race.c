/*
 * TUN + xfrm reverse-path race reproducer for the icmp_route_lookup ip_rt_bug
 * issue (second reference patch: xfrm: fix ip_rt_bug race in icmp_route_lookup
 * reverse path).
 *
 * The bug:
 *   - A packet with an invalid IP option is injected via a TUN interface.
 *   - ip_options_compile() calls __icmp_send() to emit an ICMP error.
 *   - An xfrm output policy forces icmp_route_lookup() into the reverse path.
 *   - In the reverse path, if the original source address becomes local between
 *     the initial check and ip_route_input(), ip_route_input() returns a local
 *     route with dst.output == ip_rt_bug, and the ICMP reply triggers a WARN.
 *
 * Reproduction requires a race window; a temporary kernel delay in
 * icmp_route_lookup() makes it deterministic.
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/if_tun.h>
#include <linux/ip.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/tcp.h>

#define TUN_IFACE "tun0"
#define LOCAL_IP  "192.168.141.1"
#define RACE_IP   "10.0.0.100"
#define DST_IP    "192.168.141.2"
#define LOCAL_PREFIX 24
#define LOCAL_MAC 0xaaaaaaaaaaaaULL

struct nlmsg {
    char *pos;
    char buf[4096];
};

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

static void netlink_init(struct nlmsg *nlmsg, int typ, int flags,
                         const void *data, int size)
{
    struct nlmsghdr *hdr = (struct nlmsghdr *)nlmsg->buf;
    memset(nlmsg, 0, sizeof(*nlmsg));
    hdr->nlmsg_type = typ;
    hdr->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | flags;
    memcpy(hdr + 1, data, size);
    nlmsg->pos = (char *)(hdr + 1) + NLMSG_ALIGN(size);
}

static void netlink_attr(struct nlmsg *nlmsg, int typ, const void *data, int size)
{
    struct nlattr *attr = (struct nlattr *)nlmsg->pos;
    attr->nla_len = sizeof(*attr) + size;
    attr->nla_type = typ;
    if (size > 0)
        memcpy(attr + 1, data, size);
    nlmsg->pos += NLMSG_ALIGN(attr->nla_len);
}

static int netlink_send(struct nlmsg *nlmsg, int sock)
{
    if (nlmsg->pos > nlmsg->buf + sizeof(nlmsg->buf))
        return -1;
    struct nlmsghdr *hdr = (struct nlmsghdr *)nlmsg->buf;
    hdr->nlmsg_len = nlmsg->pos - nlmsg->buf;
    struct sockaddr_nl addr = { .nl_family = AF_NETLINK };
    if (sendto(sock, nlmsg->buf, hdr->nlmsg_len, 0,
               (struct sockaddr *)&addr, sizeof(addr)) != (ssize_t)hdr->nlmsg_len)
        return -1;
    char reply[4096];
    ssize_t n = recv(sock, reply, sizeof(reply), 0);
    if (n < 0)
        return -1;
    struct nlmsghdr *rhdr = (struct nlmsghdr *)reply;
    if (n < (ssize_t)sizeof(*rhdr))
        return -1;
    if (rhdr->nlmsg_type == NLMSG_ERROR) {
        struct nlmsgerr *err = (struct nlmsgerr *)(rhdr + 1);
        return -err->error;
    }
    return 0;
}

static void netlink_add_addr4(struct nlmsg *nlmsg, int sock, const char *dev,
                              const char *addr, int prefix)
{
    struct in_addr in_addr;
    inet_pton(AF_INET, addr, &in_addr);
    struct ifaddrmsg hdr = {
        .ifa_family = AF_INET,
        .ifa_prefixlen = prefix,
        .ifa_scope = RT_SCOPE_UNIVERSE,
        .ifa_index = if_nametoindex(dev),
    };
    netlink_init(nlmsg, RTM_NEWADDR, NLM_F_CREATE | NLM_F_REPLACE, &hdr, sizeof(hdr));
    netlink_attr(nlmsg, IFA_LOCAL, &in_addr, sizeof(in_addr));
    netlink_attr(nlmsg, IFA_ADDRESS, &in_addr, sizeof(in_addr));
    netlink_send(nlmsg, sock);
}

static void netlink_del_addr4(struct nlmsg *nlmsg, int sock, const char *dev,
                              const char *addr, int prefix)
{
    struct in_addr in_addr;
    inet_pton(AF_INET, addr, &in_addr);
    struct ifaddrmsg hdr = {
        .ifa_family = AF_INET,
        .ifa_prefixlen = prefix,
        .ifa_scope = RT_SCOPE_UNIVERSE,
        .ifa_index = if_nametoindex(dev),
    };
    netlink_init(nlmsg, RTM_DELADDR, 0, &hdr, sizeof(hdr));
    netlink_attr(nlmsg, IFA_LOCAL, &in_addr, sizeof(in_addr));
    netlink_attr(nlmsg, IFA_ADDRESS, &in_addr, sizeof(in_addr));
    netlink_send(nlmsg, sock);
}

static void netlink_set_mac(struct nlmsg *nlmsg, int sock, const char *dev,
                            uint64_t mac)
{
    struct ifinfomsg hdr = {
        .ifi_family = AF_UNSPEC,
        .ifi_index = if_nametoindex(dev),
        .ifi_change = IFF_UP,
        .ifi_flags = IFF_UP,
    };
    netlink_init(nlmsg, RTM_NEWLINK, 0, &hdr, sizeof(hdr));
    netlink_attr(nlmsg, IFLA_ADDRESS, &mac, ETH_ALEN);
    netlink_send(nlmsg, sock);
}

static void netlink_bring_up(struct nlmsg *nlmsg, int sock, const char *dev)
{
    struct ifinfomsg hdr = {
        .ifi_family = AF_UNSPEC,
        .ifi_index = if_nametoindex(dev),
        .ifi_change = IFF_UP,
        .ifi_flags = IFF_UP,
    };
    netlink_init(nlmsg, RTM_NEWLINK, 0, &hdr, sizeof(hdr));
    netlink_send(nlmsg, sock);
}

static int tun_alloc(char *dev)
{
    int fd = open("/dev/net/tun", O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        perror("open /dev/net/tun");
        return -1;
    }
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
    strncpy(ifr.ifr_name, dev, IFNAMSIZ - 1);
    if (ioctl(fd, TUNSETIFF, (void *)&ifr) < 0) {
        perror("TUNSETIFF");
        close(fd);
        return -1;
    }
    return fd;
}

static uint16_t ip_checksum(const uint8_t *data, size_t len)
{
    uint32_t sum = 0;
    for (size_t i = 0; i + 1 < len; i += 2)
        sum += *(uint16_t *)&data[i];
    if (len & 1)
        sum += data[len - 1];
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return ~sum;
}

static void build_packet(uint8_t *pkt, size_t *len)
{
    uint8_t *p = pkt;
    memset(pkt, 0, 128);

    /* Ethernet header */
    memcpy(p, "\xaa\xaa\xaa\xaa\xaa\xaa", 6); p += 6;
    memcpy(p, "\xdf\x00\x00\x40\x00\x00", 6); p += 6;
    *(uint16_t *)p = htons(0x0800); p += 2;

    /* IPv4 header with an invalid SSRR option (pointer > length) */
    struct iphdr *ip = (struct iphdr *)p;
    ip->version = 4;
    ip->ihl = 9;                /* 36 bytes header */
    ip->tos = 0;
    ip->tot_len = htons(56);    /* 36 + 20 TCP */
    ip->id = 0;
    ip->frag_off = 0;
    ip->ttl = 1;                  /* expired TTL forces ICMP Time Exceeded */
    ip->protocol = IPPROTO_TCP;
    ip->check = 0;
    inet_pton(AF_INET, RACE_IP, &ip->saddr);
    inet_pton(AF_INET, DST_IP, &ip->daddr);
    p += 20;

    /* SSRR option: type=0x89, len=7, ptr=0xa2, data=255.255.255.255 */
    p[0] = 0x89; p[1] = 7; p[2] = 0xa2;
    *(uint32_t *)(p + 3) = htonl(0xffffffff);
    p[7] = 0x86; p[8] = 6; *(uint32_t *)(p + 9) = htonl(1);
    p += 16;

    ip->check = ip_checksum((uint8_t *)ip, 36);

    /* TCP header (20 bytes, SYN) */
    struct tcphdr *tcp = (struct tcphdr *)p;
    tcp->doff = 5;
    tcp->syn = 1;
    p += 20;

    *len = p - pkt;
}

int main(int argc, char **argv)
{
    int noroute = argc > 1 && strcmp(argv[1], "noroute") == 0;
    if (unshare(CLONE_NEWNET) < 0) {
        perror("unshare CLONE_NEWNET");
        return 1;
    }
    if (unshare(CLONE_NEWNS) < 0) {
        perror("unshare CLONE_NEWNS");
        return 1;
    }
    mount("none", "/proc", "proc", 0, NULL);

    write_file("/proc/sys/net/ipv4/ip_forward", "1");
    write_file("/proc/sys/net/ipv4/conf/all/rp_filter", "0");
    write_file("/proc/sys/net/ipv4/conf/default/rp_filter", "0");
    write_file("/proc/sys/net/ipv4/conf/all/accept_source_route", "1");
    write_file("/proc/sys/net/ipv4/conf/default/accept_source_route", "1");
    write_file("/proc/sys/net/ipv4/conf/all/log_martians", "1");

    int nlsock = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (nlsock < 0) { perror("netlink socket"); return 1; }

    int tun = tun_alloc(TUN_IFACE);
    if (tun < 0) { close(nlsock); return 1; }

    struct nlmsg nlmsg;
    netlink_set_mac(&nlmsg, nlsock, TUN_IFACE, LOCAL_MAC);
    netlink_add_addr4(&nlmsg, nlsock, TUN_IFACE, LOCAL_IP, LOCAL_PREFIX);
    netlink_bring_up(&nlmsg, nlsock, TUN_IFACE);
    close(nlsock);

    systemf("ip link set %s address aa:aa:aa:aa:aa:aa", TUN_IFACE);
    systemf("ip link set %s promisc on", TUN_IFACE);
    systemf("ip route add 255.255.255.255/32 dev %s", TUN_IFACE);
    if (!noroute)
        systemf("ip route add 10.0.0.0/8 dev %s", TUN_IFACE);

    char sysctl[64];
    snprintf(sysctl, sizeof(sysctl), "/proc/sys/net/ipv4/conf/%s/rp_filter", TUN_IFACE);
    write_file(sysctl, "0");
    snprintf(sysctl, sizeof(sysctl), "/proc/sys/net/ipv4/conf/%s/accept_source_route", TUN_IFACE);
    write_file(sysctl, "1");
    snprintf(sysctl, sizeof(sysctl), "/proc/sys/net/ipv4/conf/%s/accept_local", TUN_IFACE);
    write_file(sysctl, "1");

    /* xfrm output policy that forces the reverse path in icmp_route_lookup(). */
    systemf("ip xfrm policy add src 0.0.0.0/0 dst 0.0.0.0/0 "
            "dir out priority 0 ptype main flag localok icmp");

    /* Child: race adding RACE_IP as a local address on tun0. */
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 1; }
    if (pid == 0) {
        int sock = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
        if (sock < 0) exit(1);
        struct nlmsg nl;
        for (int i = 0; i < 2000; i++) {
            netlink_add_addr4(&nl, sock, TUN_IFACE, RACE_IP, 32);
            usleep(500);
            netlink_del_addr4(&nl, sock, TUN_IFACE, RACE_IP, 32);
            usleep(500);
        }
        close(sock);
        exit(0);
    }

    /* Parent: inject malformed packets. */
    uint8_t pkt[128];
    size_t pkt_len;
    build_packet(pkt, &pkt_len);

    for (int i = 0; i < 20; i++) {
        if (write(tun, pkt, pkt_len) != (ssize_t)pkt_len) {
            perror("write tun");
        }
        usleep(100000);
    }

    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);

    close(tun);
    return 0;
}
