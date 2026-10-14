# v2 验证记录（2026-10-12）

内核基线 `9b87fdc9af2fbfcdb5c24a64139685ef80f6573f`；分支 `develop/rfs_bpf_cpumap`，末端 `354807ea5cc9dadf76ffc78eea5b00c3e41a0abc`。
外部策略 `/home/sisyphus/code/rfs-bpf`，末端 `aaf425c9c842ee58ab5f48e1bcd16cbdc47e0020`。旧 v1 分支与归档完整保留。
源文件、镜像、配置、测试程序及日志 SHA256 见 `v2-validation-manifest.json`。

## 已验证

| 项目 | 实际结果与证据 |
| --- | --- |
| RT 内核 | CONFIG_RPS=n、PREEMPT_RT=y、KASAN/PROVE_LOCKING=y、SCTP/MPTCP=y、IO_URING_ZCRX=y；完整构建 `rt-zcrx-build-20261012.log` |
| 内核测试 | run `20261012-130802-3831f6f1`：5 组/35 子项通过，0 skip；含 ordered 18、组合 10、SCTP 5 和原有 socket-storage/attach 测试 |
| 最终组合夹具 | 强化 GRO 数据 CPU 断言后，`core-composition-final-retest-20261012.log`：1 组/10 子项通过，0 skip/失败 |
| 外部完整策略 | `completion-retest-20261012.log`：1 组/16 子项通过，0 skip/失败；运行实际外部 BPF 源码，含真实 MPTCP 无 fallback 检查 |
| 加载器 | 原 RT run 中 lifecycle、RPS/RFS 排他、hotplug（含 SIGSTOP 跨完整周期）、hardware pin ABI/代际/清理、三种 mode/真实 load sampler 全部通过；最终 BPF 再测 lifecycle 通过 |
| 可选协议关闭 | run `20261012-130803-05922fcd` 原加载失败；修复空 MPTCP BTF 类型后 `optional-mptcp-retest-20261012.log` lifecycle 通过；旧内核仅验证加载/排空/卸载，不声明数据收包 |
| i386 | run `20261012-124532-word32`：32 批交替 CPU 与 64 位完成快照 PASS；JIT/kfunc 运行，不等同于完整 32 位 socket 套件 |
| 配置编译 | SCTP=m + IPv6=n + BPF=y，以及 SCTP=y + BPF_SYSCALL=n + IPv6=n 子系统编译通过；未验证模块装卸运行 |
| 硬件控制器 | `external-control-tests-final-20261012.log`：15 个单元测试通过，含 ioctl/日志失败、外来规则、代际、恢复、设备更换与 UAPI 布局 |
| 性能采集工具 | `test/bench-tool-smoke-20261012` 只验证本地虚拟计数采集；没有将其当作物理性能结果 |

KVM 不可访问，本轮运行使用 QEMU TCG。RT lockdep bookkeeping 配置扩大到
CHAINS_BITS=18、STACK_TRACE_BITS=21，以容纳反复加载的 BPF trampoline/测试组合；
最终 `lockdep-core-after.txt` 的 debug_locks=1，dmesg 未见 KASAN/锁依赖/BUG/WARN。
所选测试不依赖 bpf_testmod，runner 的全局模块提示并未导致这些子项 skip。
`IO_URING_ZCRX=y` 下 observer 加载/attach 已覆盖，没有真实 NIC zero-copy 数据验证。

## 失败来源与对应修正

失败日志保持原样，不把整轮失败重新标成通过。

- generic-frags 实际收包失败：`skb_pp_cow_data` 漏拷贝 mac_len，导致 IP_INHDR；
  修复为独立前置补丁 2，真实 IPv4/IPv6 UDP 与外部 TCP/SCTP/MPTCP 回归通过。
- SCTP peeloff 夹具误把返回的新 fd 当作 0 成功值；修正为核对返回 fd 后通过。
- LLC 策略 MOD32 的 verifier 上半部范围问题：改用 64 位索引，运行策略测试通过。
- MPTCP accept/splice 绕过原 inet/TCP observer：新增专用观察，master hint 测试通过。
- CONFIG_MPTCP=n 仍暴露空 mptcp_sock 类型：CO-RE size guard 排除 storage 分支，
  同一对象在无 SCTP/MPTCP 内核上加载、排空和卸载通过。
- 硬件 pin 夹具的 /sys 挂载命名空间与 Python timeout 导致未调用 recv observer：
  改用独立 bpffs 挂载与真正的非阻塞 recv，ABI/代际和生命周期测试通过。
- 原 RT run 的外部 GRO 1 项失败：generic XDP detach 后，原 last-packet CPU
  会被本地 TCP 控制包更新。现在分别记录数据 CPU，并累计每个偏离目标的数据包，
  保留真实 GRO、载荷及迁移断言。最终外部 16 项和内核组合 10 项均通过。
- `completion-retest` 第一条命令误选旧 runner，得到 0/0（该脚本仍返回失败）；
  此结果不计入覆盖。最终独立命令使用 `dev/selftests-rfs/test_progs`，
  对应 `test_core_final` SHA256，明确跑到 10 项全部通过。

早期 RT 容量不足、SSH 超时等调查在 `rt-lockdep-check/` 及 progress.md；
只调整隔离测试配置和连接等待，没有屏蔽内核诊断或降低功能断言。
本轮调试 VM 已停止，失败磁盘与原始日志保留。

## 补丁、构建与可复现性

8 补丁逐个应用到独立 index，每一步树均与对应提交一致，最终树等于工作树；
`v2-apply-check.json` 同时证明 ordered CPUMAP 机制 diff 与 v1 未改变。
最终自测试 CPU 观测修正已折入组合测试补丁，没有追加 fixup 补丁。
完整策略按 policy/硬件控制器/测试与测量分为外部仓库 3 个提交。

构建入口：`dev/tools/build-tests.py`（输出 selftests-rfs）、外部 `tests/build.py`、
外部 Makefile；使用 clang-18、项目 libbpf/bpftool 与已完成的 RT vmlinux BTF。
`dev/tools/test-rt.py` 仅延长 TCG 的 SSH/启动等待，保留 smoke 和诊断检查。
`test/word32/init.c`、`run.py` 是独立 i386 initramfs 运行工具。

镜像在本地提交重组前构建，UTS_RELEASE 仍为 `7.3.0-rc3-cpumap-rfs-v1+`；
镜像 SHA256 与 run.json 相同，重组后变化为自测试/文档和提交历史，内核运行代码未改。
旧 `dev/build.json` 未冒充本轮 RT 构建记录。

严格 checkpatch 使用 `--strict --no-signoff`：0 error、0 CHECK，保留 11 个 warning
（已有所有者覆盖的新文件提示及 BPF volatile 用法），人工逐项核查见 review notes。
新/重写提交带 Assisted-by，没有代填人的 DCO；未修改原提交已有的来源标记。

## 未验证或未承诺

没有物理 NIC、发包端或吞吐/尾延迟对照数据；不宣称性能提升或无劣化。
硬件后端为 RXNFC/ntuple，不是原生 ndo_rx_flow_steer/filter-id 到期集成。
真实规则命中/固件行为、zero-copy RX、多路径/多宿主流量、模块装卸仍待测试。
硬件在 XDP 之前的乱序不能由软件 barrier 消除；无停流交接、特殊 demux 和
任意策略 watchdog/自动接管仍是文档明确的边界。

最终外部测试提交只调整 4 处换行/缩进以清理 checkpatch CHECK，C 源 token 相同；
没有为纯排版重新运行功能测试。测试快照提交仍为
`57de598224fd3ae5314d0a7bce192a3a9ae3256c`，保存在外部仓库
`archive/tested-before-style-20261012`。`external-fixture-format-proof.json` 记录
前后 SHA 与 token 一致性，BPF/loader/硬件代码没有变化。
