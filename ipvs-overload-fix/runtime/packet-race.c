#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <linux/ip_vs.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static struct {
    struct ip_vs_service_user svc;
    struct ip_vs_dest_user dest;
} request;
static int control_fd, sender_fd, server_fd;
static struct sockaddr_in vip;
static atomic_uint epoch, edited, sent;
static atomic_uint_fast64_t last_seen;
static atomic_int stop;
static unsigned iterations, delay_max;
static int edit_error, send_error;

static void die(const char *what) { perror(what); exit(2); }
static void pin(int cpu) {
    cpu_set_t mask;
    CPU_ZERO(&mask); CPU_SET(cpu, &mask);
    if (pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask)) {
        fprintf(stderr, "cannot pin CPU %d\n", cpu); exit(2);
    }
}
static int upper(unsigned value) {
    typeof(request) arg = request;
    arg.dest.u_threshold = value;
    return setsockopt(control_fd, IPPROTO_IP, IP_VS_SO_SET_EDITDEST,
                      &arg, sizeof(arg));
}
static int send_packet(uint64_t sequence) {
    return sendto(sender_fd, &sequence, sizeof(sequence), 0,
                  (void *)&vip, sizeof(vip)) == sizeof(sequence) ? 0 : errno;
}
static void *editor(void *unused) {
    pin(1);
    for (unsigned n = 1; n <= iterations; n++) {
        while (atomic_load_explicit(&epoch, memory_order_acquire) != n) {
            if (atomic_load(&stop)) return NULL;
            __asm__ volatile("pause");
        }
        unsigned delay = delay_max ? (n * 2654435761U) % delay_max : 0;
        for (unsigned i = 0; i < delay; i++) __asm__ volatile("pause");
        edit_error = upper(1) < 0 ? errno : 0;
        atomic_store_explicit(&edited, n, memory_order_release);
    }
    return NULL;
}
static void *sender(void *unused) {
    pin(2);
    for (unsigned n = 1; n <= iterations; n++) {
        while (atomic_load_explicit(&epoch, memory_order_acquire) != n) {
            if (atomic_load(&stop)) return NULL;
            __asm__ volatile("pause");
        }
        send_error = send_packet((uint64_t)n * 2);
        atomic_store_explicit(&sent, n, memory_order_release);
    }
    return NULL;
}
static void *server(void *unused) {
    struct pollfd p = { .fd = server_fd, .events = POLLIN };
    pin(3);
    while (!atomic_load(&stop)) {
        if (poll(&p, 1, 50) > 0) {
            uint64_t seq;
            if (recv(server_fd, &seq, sizeof(seq), 0) == sizeof(seq))
                atomic_store_explicit(&last_seen, seq, memory_order_release);
        }
    }
    return NULL;
}
static int observed(int *total, unsigned *u, unsigned *l, unsigned *over) {
    FILE *f = fopen("/proc/ipvs_packet_state", "r");
    if (!f) return -1;
    int ret = fscanf(f, "total=%d upper=%u lower=%u overload=%u",total,u,l,over);
    fclose(f);
    return ret == 4 ? 0 : -1;
}
static int await_packet(uint64_t seq, unsigned milliseconds) {
    for (unsigned n = 0; n < milliseconds; n++) {
        if (atomic_load_explicit(&last_seen, memory_order_acquire) >= seq) return 1;
        usleep(1000);
    }
    return atomic_load_explicit(&last_seen, memory_order_acquire) >= seq;
}
int main(int argc, char **argv) {
    unsigned completed = 0, misses = 0, probe_lost = 0;
    pthread_t ed, tx, rx;
    struct sockaddr_in backend = { .sin_family=AF_INET, .sin_port=htons(31002) };
    iterations = argc > 1 ? strtoul(argv[1], NULL, 0) : 100000;
    delay_max = argc > 2 ? strtoul(argv[2], NULL, 0) : 64;
    setbuf(stdout, NULL);
    pin(0);
    control_fd = socket(AF_INET, SOCK_RAW, IPPROTO_RAW);
    sender_fd = socket(AF_INET, SOCK_DGRAM, 0);
    server_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (control_fd < 0 || sender_fd < 0 || server_fd < 0) die("socket");
    inet_pton(AF_INET, "127.0.0.2", &backend.sin_addr);
    if (bind(server_fd, (void *)&backend, sizeof(backend))) die("bind backend");
    vip.sin_family = AF_INET; vip.sin_port=htons(31001);
    inet_pton(AF_INET, "127.0.0.100", &vip.sin_addr);
    request.svc.protocol = IPPROTO_UDP;
    request.svc.addr = vip.sin_addr.s_addr;
    request.svc.port = vip.sin_port;
    request.svc.netmask = htonl(0xffffffff);
    request.svc.flags = IP_VS_SVC_F_ONEPACKET;
    strcpy(request.svc.sched_name,"rr");
    request.dest.addr = backend.sin_addr.s_addr;
    request.dest.port = backend.sin_port;
    request.dest.weight = 1;
    request.dest.u_threshold = 2;
    request.dest.conn_flags = IP_VS_CONN_F_MASQ | IP_VS_CONN_F_ONE_PACKET;
    if (setsockopt(control_fd, IPPROTO_IP, IP_VS_SO_SET_ADD,
                   &request.svc, sizeof(request.svc))) die("add service");
    if (setsockopt(control_fd, IPPROTO_IP, IP_VS_SO_SET_ADDDEST,
                   &request, sizeof(request))) die("add destination");
    if (pthread_create(&rx,NULL,server,NULL)) die("create receiver");
    if (send_packet(1) || !await_packet(1,1000)) {
        fprintf(stderr,"PACKETTEST smoke delivery failed\n"); return 2;
    }
    puts("PACKETTEST smoke delivery passed");
    if (pthread_create(&ed,NULL,editor,NULL) || pthread_create(&tx,NULL,sender,NULL))
        die("create workers");
    for (unsigned n = 1; n <= iterations; n++) {
        int total;
        unsigned u, l, over;
        if (upper(2)) die("rearm upper");
        if (observed(&total,&u,&l,&over) || total || over) {
            fprintf(stderr,"PACKETTEST initial state invalid\n"); exit(2);
        }
        atomic_store_explicit(&epoch,n,memory_order_release);
        while (atomic_load_explicit(&edited,memory_order_acquire) != n ||
               atomic_load_explicit(&sent,memory_order_acquire) != n)
            __asm__ volatile("pause");
        if (edit_error || send_error) {
            fprintf(stderr,"PACKETTEST syscall error edit=%d send=%d\n",edit_error,send_error);
            exit(2);
        }
        if (!await_packet((uint64_t)n*2,1000)) {
            fprintf(stderr,"PACKETTEST racing packet not delivered round=%u\n",n);
            exit(2);
        }
        if (observed(&total,&u,&l,&over)) die("observe");
        completed++;
        if (over && total == 0) {
            /* Exclude a transient sample while the expiry is finishing. */
            usleep(10000);
            if (observed(&total,&u,&l,&over)) die("confirm state");
            if (over && total == 0 && u == 1 && l == 1) {
                uint64_t probe = (uint64_t)n*2+1;
                misses++;
                int err=send_packet(probe);
                probe_lost=!await_packet(probe,100);
                printf("PACKETTEST miss round=%u total=%d upper=%u lower=%u overload=%u probe_send_errno=%d probe_lost=%u\n",
                       n,total,u,l,over,err,probe_lost);
                break;
            }
        }
    }
    atomic_store(&stop,1);
    pthread_join(ed,NULL); pthread_join(tx,NULL); pthread_join(rx,NULL);
    printf("PACKETTEST result requested=%u completed=%u delay_max=%u misses=%u probe_lost=%u\n",
           iterations,completed,delay_max,misses,probe_lost);
    if (setsockopt(control_fd, IPPROTO_IP, IP_VS_SO_SET_DEL,
                   &request.svc,sizeof(request.svc))) die("delete service");
    close(control_fd); close(sender_fd); close(server_fd);
    return 0;
}
