# eBPF RPS/RFS Steering Design

## Goals

This project provides an eBPF/XDP sample that demonstrates receive packet
steering without modifying the in-kernel RPS/RFS implementation in
`net/core/dev.c`.

The design goals are:

- Distribute flows across a configured CPU set when no flow affinity exists.
- Prefer flow-to-CPU affinity when an RFS entry is available.
- Support topology-aware CPU selection for RFS:
  - run on the userspace process CPU;
  - run on another CPU in the same cluster;
  - run on another CPU in the same NUMA node.
- Avoid expensive topology work in the BPF fast path.
- Keep the XDP receive path verifier-friendly and bounded.
- Preserve packet order while a flow is moved between CPUs.
- Refresh RFS CPU hints on receive, so userspace CPU migration is reflected.

## Components

### XDP data plane

`samples/bpf/xdp_rps_rfs_kern.c` contains `xdp_rps_rfs_prog`.

For every supported packet it:

1. Parses Ethernet, optional VLAN, IPv4/IPv6, and TCP/UDP headers.
2. Builds a stable flow key.
3. Looks up `flow_state` for RFS steering.
4. Falls back to hash-based RPS selection from `cpu_slots`.
5. Redirects the packet with `bpf_redirect_map()` into `cpu_map`.

### Sockops control plane

The same BPF object contains `xdp_rps_rfs_sockops`.

When attached to a cgroup, the sockops program handles TCP established events,
builds the reverse receive-side flow key, selects a target CPU according to the
configured RFS policy, and writes the result into `flow_state`.

### Receive-side update plane

`xdp_rps_rfs_tcp_recvmsg` attaches to `tcp_recvmsg`. Each IPv4 or IPv6 TCP
receive updates the flow's desired CPU using the current userspace CPU. This
mirrors the classic RFS `rps_record_sock_flow()` idea: receive-side execution
continuously refreshes the CPU hint instead of only learning it at connection
setup.

Userspace attaches this program with an explicit kprobe attach to
`tcp_recvmsg`. Avoid relying on generic section-name auto attach here; explicit
attach makes failures easier to diagnose and avoids silently missing receive
updates.

The hook updates the desired CPU only. The XDP data path commits the migration
after the old target CPU queue has drained enough to preserve packet order.

### Userspace loader

`samples/bpf/xdp_rps_rfs_user.c`:

- loads the BPF object;
- configures CPUMAP entries;
- reads CPU topology from sysfs;
- precomputes topology-aware candidate CPU sets;
- optionally attaches sockops to a cgroup;
- pins maps for manual flow updates;
- reports steering statistics;
- lists pinned flow states for debugging and tests.

## Maps

| Map | Type | Purpose |
| --- | --- | --- |
| `cpu_map` | `BPF_MAP_TYPE_CPUMAP` | Redirect packets to a target CPU. |
| `cpu_slots` | array | Ordered RPS CPU set. |
| `config` | array | Number of RPS CPUs and active RFS strategy. |
| `allowed_cpus` | array | CPU membership from the user supplied `-c` set. |
| `cluster_cpu_counts` | array | Number of same-cluster alternate CPUs per process CPU. |
| `cluster_cpu_slots` | array | Flattened same-cluster alternate CPU slots. |
| `numa_cpu_counts` | array | Number of same-NUMA alternate CPUs per process CPU. |
| `numa_cpu_slots` | array | Flattened same-NUMA alternate CPU slots. |
| `flow_state` | LRU hash | Flow-to-CPU RFS state with active CPU, desired CPU, and last queue tail. |
| `cpu_queue_head` | array | Per-CPU CPUMAP dequeue progress. |
| `cpu_queue_tail` | array | Per-CPU CPUMAP enqueue progress. |
| `stats` | per-CPU array | Pass, redirect, fallback, drop, sockops, and receive counters. |

## Flow handling

### RPS miss path

If `flow_state` does not contain an entry:

1. Compute a flow hash from IP addresses, ports, family, and protocol.
2. Use `hash % nr_cpus` to select one `cpu_slots` entry.
3. Redirect to the corresponding CPUMAP CPU.

### RFS hit path

If `flow_state` contains an entry:

1. Read the active and desired CPUs.
2. If the desired CPU differs from the active CPU, check whether the old CPU's
   queue head has reached the flow's saved queue tail.
3. Switch active CPU only after the old queue has drained.
4. Save the enqueue tail for the selected CPU.
5. Redirect to that CPU through CPUMAP.

## In-order migration

Classic RPS stores the target CPU and `last_qtail` for each flow. When the
application CPU changes, RPS does not immediately steer to the new CPU. It waits
until the old CPU's input queue head advances past the saved tail value:

```text
old_cpu.input_queue_head - flow.last_qtail >= 0
```

The sample mirrors that design for CPUMAP:

- `cpu_queue_tail[cpu]` is incremented when XDP enqueues a packet to that CPU.
- `flow_state.last_qtail` records the tail value used for the flow's last
  enqueue.
- `xdp_rps_rfs_cpumap` runs on the target CPU and increments
  `cpu_queue_head[cpu]` when a packet is dequeued from CPUMAP.
- A flow can switch from `active_cpu` to `desired_cpu` only when the old
  `cpu_queue_head` has caught up with `last_qtail`.

This prevents packets already queued to the old CPU from being overtaken by new
packets sent to the new CPU.

## RFS CPU strategies

### `process`

Use the CPU on which the sockops callback runs.

Benefits:

- Closest approximation to classic RFS locality.
- Good cache locality for socket setup.

Tradeoff:

- Softirq work and userspace execution can share one CPU.

### `cluster`

Choose another allowed CPU in the same cluster as the process CPU.

Benefits:

- Avoids direct CPU contention with the process.
- Keeps steering close in the CPU topology.

Fallback:

- If no alternate allowed cluster CPU exists, use the process CPU.

### `numa`

Choose another allowed CPU in the same NUMA node as the process CPU.

Benefits:

- Avoids direct CPU contention with the process.
- Avoids cross-NUMA access when possible.

Fallback:

- If no alternate allowed NUMA CPU exists, use the process CPU.

### Final fallback

If the process CPU itself is not in the allowed CPU set, use the first configured
RPS CPU.

## Topology discovery

Userspace reads:

- `/sys/devices/system/cpu/cpu*/topology/cluster_cpus_list`
- `/sys/devices/system/cpu/cpu*/topology/package_cpus_list`
- `/sys/devices/system/node/node*/cpulist`

Cluster and NUMA candidate sets are intersected with the user supplied `-c`
CPU set and exclude the process CPU itself.

## Why topology is precomputed in userspace

The BPF programs need bounded work and simple verifier-friendly accesses.
Userspace can parse sysfs and build topology tables once at startup. The BPF
programs then only perform fixed map lookups.

## Why topology slots are flattened

A map value containing `nr_cpus` plus `cpus[MAX_CPUS]` is natural in C, but
variable indexing into a large map value is difficult for the verifier to prove
safe. The implementation therefore uses:

- one count map indexed by process CPU;
- one flattened slot map indexed by `process_cpu * MAX_CPUS + slot`.

This keeps the sockops path simple and verifier-compatible.

## Limits

- This is a sample and does not replace the kernel RPS/RFS implementation.
- Automatic RFS learning covers TCP established callbacks and optional
  `tcp_recvmsg` updates.
- UDP entries need manual population.
- The receive hook records IPv4 and IPv6 TCP receive CPU hints.
- Cluster behavior depends on topology files exposed by the platform.
- Pinned maps require bpffs to be mounted before use.

## Debug counters

The receive hook has separate counters to make failures diagnosable:

- `recv_enter`: the `tcp_recvmsg` kprobe program ran.
- `recv_bad_family`: the hook ran, but the socket family was not IPv4 or IPv6.
- `recv_update`: the hook parsed the socket and updated `flow_state`.

Expected healthy TCP receive behavior is `recv_enter == recv_update` and
`recv_bad_family == 0` for IPv4/IPv6 TCP traffic.

## NUMA strategy validation

The local QEMU test environment uses two NUMA nodes:

```text
node0: CPUs 0-1
node1: CPUs 2-3
```

The NUMA strategy test pins the TCP receiver to CPU0 and allows CPUs `0-3`.
With `-m numa`, the receive hook should choose CPU1: it is in the same NUMA
node as CPU0 and is not the receiver CPU.

The validation checks:

- `recv_enter` and `recv_update` increase after TCP receive.
- `recv_bad_family` remains zero.
- listed `flow_state` entries have `desired_cpu 1`.
- packets can be redirected through RFS after the receive-side update.
