#!/usr/bin/env python3
"""Render the local report and a reviewable commit-message addition."""
import json
from pathlib import Path
from bench import OUT

dest = Path(__file__).resolve().parents[1] / 'test-results/redis-numa'
d = json.loads((OUT / 'summary.json').read_text())
m = d['median']
names = {'baseline': '基线（RPS/RFS 关闭）', 'rfs': '普通 RFS',
         'rps': 'RPS → CPU5', 'bpf-numa': 'BPF-RFS NUMA → CPU5'}
cpus = {'baseline': 0, 'rfs': 4, 'rps': 5, 'bpf-numa': 5}
lines = ['# Redis 双 NUMA 性能测试', '',
         '2026-09-07，使用 `test-kernel` 的隔离 QEMU/KVM 环境，完成四种模式各三轮压测。',
         '以下只统计最终的 384 连接测试，早期校准和带探针的验证均不计入 QPS。', '',
         '## 配置和负载', '',
         '- 宿主机：AMD Ryzen 7 9700X，WSL2 Linux 6.6.87.2，16 个逻辑 CPU，**1 个物理 NUMA 节点**。',
         '- 客体：8 vCPU、2 个虚拟 NUMA 节点（0–3 / 4–7）、4 GiB RAM；内核 `7.2.0-rfs-perf+`。',
         '- KASAN、lockdep、锁调试关闭；`CONFIG_IRQ_TIME_ACCOUNTING=y`，HZ=1000、PREEMPT_DYNAMIC。',
         '- 单队列 virtio-net + vhost-net + 独立 TAP；MTU 1500。数据流不经过 SLIRP/SSH 端口转发。',
         '- RX/TX 中断固定 CPU0/node0；Redis 固定 CPU4/node1，并使用 `numactl --membind=1`。',
         '- RPS 与 BPF NUMA 策略均选 CPU5/node1；普通 RFS 跟随应用提示到 CPU4。',
         '- RFS 两种模式：全局表 32768、RX 表 4096、RPS mask=0；RPS 模式：表关闭、mask=0x20。',
         '- 使用已提交的 NUMA sample，含原有 per-CPU 统计，候选列表为 `-c 5`。',
         '- Redis/redis-benchmark 7.2.5，静态构建、MALLOC=libc；I/O threads=1、关闭 RDB/AOF。',
         '- GET 固定的 64 字节值；3 个客户端线程、384 连接、pipeline=8；每轮预热 50 万，再计时 2000 万请求。',
         '- Host 绑核：vCPU0→CPU0、vCPU4→CPU2、vCPU5→CPU4；客户端 CPU6/8/10，vhost CPU12。热核使用不同物理 core。',
         '- 三轮顺序：baseline/RFS/RPS/BPF；BPF/RPS/RFS/baseline；RPS/baseline/BPF/RFS。', '',
         '```sh', 'taskset -c 6,8,10 redis-benchmark -h 192.0.2.2 --threads 3 \\',
         '  -c 384 -P 8 -n 20000000 --csv GET rfs-key', '```', '',
         '## 结果', '',
         'QPS、P99 和 CPU 百分比均为三轮各自测量值的中位数；QPS 范围为三轮最小值至最大值。',
         '整核忙碌度包括 user/system/IRQ/softirq，排除 idle/iowait/steal；Redis 进程占用单独列出。',
         'CPU 统一使用原始每秒采样的中央 50% 窗口，避免开始和退出阶段的空闲。P99 的中位数不是合并请求分布后的 P99。', '',
         '| 模式 | TCP 收包 CPU | QPS 中位数 | QPS 最小–最大 | 相对基线 | P99 ms |',
         '|---|---:|---:|---:|---:|---:|']
for mode, v in m.items():
    lines.append(f"| {names[mode]} | {cpus[mode]} | {v['qps']:,.0f} | {v['qps_min']:,.0f}–{v['qps_max']:,.0f} | {v['vs_baseline_percent']:+.2f}% | {v['p99_ms']:.3f} |")
lines += ['', '| 模式 | CPU0 忙碌 % | CPU4 忙碌 % | CPU5 忙碌 % | Redis 进程 % | CPU4 softirq % |',
          '|---|---:|---:|---:|---:|---:|']
for mode, v in m.items():
    lines.append(f"| {names[mode]} | {v['cpu0_busy']:.2f} | {v['cpu4_busy']:.2f} | {v['cpu5_busy']:.2f} | {v['redis_cpu']:.2f} | {v['cpu4_softirq']:.2f} |")
lines += ['', '独立探针窗口中的实际 TCP 收包分布：', '',
          '| 模式 | 预期 CPU | 该 CPU 占比 | CPU0 skb 数 | 预期 CPU skb 数 |',
          '|---|---:|---:|---:|---:|']
for mode in m:
    v = json.loads((OUT / f'verify-{mode}.json').read_text())
    counts = v['tcp_rx_skb_counts']
    lines.append(f"| {names[mode]} | {cpus[mode]} | {v['on_expected_percent']:.3f}% | {counts[0]} | {counts[cpus[mode]]} |")
lines += ['', 'RFS/BPF 中少量 skb 仍在 IRQ CPU0 处理，与原生 miss/回退路径兼容；本探针没有进一步区分每个回退原因。']
lines += ['', f"BPF-RFS 相对普通 RFS 的 QPS 变化为 **{d['bpf_vs_rfs_percent']:+.2f}%**，相对静态 RPS 为 **{d['bpf_vs_rps_percent']:+.2f}%**。",
          '普通 RFS 在应用核上处理 TCP 收包，软中断会占用该核的执行时间；BPF NUMA 策略将 TCP 收包转到同虚拟节点的另一核。',
          '固定一个应用核、一个目标核时，静态 RPS 已能表达同一映射；本测试不证明 BPF 比静态 RPS 更有吞吐优势。', '',
          '## 有效性核验', '',
          '- 最终内核的 `test-kernel` 启动、SSH、9p 和 22/22 功能自测通过，无跳过。',
          '- 单独的 5 秒 `tcp_v4_rcv()` 探针窗口确认 TCP/6379 在预期 CPU；正式计时前已卸载探针。',
          '- 每轮预热后确认 RX/TX IRQ 的 effective affinity，正式窗口的 RX/TX IRQ 增量全部位于 CPU0。',
          '- 每轮 CPU4 忙碌度均至少 98%；这里达到饱和的是应用所在核，并未声称 IRQ 核和 CPU5 也同时 100%。',
          '- 12 轮均校验 Redis GET 处理计数，keyspace miss 和 error reply 增量为 0。',
          '- 原始记录包含每核 CPU、Redis 进程 CPU、host CPU/客户端/QEMU 线程、IRQ/softirq、softnet、TCP 计数及 INFO。', '',
          '## 解释范围', '',
          '宿主机只有一个物理 NUMA 节点，客体中的两个节点由 QEMU 创建。本结果说明该虚拟环境中的选核行为和端到端性能，**不能证明真实跨 NUMA 内存访问或物理 NIC 的收益**。',
          '基线、RPS 与 BPF 的微小差异还受虚拟化、调度、cache 和后台负载影响；三轮结果不是严格的统计显著性结论。宿主机原有服务和其他 VM 保持运行。',
          '尚未覆盖真实 NIC/aRFS、多 RX 队列、真实 NUMA/SMT、多 Redis 实例、跨节点应用迁移和长时间拥塞；未隔离测量单次 BPF hook 成本或 CONFIG_BPF_RFS 关闭时的回归。', '',
          '校准阶段发现并修正了两项测量条件：自测 CPU 热插拔会重置 virtio IRQ 亲和性；未开启 IRQ 时间统计时，tick 采样会漏记短软中断。',
          '最初的采集窗口混入客户端退出后的空闲，曾把 CPU4 稳态占用低估为约 95%–97%。逐秒数据确认了这一点，最终统一用中央 50% 窗口重算 CPU 数据，QPS 保持工具原始值。',
          '192 连接初始组保留在 initial-c192 中；正式结果只用完整的 384 连接组，没有按 QPS 高低筛选轮次。旧窗口数值保留为 initial_window_metrics 供核对。', '',
          '## 产物', '',
          '- [逐轮 CSV](raw/summary.csv)、[汇总 JSON](raw/summary.json)、[完整原始记录](raw/)。',
          '- [测试流程](test-kernel.log)、[收包 CPU 验证](verify-rx.log)、[逐轮输出](runs.log)。',
          '- [性能内核配置](perf-kernel.config)、[二进制校验和](sha256sum.txt)、[host 绑核](host-affinity.json)。',
          '- [测试脚本及复现说明](../../perf/README.md)。原内核和 sample 源码未修改。', '']
(dest / 'report.md').write_text('\n'.join(lines))

table = []
for mode, label in [('baseline', 'No RPS/RFS'), ('rfs', 'Native RFS'), ('rps', 'RPS CPU5'), ('bpf-numa', 'BPF NUMA CPU5')]:
    v = m[mode]
    table.append(f"  {label:<15} {v['qps']:>9.0f} {v['cpu0_busy']:>6.1f} {v['cpu4_busy']:>6.1f} {v['cpu5_busy']:>6.1f}")
paragraph = '''Preliminary full-series Redis benchmark (three runs per mode): use
test-kernel with QEMU/KVM, eight vCPUs, two virtual NUMA nodes and one
virtio-net queue over TAP/vhost-net. Pin RX/TX IRQs to CPU0/node0 and
Redis CPU/memory to CPU4/node1. Native RFS follows CPU4; RPS and the
NUMA sample with candidate CPU5 use CPU5/node1. Enable RFS tables of
32768 global/4096 per queue for the two RFS modes. RPS uses mask 0x20
with RFS disabled. Disable KASAN/lockdep and enable IRQ time accounting.

Redis 7.2.5, one I/O thread, libc allocator, no persistence, 64-byte GET:
redis-benchmark --threads 3 -c 384 -P 8 -n 20000000, after 500000 warmup
requests. Rotate mode order and keep the load generator on separate host
cores. QPS and per-CPU busy percentages below are medians of three runs:

  Mode                  QPS   CPU0   CPU4   CPU5
''' + '\n'.join(table) + f'''

BPF NUMA changes QPS by {d['bpf_vs_rfs_percent']:+.1f}% versus native RFS and {d['bpf_vs_rps_percent']:+.1f}% versus
static RPS. CPU4 is 99.4-99.9% busy in every timed run, using the central
half of per-second samples. Separate TCP receive probes put at least
99.88% of skbs on the intended CPUs and are detached before timing;
RX/TX IRQ deltas remain on CPU0. There are no GET misses or error replies.

The AMD Ryzen 7 9700X host runs WSL2 and has one physical NUMA node.
These virtual-topology measurements do not establish physical NUMA
locality gains or isolated callback cost. Static RPS is a control for
the same fixed CPU mapping; small differences need more samples.
'''
(dest / 'commit-benchmark.txt').write_text(paragraph)
