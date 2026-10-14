# CPUMAP + BPF RFS v2 本地 RFC

内核分支 `develop/rfs_bpf_cpumap`，末端 `354807ea5cc9`。
基线 `9b87fdc9af2fbfcdb5c24a64139685ef80f6573f`，全系列 8 补丁。
外部策略 `/home/sisyphus/code/rfs-bpf`，末端 `aaf425c9c842ee58ab5f48e1bcd16cbdc47e0020`。
这是重组系列，从基线应用；不要在旧七补丁之后追加。旧 v1 保留在
`archive/rfs-bpf-cpumap-v1-20260928`，旧归档也未改动。

| 补丁 | 作用 |
| --- | --- |
| 1 | XDP 访问 socket storage |
| 2 | 修复 generic XDP page-pool 复制遗漏 mac_len |
| 3 | 可选 ordered CPUMAP domain、迁移与完成水位 |
| 4 | ordered 机制、普通模式阳性对照、故障/热插拔测试 |
| 5 | 独立 RFS 组合测试，真实 TCP GRO 与 generic-frags 收包 |
| 6 | CPUMAP 接口、完成边界、性能成本与使用约定 |
| 7 | SCTP 带引用的关联 socket lookup/release kfunc |
| 8 | SCTP IPv4/IPv6、并发 peeloff 与引用释放测试 |

`kernel/series` 给出顺序。`apply-check.json` 验证每个提交树与独立 index 的
顺序应用结果一致；ordered 机制 diff 与 v1 相同。补丁 7–8 可以单独讨论。
内核 selftests 不依赖外部策略源码；完整 policy/loader 已移出 kernel samples。

外部仓库分 policy/loader、RXNFC 控制器、测试/基准工具 3 个提交。
`external/rfs-bpf.bundle` 可直接 git clone，`external/rfs-bpf.tar.gz` 仅含已提交源码。
实现 hash、socket RFS、LLC 负载避让、SCTP/MPTCP、热插拔以及 socket/策略 CPU
驱动的 ntuple 意图。硬件后端为 RXNFC，不是原生 aRFS filter-id 到期接口。

验证包括 RT 内核 35 子项、最终组合 10 子项、外部策略 16 子项、硬件控制器 15
单元测试、i386 水位运行及可选协议关闭加载。最终 dmesg 无新的 KASAN/锁依赖告警，
lockdep 有效。不同快照、失败及重测的准确范围见 `validation.md`，不能将原始失败
run 作为整轮通过。没有物理网卡性能、实际规则命中或真实 io_zcrx 数据验证。

`cover-letter.txt` 是本地讨论稿。`checkpatch.txt` 使用 --strict --no-signoff，
0 error/0 CHECK，剩余提示说明在 `review-notes.md`。新/重写提交未代填人的 DCO；
投稿前须开发者独立审核并签署，同时补公开外部仓库引用。没有发送邮件或发布远端。

可复现来源还包括 `../../dev/tools/build-tests.py`、外部 `tests/build.py`、
`../../dev/tools/test-rt.py`、`../../test/word32/` 及此目录保存的配置。
原始 clean 工作树归档：`../20261012-132141-befa0c78/`。
设计和待测清单：`../../design/todo.md`。
