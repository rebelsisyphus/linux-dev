# CPUMAP RFS infrastructure v1

独立四补丁系列，基于 Linux 本地 master
`9b87fdc9af2fbfcdb5c24a64139685ef80f6573f`。
分支：`develop/rfs_bpf_cpumap`，末端：`7cbebe4694ff2abff42acec6eb5f7678d69beb57`。
源码：`/home/sisyphus/code/worktree/rfs_bpf_cpumap`。

| 补丁 | 提交 | 内容 |
| --- | --- | --- |
| 1 | 998b3d82d1e1 | XDP 访问现有 socket storage，附 TCP/IPv6 lookup 自测试 |
| 2 | 1a5083899c7d | CPUMAP 可选保序域、redirect/state kfunc、完成水位与生命周期 |
| 3 | 94fe954cada5 | 13 项 ordered CPUMAP selftests |
| 4 | 7cbebe4694ff | API、顺序/热插拔/卸载边界和性能成本文档 |

`series` 给出应用顺序。四个文件可通过 `git am` 按顺序应用到基线；
归档过程使用独立 Git index 验证结果树与末端提交一致，见 `apply-check.txt`。
旧 `rfs_bpf` backlog v3 不在本系列依赖中，旧源码及补丁保留。

数据面由 BPF 选择 domain 和候选 CPU，内核只负责受控投递与迁移记账。
独立于 CONFIG_RPS；map_extra=0 保留普通 CPUMAP，非零开启预分配保序域。
没有新增通用网络选核 hook 或新的内核五元组表。

`CONFIG_RPS=n` 完整内核构建及 KASAN/lockdep QEMU 测试通过：
3 组、15 子用例，0 skipped/failed。另有 RPS=y、INET=n、BPF_SYSCALL=n、
PREEMPT_RT=y 的相关对象编译验证。

完整报告：[验证](../../dev/validation.md)、[实现复核](../../dev/review-notes.md)、
[设计](../../design/overview.md)、[后续工作](../../design/todo.md)。
`checkpatch-*.txt` 保留原始检查结果，包含测试握手 volatile 的已说明警告。
不是实机性能结论，也不是完整的 RFS 选核应用或社区已接受方案。

恢复开发：`develop rfs_bpf_cpumap`。
构建测试 runner：`python3 dev/tools/build-tests.py`（在项目目录）。
再次 QEMU 测试需设置
`ROOTFS=/home/sisyphus/code/kernel-dev/rfs_bpf_cpumap/test/rootfs-checked.img`，
再运行 `develop rfs_bpf_cpumap test`。
