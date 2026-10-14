# BPF RFS sample

2026-09-28: build on infrastructure tip 7cbebe4694ff, without adding a
kernel selection policy or CONFIG_RPS dependency.

The sample observes application socket use with fentry programs at
inet_sendmsg/inet6_sendmsg, inet_recvmsg/inet6_recvmsg, inet_splice_eof and
tcp_splice_read, plus successful inet_accept at fexit;
tcp_zerocopy_receive is optional when available in BTF.
Only established TCP/connected UDP sockets in the loader's network namespace
are recorded. One aligned u32 CPU-plus-one hint in sk_storage permits
lockless last-writer-wins updates. No task pointer, flow hashtable, TTL timer,
or separate socket lifetime management is needed. This matches native RFS's
persistent advisory hint; multi-threaded sockets have no exclusive CPU owner.

XDP parses IPv4/IPv6 TCP/UDP tuples, hashes the tuple to an immutable ordered
domain from the first packet, and reads a socket hint without allocating
storage. TCP listeners and unconnected UDP are not used as hints. Missing
lookup/storage, or hints outside the configured CPU set, select a stable
hash fallback CPU but still use ordered redirect and the original domain.
Hash collisions delay migration conservatively. No live domain reallocation
or LRU eviction is used. All acquired socket references are released.

Scope: direct unicast, non-fragmented IPv4 and IPv6 without extension headers,
and a static configured CPU set. Other packet formats use PASS and are outside
the ordering contract, including flows mixing supported and unsupported
formats. VLAN, tunnels, NAT/VRF and reuseport/custom demux are not claimed as
equivalent to final stack lookup. Native RPS must be disabled on this path.
The loader requires ingress administratively down for attach/detach, drains
domains before detach, and refuses replacement of another XDP program.
Both rps_cpus and rps_flow_cnt must be zero: native RFS is independent of
the RPS CPU mask and would otherwise add another asynchronous handoff.
CPU topology changes require quiesce/drain/restart to recreate workers;
kernel hotplug exception semantics still apply during the transition.

Deliver separate patches for policy + loader, real-socket selftests, and
usage/limitations documentation. Reuse the actual sample BPF source in tests.
Validate CONFIG_RPS=n under the existing KASAN/lockdep QEMU kernel, including
CPU migration on receive/send/splice, socket lookup/storage miss, CPU-set
fallback, socket destruction, IPv4/IPv6 and TCP/connected UDP. Re-run the
underlying ordered CPUMAP regressions.
