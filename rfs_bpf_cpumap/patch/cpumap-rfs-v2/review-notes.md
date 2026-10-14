# v2 implementation review

Scope: CPUMAP mechanism is unchanged from v1; expanded selftests, standalone
composition test, skb page-pool MAC-length fix, SCTP lookup, and external policies.
Source navigation used the worktree Semcode index at 9de54e62c380 plus direct
reads of all uncommitted implementations. Review is local, not maintainer approval.

## Changes and reachable paths

1. Generic XDP with BPF_F_XDP_HAS_FRAGS reaches skb_pp_cow_data when copying is
   necessary. The old skb mac_len is not in __copy_skb_header's headers group.
   CPUMAP generic redirect pulls mac_len; without the copy it sends an Ethernet
   header to ip_rcv_core. kfree_skb tracing recorded IP_INHDR. The fix preserves
   that scalar, with no new ownership/locking transition. Native veth is the other
   caller and preserves the same packet/header geometry. Original non-frags
   generic XDP remains covered. This is a functional bug, with no demonstrated
   new privilege boundary violation or kernel memory corruption.
2. SCTP lookup validates tuple size and obtains ingress dev/netns/interface scope.
   sctp_addrs_lookup_transport runs under RCU and holds the transport; transport
   ownership pins the association. sctp_assoc_migrate can replace base.sk during
   accept/peeloff. Its callers retain the old file/socket/endpoint while migrating,
   and endpoint final socket release occurs in sctp_endpoint_destroy_rcu. The new
   endpoint already holds its socket. READ_ONCE plus an own sock_hold under RCU
   therefore protects either snapshot. Release transport before RCU unlock, return
   own socket reference, require KF_RELEASE on all BPF paths. No helper sleeps.
   The lookup does not validate SCTP packets or promise final demux/path ordering.
3. BPF_SYSCALL=n uses a stub. SCTP module registration is owned by THIS_MODULE;
   check_btf_kconfigs allows absent optional BTF without making SCTP fail to load.
   Registration errors unwind IPv6/IPv4 protocol and per-net registrations. There
   is no later fatal init error after successful kfunc registration. No Kconfig
   symbol or scheduler/RPS dependency is added.
4. MPTCP's accept and splice operations bypass inet_accept/tcp_splice_read.
   External observers cover those targets, and existing recv/send observers
   cover the master socket. Tests assert no MPTCP fallback. The XDP helper reads
   the master while the looked-up subflow reference remains held; each subflow
   has its own tuple domain. Optional observers are disabled explicitly by BTF
   availability. io_zcrx uses the public wrapper at exit, excluding unsupported
   protocols; its real NIC receive path remains untested.
   With CONFIG_MPTCP=n the helper emits an empty mptcp_sock BTF type. Disabling
   tracing alone does not remove the invalid storage-pointer verifier branch.
   A CO-RE type-size guard prunes that branch; loader lifecycle was retested on
   the configuration with neither SCTP nor MPTCP enabled.
5. CPU hotplug withdraws eligibility before replacement, then republishes after
   successful entry creation. The uevent listener precedes the first snapshot;
   overflow rebuilds configured online entries. An offline/online cycle while
   the controller is SIGSTOP'ed is covered. Failure keeps a CPU ineligible and
   retries, without a local PASS path. Initial-userns uevent visibility is a
   documented control-plane requirement.
6. LLC load sampling uses /proc/stat and sysfs, without scheduler-private state.
   At most eight candidates and fixed-size maps bound policy work. A 64-bit
   dynamic index avoids a verifier MOD32 high-half precision issue. Choice-lock
   sections contain no helpers; eligibility and telemetry are advisory snapshots.
   Stale telemetry, dwell time and improvement threshold are exercised. This
   does not guarantee globally balanced load or eliminate cross-flow contention.
7. RXNFC uses explicit operator-owned locations and CPU/queue mappings. Durable
   pending intent precedes ioctl; exact readback precedes active state. Deletion
   confirms ownership and absence before reuse. Failures retain recovery records.
   Runtime device-identity changes prevent automatic cleanup on a replacement
   device. The exclusive-writer contract is required because RXNFC has no CAS.
   BPF intent map exhaustion affects acceleration only. CPU changes and rule
   movement retain software CPUMAP admission; no pre-XDP wire-order guarantee.
8. The TCP GRO fixture detaches generic XDP before native injection. The local
   peer can emit pure ACKs outside CPUMAP; a last-packet CPU includes those
   controls and is not a data-placement assertion. Track the data CPU separately
   and count every data packet seen off the expected CPU during each burst.
   Keep payload delivery, CPU migration and actual GRO segment assertions.

## Style and series

Strict checkpatch has no errors or CHECK findings in the kernel series. Remaining
warnings are new-file MAINTAINERS reminders (the files match existing BPF/SCTP/
selftests ownership) and deliberate BPF const-volatile configuration/single-copy
aligned advisory hint accesses. No lock ordering or synchronization is inferred
from volatile. External userspace sig_atomic_t is the signal-handler use case.
New/reworked commits carry Assisted-by: LLM and do not fabricate a human DCO.
Existing unchanged commits retain their original provenance.

The series removes the full sample/controller from kernel submission, retaining
an independent composition selftest. The MAC-copy bug is an independent fix,
not a new fixup appended to an old feature patch. SCTP forms an optional pair.
No external messages were sent. Final validation and artifact tree equality are
recorded separately in v2-validation.md and v2-apply-check.json.
