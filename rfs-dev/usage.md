# eBPF RPS/RFS Steering Usage

## Build

The sample uses the kernel BPF samples build flow:

```bash
make M=samples/bpf xdp_rps_rfs_user.o xdp_rps_rfs_kern.o -j$(nproc) \
  CLANG=clang-18 LLC=llc-18 OPT=opt-18 LLVM_DIS=llvm-dis-18 \
  LLVM_READELF=llvm-readelf-18 LLVM_OBJCOPY=llvm-objcopy-18
```

The exact LLVM binary names depend on the local toolchain.

If the host only has versioned LLVM tools, pass the full set above. Otherwise
the samples Makefile may invoke missing `opt` or `llvm-dis` binaries and leave
an empty BPF object behind.

## Prerequisites

- `CONFIG_BPF`
- XDP support on the target interface
- bpffs mounted when using pinned maps, usually at `/sys/fs/bpf`
- cgroup v2 path when enabling the sockops RFS updater

Example bpffs mount:

```bash
mount -t bpf bpf /sys/fs/bpf
```

## Basic RPS

Distribute receive flows across CPUs `0-15`:

```bash
sudo samples/bpf/xdp_rps_rfs -i eth0 -c 0-15
```

Use skb mode when native XDP is unavailable:

```bash
sudo samples/bpf/xdp_rps_rfs -i eth0 -c 0-15 -S
```

## Automatic RFS

Attach the sockops updater to the root cgroup and keep flow processing on the
process CPU:

```bash
sudo samples/bpf/xdp_rps_rfs \
  -i eth0 -c 0-15 -g /sys/fs/cgroup -m process
```

Refresh the RFS desired CPU on every TCP receive:

```bash
sudo samples/bpf/xdp_rps_rfs \
  -i eth0 -c 0-15 -g /sys/fs/cgroup -m process -R
```

Select another allowed CPU in the same cluster:

```bash
sudo samples/bpf/xdp_rps_rfs \
  -i eth0 -c 0-15 -g /sys/fs/cgroup -m cluster
```

Select another allowed CPU in the same NUMA node:

```bash
sudo samples/bpf/xdp_rps_rfs \
  -i eth0 -c 0-15 -g /sys/fs/cgroup -m numa
```

## Pinned maps

Pin maps for later inspection and manual flow updates:

```bash
sudo samples/bpf/xdp_rps_rfs \
  -i eth0 -c 0-15 -p /sys/fs/bpf/xdp_rps_rfs
```

Pinning can be combined with sockops:

```bash
sudo samples/bpf/xdp_rps_rfs \
  -i eth0 -c 0-15 \
  -g /sys/fs/cgroup \
  -m numa \
  -p /sys/fs/bpf/xdp_rps_rfs
```

## Statistics

Read counters from pinned maps:

```bash
sudo samples/bpf/xdp_rps_rfs \
  -p /sys/fs/bpf/xdp_rps_rfs -s
```

Reported counters:

- `pass`
- `rps_redirect`
- `rfs_redirect`
- `fallback_pass`
- `drop`
- `sockops_update`
- `recv_enter`
- `recv_bad_family`
- `recv_update`

For TCP receive-side learning, `recv_enter` confirms that the `tcp_recvmsg`
kprobe ran, and `recv_update` confirms that it updated `flow_state`.

## Flow state dump

List pinned RFS flow state:

```bash
sudo samples/bpf/xdp_rps_rfs \
  -p /sys/fs/bpf/xdp_rps_rfs -l
```

Example output:

```text
family 2 proto 6 sport 23456 dport 51610 active_cpu 1 desired_cpu 1 last_qtail 43
```

This is intended for debugging and tests. It avoids depending on a full
`bpftool map dump` in small VM images where only bootstrap bpftool is
available.

## Manual RFS flow management

Add or replace an IPv4 TCP flow entry:

```bash
sudo samples/bpf/xdp_rps_rfs \
  -p /sys/fs/bpf/xdp_rps_rfs \
  -r tcp4,192.0.2.1,198.51.100.2,443,51514 \
  -t 2
```

Manual flow updates change the desired CPU. Existing packets already queued to
the old CPU are allowed to drain before XDP commits the new active CPU.

Delete an entry:

```bash
sudo samples/bpf/xdp_rps_rfs \
  -p /sys/fs/bpf/xdp_rps_rfs \
  -D tcp4,192.0.2.1,198.51.100.2,443,51514
```

Supported flow prefixes:

- `tcp4`
- `udp4`
- `tcp6`
- `udp6`

Flow format:

```text
protocol,src_ip,dst_ip,src_port,dst_port
```

## Detach

When running in the foreground, `Ctrl-C` detaches the program.

Detach explicitly:

```bash
sudo samples/bpf/xdp_rps_rfs -d -i eth0
```

## Strategy selection guidance

| Strategy | Use when |
| --- | --- |
| `process` | Maximum cache locality matters more than CPU isolation. |
| `cluster` | The workload benefits from avoiding softirq/application CPU sharing while staying topologically close. |
| `numa` | Avoiding CPU sharing and cross-NUMA memory traffic matters more than choosing the nearest sibling CPU. |

## In-order migration behavior

The sample tracks per-CPU CPUMAP queue progress:

- XDP increments `cpu_queue_tail[cpu]` when enqueueing to CPUMAP.
- The CPUMAP program increments `cpu_queue_head[cpu]` when packets are dequeued
  on the target CPU.
- `flow_state.last_qtail` records the old queue tail for the flow.

When the desired CPU changes, XDP keeps using the old active CPU until:

```text
cpu_queue_head[old_cpu] - flow_state.last_qtail >= 0
```

This mirrors the classic RPS/RFS queue-drain rule and avoids crossing packets
between the old and new CPU queues.

## Validation commands

```bash
make M=samples/bpf xdp_rps_rfs_user.o xdp_rps_rfs_kern.o -j$(nproc) \
  CLANG=clang-18 LLC=llc-18 OPT=opt-18 LLVM_DIS=llvm-dis-18 \
  LLVM_READELF=llvm-readelf-18 LLVM_OBJCOPY=llvm-objcopy-18

./scripts/checkpatch.pl --strict --no-tree -f \
  samples/bpf/xdp_rps_rfs_kern.c samples/bpf/xdp_rps_rfs_user.c
```

Run the local NUMA steering test with `test-kernel`:

```bash
test-kernel arch/x86/boot/bzImage

ssh root@localhost -p 2222 \
  'cd /mnt/shared && ./bpf_rfs_numa_test.sh'
```

Expected key lines:

```text
recv_enter       132
recv_bad_family  0
recv_update      132
family 2 proto 6 ... active_cpu 1 desired_cpu 1 ...
PASS: NUMA strategy selected CPU1 for a CPU0 receiver
```
