/*
 * Minimized x86 userspace model of IPVS destination edit versus bind/unbind.
 * This models the shared counter, thresholds and flag update; it does not
 * configure IPVS or reproduce a complete kernel network workload.
 * Requires three available CPUs. The second argument optionally adds a
 * full fence after publishing thresholds, before reading the count.
 * The third argument selects bind (0) or unbind (1).
 *
 * gcc -O2 -std=gnu11 -pthread overload-ordering.c -o /tmp/ipvs-ordering
 * /tmp/ipvs-ordering 1000000 0
 * /tmp/ipvs-ordering 1000000 1
 */
#define _GNU_SOURCE
#include <stdatomic.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sched.h>
#include <stdint.h>
struct dest_model {
    atomic_uint flags;
    atomic_int total;
    atomic_uint u, l, lv;
    atomic_flag lock;
} __attribute__((aligned(64)));
static struct dest_model d;
static atomic_uint epoch __attribute__((aligned(64)));
static atomic_uint done_bind __attribute__((aligned(64)));
static atomic_uint done_edit __attribute__((aligned(64)));
static int cpus[3];
static void pin(int which) {
    cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET(cpus[which], &mask);
    pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask);
}
static void wait_epoch(unsigned n) {
    while (atomic_load_explicit(&epoch, memory_order_acquire) < n + 1)
        __asm__ volatile("pause");
}
static unsigned iterations;
static int fence_edit;
static int unbind_event;
static int edit_read, bind_read;
static inline void lock_dest(void) {
    while (atomic_flag_test_and_set_explicit(&d.lock, memory_order_acquire))
        __asm__ volatile("pause");
}
static inline void unlock_dest(void) {
    atomic_flag_clear_explicit(&d.lock, memory_order_release);
}
static inline void update(int mode) {
    unsigned u = atomic_load_explicit(&d.u, memory_order_relaxed);
    if (!u) goto unset;
    unsigned l = atomic_load_explicit(&d.lv, memory_order_relaxed);
    int conns = atomic_load_explicit(&d.total, memory_order_relaxed);
    if (!mode) edit_read = conns;
    if (conns >= (mode > 0 ? l : u)) {
        atomic_store_explicit(&d.flags, 2, memory_order_relaxed);
        return;
    }
    if (conns >= (mode < 0 ? u : l)) return;
unset:
    atomic_store_explicit(&d.flags, 0, memory_order_relaxed);
}
static void *bind_worker(void *unused) {
    pin(1);
    for (unsigned n = 0; n < iterations; n++) {
        wait_epoch(n);
        int tc;
        unsigned u;
        if (unbind_event) {
            tc = atomic_fetch_sub_explicit(&d.total, 1, memory_order_seq_cst);
            u = atomic_load_explicit(&d.lv, memory_order_relaxed);
        } else {
            tc = atomic_fetch_add_explicit(&d.total, 1, memory_order_seq_cst) + 1;
            u = atomic_load_explicit(&d.u, memory_order_relaxed);
        }
        bind_read = u;
        if (tc == u) {
            lock_dest();
            update(unbind_event ? -1 : 1);
            unlock_dest();
        }
        atomic_store_explicit(&done_bind, n + 1, memory_order_release);
    }
    return 0;
}
static void *edit_worker(void *unused) {
    pin(2);
    for (unsigned n = 0; n < iterations; n++) {
        wait_epoch(n);
        if (atomic_load_explicit(&d.u, memory_order_relaxed) != 100 ||
            atomic_load_explicit(&d.l, memory_order_relaxed) != (unbind_event ? 75 : 0)) {
            lock_dest();
            atomic_store_explicit(&d.u, 100, memory_order_relaxed);
            atomic_store_explicit(&d.l, unbind_event ? 75 : 0, memory_order_relaxed);
            atomic_store_explicit(&d.lv, 75, memory_order_relaxed);
            if (fence_edit) atomic_thread_fence(memory_order_seq_cst);
            update(0);
            unlock_dest();
        }
        atomic_store_explicit(&done_edit, n + 1, memory_order_release);
    }
    return 0;
}
int main(int argc, char **argv) {
    iterations = argc > 1 ? atoi(argv[1]) : 200000;
    fence_edit = argc > 2 ? atoi(argv[2]) : 0;
    unbind_event = argc > 3 ? atoi(argv[3]) : 0;
    atomic_flag_clear(&d.lock);
    cpu_set_t allowed;
    sched_getaffinity(0, sizeof(allowed), &allowed);
    int count = 0;
    for (int i = 0; i < CPU_SETSIZE && count < 3; i++)
        if (CPU_ISSET(i, &allowed)) cpus[count++] = i;
    if (count < 3) return 2;
    pin(0);
    pthread_t other, editor;
    pthread_create(&other, 0, bind_worker, 0);
    pthread_create(&editor, 0, edit_worker, 0);
    unsigned misses = 0;
    for (unsigned n = 0; n < iterations; n++) {
        atomic_store_explicit(&d.total, unbind_event ? 75 : 99, memory_order_relaxed);
        atomic_store_explicit(&d.u, unbind_event ? 100 : 200, memory_order_relaxed);
        atomic_store_explicit(&d.l, unbind_event ? 50 : 0, memory_order_relaxed);
        atomic_store_explicit(&d.lv, unbind_event ? 50 : 150, memory_order_relaxed);
        atomic_store_explicit(&d.flags, unbind_event ? 2 : 0, memory_order_relaxed);
        atomic_store_explicit(&epoch, n + 1, memory_order_release);
        while (atomic_load_explicit(&done_bind, memory_order_acquire) < n + 1 ||
               atomic_load_explicit(&done_edit, memory_order_acquire) < n + 1)
            __asm__ volatile("pause");
        if (atomic_load_explicit(&d.flags, memory_order_relaxed) != (unbind_event ? 0 : 2)) {
            if (misses < 3)
                printf("miss iteration=%u edit_read_total=%d accounting_read_threshold=%d final_total=%d final_upper=%u flags=%u\n", n, edit_read, bind_read, atomic_load(&d.total), atomic_load(&d.u), atomic_load(&d.flags));
            misses++;
        }
    }
    pthread_join(other, 0);
    pthread_join(editor, 0);
    printf("event=%s iterations=%u edit_full_fence=%d misses=%u\n", unbind_event ? "unbind" : "bind", iterations, fence_edit, misses);
}
