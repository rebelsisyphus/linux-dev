# Redis / NUMA benchmark harness

This harness uses the isolated `test-kernel` wrapper in
`../run-test-kernel.sh`. It changes no kernel or sample sources. The final
results and environment snapshots are in `../test-results/redis-numa/`.

Topology:

```text
host redis-benchmark (host CPUs 6,8,10; three client threads)
    -> TAP rfsp-tap (192.0.2.1/30), vhost-net (host CPU12)
    -> guest virtio-net, one RX/TX queue (192.0.2.2/30)
         IRQ: guest CPU0 / NUMA node0 / host CPU0
         Redis: guest CPU4 / NUMA node1 / host CPU2
         RPS / BPF NUMA candidate: guest CPU5 / node1 / host CPU4
```

The guest has 8 vCPUs, 2 sockets, 4 cores per socket, and 4 GiB RAM split
equally between two NUMA nodes. Host CPUs 0, 2 and 4 are separate cores.
The host has **one physical NUMA node**, so these are virtual topology
results, not evidence of physical remote-memory savings. The management
e1000 NIC uses a separate user-mode network and SSH port 2246.

`guest-setup.sh` configures the data NIC and requests CPU0 for its MSI-X interrupts
to CPU0. `start-redis.sh` runs one Redis command/I/O thread, pins CPU and
memory allocation to CPU4/node1, and disables persistence. The key
`rfs-key` contains exactly 64 bytes. `set-mode.sh` selects:

| Mode | RFS global/per-queue entries | RPS mask | Policy |
|---|---|---|---|
| baseline | 0 / 0 | 0 | none |
| rfs | 32768 / 4096 | 0 | native RFS, app CPU4 |
| rps | 0 / 0 | 0x20 | static CPU5 |
| bpf-numa | 32768 / 4096 | 0 | sample NUMA policy, candidate CPU5 |

`set-mode.sh` reasserts interrupt affinity after the selftest's CPU
hotplug operations. After warmup, effective RX/TX interrupt affinity is
checked again. An unused configuration vector may retain its old
effective CPU until it next fires; it carries no data traffic.

RFS misses remain on the IRQ CPU in both RFS modes. Existing flow tables
are cleared at each mode change. The sample is the committed sample,
including its per-CPU statistics; no reduced-cost benchmark policy is
substituted.

`bench.py MODE LABEL -n REQUESTS -c CLIENTS -P PIPELINE` runs a warmup and
one measured GET test, saves the exact argv and Redis CSV latency/QPS,
and samples guest and host CPU data once per second. It saves INFO
before/after for successful-command, hit, miss and error checks. The
reported guest CPU percentages use the central half of the samples when
the run is long enough; the raw samples retain the full interval. This
uniform window excludes polling and client teardown idle time. Busy
CPU includes user, nice, system, IRQ and softirq time, excluding idle,
iowait and steal. Redis process CPU is reported separately.

`verify-rx.py` runs a separate instrumented stream. `rx_cpu.bpf.c` counts
IPv4 TCP handler entries with destination port 6379 by CPU. The probe
counts skbs at `tcp_v4_rcv()`, not Redis commands or wire packets. These
diagnostic runs are excluded from QPS results, and the probe is detached
before timing runs. IRQ counter deltas provide an independent check of
the interrupt CPU.

Use `pin-vm.py` after boot to pin this VM's vCPU, emulator and vhost
threads. It identifies only the QEMU PID saved by this harness. Other
running VMs and host services are left intact; their activity is a
source of measurement noise.

Redis 7.2.5 is built from
<https://download.redis.io/releases/redis-7.2.5.tar.gz> with
`MALLOC=libc BUILD_TLS=no USE_SYSTEMD=no REDIS_CFLAGS='' REDIS_LDFLAGS=''
LDFLAGS=-static`. The source archive SHA-256 is
`5981179706f8391f03be91d951acafaeda91af7fac56beffb2701963103e423d`.
Numeric IP addresses and no loadable modules are used. The benchmark
method follows Redis's guidance to use a consistent workload and enough
concurrency/pipelining to avoid measuring only request/response RTT:
<https://redis.io/docs/latest/operate/oss_and_stack/management/optimization/benchmarks/>.

The performance kernel has KASAN, lockdep and lock debugging disabled and
`CONFIG_IRQ_TIME_ACCOUNTING=y` to account for hardirq/softirq execution;
the saved `.config` is authoritative. The existing debug kernel in the
main checkout is retained. Preliminary logs prefixed `prepare-` are
tool/guest setup checks on that debug kernel and are **not** performance
results. Files prefixed `cal-` are also excluded: they are calibration,
including early checks before restoring IRQ affinity and enabling IRQ
time accounting. Only runs explicitly listed in the final report belong
to the result set.

## Reproduction in this workspace

The saved configuration can be used with an isolated source worktree:

```sh
git worktree add --detach /tmp/rfs-perf-src HEAD
mkdir -p /tmp/rfs-perf-build
cp rfs-dev/test-results/redis-numa/perf-kernel.config /tmp/rfs-perf-build/.config
make -C /tmp/rfs-perf-src O=/tmp/rfs-perf-build olddefconfig
make -C /tmp/rfs-perf-src O=/tmp/rfs-perf-build -j10 bzImage
```

The current shared fixture is `/home/sisyphus/code/test/rfs-perf/`, with
`bin/`, `numa/` (unpacked Ubuntu numactl/libnuma packages), the installed
`selftests/` from `tools/testing/selftests/bpf/rfs`, and copies of the
guest shell/Python scripts. Its `test.sh` runs guest setup and the
22 functional selftests. `set-mode.sh` restores IRQ affinity afterwards.
The original guest disk is used as the backing file for a disposable
qcow2 overlay. The source/build trees and binaries remain in `/tmp` for
reuse after this run.

The sample builds using `samples/bpf/Makefile.rfs`. The TCP receive probe
is built with `clang-18 -target bpf -D__TARGET_ARCH_x86 -O2 -g`, the sample
build's `vmlinux.h` and generated libbpf headers. Its userspace loader
links against the same in-tree `libbpf.a`, `libelf` and `zlib`. Build a
full bpftool with:

```sh
make -C tools/bpf/bpftool OUTPUT=/tmp/rfs-perf-tools/bpftool/ -j4 \
  CLANG=clang-18 LLVM_STRIP=llvm-strip-18
```

Create the TAP once, then boot and run; use an unused interface name and
SSH port if these already exist. The harness defaults below match the
saved run. `bench.py` refuses to overwrite completed result labels, so
archive or move previous `results/` before repeating the series.

```sh
ip tuntap add dev rfsp-tap mode tap
ip address add 192.0.2.1/30 dev rfsp-tap
ip link set rfsp-tap up
RFS_TAP=rfsp-tap RFS_KEEP_VM=1 RFS_SSH_PORT=2246 \
  bash rfs-dev/run-test-kernel.sh /tmp/rfs-perf-build/arch/x86/boot/bzImage \
  /home/sisyphus/code/test/rfs-perf
python3 rfs-dev/perf/pin-vm.py
ssh -p 2246 root@localhost 'bash /mnt/shared/start-redis.sh'
python3 rfs-dev/perf/verify-rx.py
bash rfs-dev/perf/run-series.sh
python3 rfs-dev/perf/analyze.py
```

The timed client command in each of the 12 runs is:

```sh
taskset -c 6,8,10 redis-benchmark -h 192.0.2.2 --threads 3 \
  -c 384 -P 8 -n 20000000 --csv GET rfs-key
```

Each run follows 500,000 warmup requests. Order is baseline/RFS/RPS/BPF,
then BPF/RPS/RFS/baseline, then RPS/baseline/BPF/RFS. The report uses the
median of three runs per mode and retains the min/max range. Stop only
the QEMU PID from `vm-directory.txt` and remove this task's TAP when
finished. Do not use a global QEMU process kill on a shared host.
