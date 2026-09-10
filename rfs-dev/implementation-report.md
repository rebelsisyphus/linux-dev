# 原生 RFS BPF 策略实现与测试记录

2026-09-07，基于 `45c13f3f9e3b`。第一版已实现，并通过 `test-kernel`
启动新内核执行功能验证。内核接口、示例、自测和正式文档已提交至
`rfs_select_cpu` 分支，共四个提交：

- `e37739322a0c` net: rfs: Add a BPF struct_ops hook for CPU selection
- `ba5347a259ea` samples/bpf: Add topology-aware RFS CPU policies
- `75c47d5b969c` selftests/bpf: Cover native RFS CPU selection and policy lifetime
- `9f3cd6a8fd24` docs: networking: Describe BPF CPU selection for RFS

提交使用当前 Git 身份 `Dong Chenchen <superdcc97@163.com>`，按用户提供的
AGENTS.md 添加 Signed-off-by，并包含 Assisted-by。代码内容与上述功能验证
一致，本次整理提交未改变代码逻辑。

逐提交 strict checkpatch 均为 0 errors；剩余提示是新增文件归属确认、
BPF `const volatile`、共享 flag 的 BIT 宏风格、Makefile 中的构建命令和
合法 `mount -t bpf bpf` 命令的重复单词提示。新增文件由现有网络、BPF 和
自测维护者规则覆盖。SPDX 检查依赖在临时目录补齐后，检查完整执行。
详见 [提交检查记录](test-results/native-bpf/commits/)。

## 已实现内容

- 增加 `CONFIG_BPF_RFS` 和 `bpf_rfs_ops` struct_ops 接口。在
  `get_rps_cpu()` 原生 RFS 命中后、迁移检查之前调用 BPF，只改变期望 CPU。
  active CPU、`last_qtail`、backlog 和 aRFS 继续由原生路径管理。
- 回调只接收标量快照，verifier 禁止写 context、禁止 sleepable 回调。
  返回 0 表示原生选择，CPU + 1 表示请求该 CPU；无效或离线结果回退到
  原生候选，并继续通过原有迁移检查。
- 每个设备一个 link，支持 pin、expected-old 原子更新、显式 detach。
  RTNL 串行化控制面，RCU 保护数据面；设备注销时撤销绑定，旧 link 保留到
  用户释放，后续更新返回 ENODEV。失效绑定不持有设备引用。
- 示例提供 process、cluster、NUMA 三种策略及可选 SMT sibling 排除。
  CPU 列表动态解析，拓扑按共享分组存储，配置 map 填充后冻结。
  配置刷新通过新程序和整套 map 更新，不原地修改正在使用的拓扑。
- 增加独立 sample 构建文件、22 项 kselftest、内核使用文档与 VM 测试脚本。

主要入口：

| 内容 | 文件 |
| --- | --- |
| 内核接口 | [include/net/bpf_rfs.h](../include/net/bpf_rfs.h) |
| 注册、校验、选 CPU 和生命周期 | [net/core/bpf_rfs.c](../net/core/bpf_rfs.c) |
| RFS 挂载点 | [net/core/dev.c](../net/core/dev.c) 的 `get_rps_cpu()` |
| BPF 策略 | [samples/bpf/rfs_policy.bpf.c](../samples/bpf/rfs_policy.bpf.c) |
| 管理 CLI | [samples/bpf/rfs_policy.c](../samples/bpf/rfs_policy.c) |
| 拓扑发现 | [samples/bpf/rfs_policy_topology.c](../samples/bpf/rfs_policy_topology.c) |
| 自测 | [tools/testing/selftests/bpf/rfs/](../tools/testing/selftests/bpf/rfs/) |
| 用法和接口约定 | [Documentation/networking/bpf_rfs.rst](../Documentation/networking/bpf_rfs.rst) |

## 验证结果

测试内核为 `7.2.0-rfs-bpf+`，x86_64，开启 KASAN、PROVE_LOCKING、
LOCKDEP 和 PROVE_RCU。QEMU/KVM 使用 8 个 vCPU、2 个 NUMA node、4 GiB
内存，node 0 为 CPU 0–3，node 1 为 CPU 4–7。

| 验证 | 结果 |
| --- | --- |
| 新内核完整构建 | 通过，生成 `arch/x86/boot/bzImage` 与 `vmlinux` |
| 关闭 BPF_RFS 后构建 `net/core/dev.o` | 通过，目标文件无 `bpf_rfs` 符号 |
| sample、BPF 对象、selftests 构建及安装 | 通过 |
| 使用未提供新 RFS 类型的宿主机 6.6 BTF 构建 sample | 通过；这里只验证构建，不在宿主机挂载 |
| `test-kernel` 启动、SSH、共享目录、脚本执行 | 全部通过 |
| kselftest | **22 / 22 通过，0 失败，0 跳过** |
| 8 个 worker、512 次并发 fork/exec | 通过 |
| CLI attach → show → update → detach | 通过，更新保留 link ID 并更换 map ID |
| 重复 attach、更新到错误设备 | 按预期失败，原策略和 pin 保留 |
| 测后 dmesg | 未发现 KASAN、BUG、lockdep 锁序、RCU stall 或 WARNING 告警 |
| `git diff --check`、shell 语法检查 | 通过 |
| 新内核 C/header 的 strict checkpatch | 0 errors、0 warnings、0 checks |

22 项用例覆盖：大于 63 及稀疏 CPU ID 解析、未挂载、DEFAULT、无 RFS 表、
离线/越界 CPU 回退、CPU 0 编码、指定接收 CPU、IPv6、只读 context、
sleepable 拒绝、保留 flags、重复挂载、应用 CPU 迁移、连接中更新和卸载、
expected-old 不匹配、设备删除、16 轮设备删除与更新竞争、NUMA 迁移、
cluster/SMT 排除及空候选集合。

接收 CPU 通过真实 TCP 流量的 `SO_INCOMING_CPU` 检查。
测试日志中的只读/sleepable 加载失败、EBUSY 和 EINVAL 是负向用例的预期结果。
用户态/BPF checkpatch 剩余提示为 BPF `const volatile` 配置及本地 flag 的
BIT 宏风格；这些提示不代表编译或运行失败。

原始结果保存于 [test-results/native-bpf/](test-results/native-bpf/)，包括：
[完整流程](test-results/native-bpf/test-kernel.log)、
[逐项测试](test-results/native-bpf/functional.tap)、
[dmesg](test-results/native-bpf/dmesg.log)、构建日志、测试配置、修改前配置及
[产物校验和](test-results/native-bpf/sha256sum.txt)。

测试过程中修复了 struct_ops 描述符缺少必需 `.init` 引起的启动空指针问题，
记录见 [boot-debug/debug-report.txt](boot-debug/debug-report.txt)；最终镜像
已通过完整启动。另修复了 Clang 合并不同 BPF 指针来源的访问造成的 verifier
拒绝，以及 `ip netns exec` 重挂 sysfs 遮蔽 bpffs 的测试环境问题。

## 复现

在当前配置上构建内核和测试：

```sh
make -j16
make -C tools/testing/selftests/bpf/rfs \
  OUTPUT=/tmp/rfs-bpf-build/selftests \
  CLANG=clang-18 BPFTOOL=/home/sisyphus/code/test/bpf_test/bpftool \
  INSTALL_PATH=/home/sisyphus/code/test/rfs-native/bpf/rfs install
cp rfs-dev/guest-test.sh /home/sisyphus/code/test/rfs-native/test.sh
RFS_SSH_PORT=2244 bash rfs-dev/run-test-kernel.sh \
  arch/x86/boot/bzImage /home/sisyphus/code/test/rfs-native
```

[run-test-kernel.sh](run-test-kernel.sh) 调用已安装 `test-kernel` 的私有副本，
使用独立 rootfs overlay、端口和日志目录，结束后回收本次 VM。该机器的
原工具会清理全部 QEMU 进程，因此副本将清理范围限定为自己的 VM，并修正
原工具测试失败的退出码处理。原工具、基础 rootfs 和其他运行中 VM 未修改。
脚本默认依赖本机现有 rootfs，可通过 `RFS_ROOTFS` 指定其他 raw 基础镜像。

## 尚未覆盖与使用边界

- 已完成软件 RFS 功能验证及 Redis VM 的 QPS/P99 对比；未测真实 NIC/aRFS、
  多 RX 队列和独立的逐包 BPF 成本。VM 对比不构成物理 NUMA 收益证据。
- 未做持续 backlog 压力、tail 回绕、表碰撞、入队失败和协议重组前的序号
  观测。TCP 数据校验不等同于证明底层无乱序。原生迁移条件在源码中保持原样。
- 设备跨 netns 移动使用 NETDEV_UNREGISTER 清理路径，但本次没有独立迁移用例。
  大 CPU ID 验证是列表解析测试，VM 本身只有 8 个 vCPU。
  VM 每个虚拟 core 只有一个线程，尚未验证多线程物理 core 上的 SMT 排除效果。
- 没有新增迁移提交/延期 tracepoint；示例统计只描述策略调用和分支选择，
  不能当作实际迁移或成功入队计数。
- CPU 候选集合是策略偏好，最终回退可能返回原生 CPU，不构成硬隔离。
  拓扑热插拔后需显式 `update`；第一版没有自动监控 daemon。
- 原生全局表仍是 hash 提示，设备级绑定不会将其变为精确 socket 或 netns
  归属表。`show` 的 map ID 也不能证明设备仍在绑定。

已补充 Redis 双 NUMA VM 性能对比，见 [性能报告](test-results/redis-numa/report.md)。BPF-RFS QPS 中位数为 1,438,332，相对普通 RFS +32.1%，与静态 RPS 相当（-0.2%）。真实硬件、压力和诊断增强仍是后续验收目标。
