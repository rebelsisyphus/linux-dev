/* SPDX-License-Identifier: GPL-2.0 */
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_core_read.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_endian.h>

char LICENSE[] SEC("license") = "GPL";

struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, __u64);
} rx_counts SEC(".maps");

/* Diagnostic runs only: count entry to the TCP receive protocol handler. */
SEC("kprobe/tcp_v4_rcv")
int BPF_KPROBE(count_rx, struct sk_buff *skb)
{
	unsigned char *data = BPF_CORE_READ(skb, data);
	__u32 key = 0;
	__u64 *count;
	__be16 port;

	if (bpf_probe_read_kernel(&port, sizeof(port), data + 2))
		return 0;
	if (port != bpf_htons(6379))
		return 0;
	count = bpf_map_lookup_elem(&rx_counts, &key);
	if (count)
		(*count)++;
	return 0;
}
