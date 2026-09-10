# RFS BPF 方案审查

审查日期：2026-09-10。结论：当前四个提交中，未发现可确认的内核正确性回归。现有方案可以继续作为 RFC 讨论和验证的基础；测试范围还不足以证明所有收包场景的保序和硬件 NUMA 收益。

本轮没有修改实现、已有 selftests 或 commit。新增内容仅为本目录下的审查记录、独立验证脚本和测试日志。根目录原有 review 文件已备份到 `previous-review/`。

## 审查范围

1. `e37739322a0c` — net: rfs: Add a BPF struct_ops hook for CPU selection
2. `ba5347a259ea` — samples/bpf: Add topology-aware RFS CPU policies
3. `75c47d5b969c` — selftests/bpf: Cover native RFS CPU selection and policy lifetime
4. `9f3cd6a8fd24` — docs: networking: Describe BPF CPU selection for RFS

按 kernel skill 读取完整 diff、相关子系统指南和调用链，并检查源码中的错误处理、对象所有权、并发时序与 Kconfig。Semcode 不可用，使用本地 `git show`、`rg` 和完整函数定义获取上下文。没有把其他审查工具的意见作为证据。

## 已确认的设计约束

| 检查项 | 结果与依据 |
|---|---|
| 接入点 | `get_rps_cpu()` 在全局 socket flow 表匹配后调用策略；缺表、hash miss、无有效 hash 的路径保留原行为。全局 RFS 表配置会启用原有 `rps_needed`，不需要靠 BPF attachment 启用收包路径。 |
| CPU 返回值 | DEFAULT=0，CPU n 编码为 n+1。内核检查 `nr_cpu_ids`、`RPS_NO_CPU`、possible/online；非法建议回退到原生提示。 |
| 迁移约束 | BPF 只替换 `next_cpu`。随后仍检查旧 CPU 和队列进度，才调用 `set_rps_cpu()`；没有直接改 `rflow->cpu`、队尾或全局应用提示。 |
| 读侧生命周期 | `netif_rx_internal()`、`netif_receive_skb_internal()`、`netif_receive_skb_list_internal()` 都在 RCU 读侧内调用 `get_rps_cpu()`。读取 ops 到回调返回均处于保护范围。 |
| 更新与释放 | 更新先发布新 ops，再等待 RCU；unreg 先清除 dev 指针并摘除 binding，再等待 RCU。struct_ops 核心在回调返回后释放旧 map 引用，map 最终释放还等待 RCU/Tasks RCU。 |
| 设备生命周期 | RTNL 串行化 binding 字段和设备查找。注销通知清空 `binding->dev`，后续更新不再解引用设备；没有长期 dev 引用阻塞设备/netns 销毁。通知器不进入 struct_ops 的 update mutex。 |
| verifier | 拒绝 sleepable callback；默认 BTF 访问路径拒绝写入该内核上下文，且上下文没有 skb/socket 内核指针。回调必填和保留 flags=0 也有校验。 |
| 权限 | 注册检查当前 netns 的 CAP_NET_ADMIN，更新检查实际绑定设备所在 user namespace 的权限。BPF 程序加载本身还受核心加载权限约束。 |
| 拓扑策略 | cluster、NUMA、应用 CPU、全局池和 DEFAULT 的回退顺序与文档一致；SMT 按 core 排序后形成连续排除区间；active CPU 只有满足当前组与候选条件才保留。 |
| 配置代际 | 用户态填充拓扑 map 后 freeze，再 attach/update；map 对 BPF 程序只读。替换使用新的程序/map 集，不向正在使用的拓扑写入半成品。 |
| Kconfig | 新符号及依赖存在，没有新 select 或依赖环；BPF_RFS 关闭时为内联原生回退。架构还需支持 BPF trampoline，文档已明确，不能把本次 x86_64 结果推广到所有架构。 |

相关源码：

- [原生 RFS 接入及迁移判断](/home/sisyphus/code/linux/net/core/dev.c:5223)
- [CPU 返回值验证](/home/sisyphus/code/linux/net/core/bpf_rfs.c:33)
- [binding 更新与注销](/home/sisyphus/code/linux/net/core/bpf_rfs.c:121)
- [struct_ops 核心更新和引用管理](/home/sisyphus/code/linux/kernel/bpf/bpf_struct_ops.c:1338)
- [默认 BTF 写入限制](/home/sisyphus/code/linux/kernel/bpf/verifier.c:6004)

## 本轮验证

重新构建当前 sample 和 selftests，使用 Clang 18、仓库内 libbpf、已有 bpftool；构建和安装通过。启动隔离的 test-kernel VM：8 vCPU、2 个虚拟 NUMA 节点、4 GiB 内存。

本轮复用此前构建的 `7.2.0-rfs-bpf+` 调试内核，没有重新编译内核。镜像 SHA-256 与此前验证记录一致：

```
a45d415b7238f7f94e815aee022bd43d4f8f4d8955513be2708b3df695477893
```

配置启用 KASAN、PROVE_LOCKING、PROVE_RCU、RPS 和 BPF_RFS。四个提交重写前后的 tree 相同，重写只调整提交描述。

- 原有 selftests：**22 pass，0 fail，0 skip**，见 [selftests.log](selftests.log)。
- 对已经准备好的 map，移除有效 CAP_NET_ADMIN 后 attach/update 均返回 EPERM。
- 第二个策略占用同一设备返回 EBUSY；update 到不同 ifindex 返回 EINVAL。
- 错误 expected-old 返回 EPERM；正确 expected-old 更新成功；detach 后旧 map 可复用。
- 设备移动到另一个 netns 后，旧 link update 返回 ENODEV，新策略可绑定移动后的设备。
- 保留 pinned links 时 netns 删除完成；复用 ifindex 也不会使 inactive link 重新生效。
- 没有观察到 BUG、KASAN、Oops、lockdep 或 RCU stall 报告；VM 已退出。

补充验证见 [lifecycle.log](lifecycle.log)、[test_result.txt](test_result.txt) 和 [dmesg.log](dmesg.log)。这些检查没有模拟并发 CPU 热拔插，也没有对注销瞬间的在途数据包进行完整压力测试。

## 优先改进点

以下是验证与可用性改进，不计为已经确认的内核 bug。

1. **补充迁移保序和竞争的压力验证。** 当前 TCP 测试只有一条连接、逐次请求/应答，且通过 SO_INCOMING_CPU 和重组后的 payload 判断结果。建议在真实接收路径制造 backlog，与策略更新/detach、应用迁移和 CPU hotplug 并发；用协议重组前的序列记录检查乱序。另补多队列和硬件 aRFS。代码保留原生检查，只能证明接入方式，不能替代这些测试。

2. **让 inactive 状态可查询。** `rfs_policy show` 目前只显示 link_id/map_id。设备移除后两个 ID 仍可存在，用户无法从 show 判断策略是否仍生效。建议增加实际绑定状态、netns/设备身份和代际信息；同时保留注销回调不获取 update mutex 的锁序。

3. **补充策略执行结果的观测。** sample 的 calls/defaults/proposed/kept_active 是策略层统计。proposed 不等于实际迁移：内核还可能拒绝离线 CPU，原生迁移门槛也可能暂时保留旧 CPU。建议按需记录 requested CPU、最终 CPU、fallback 原因和迁移暂缓原因，便于解释性能结果。

4. **完善 CPU hotplug 后的拓扑刷新。** 当前 snapshot+freeze+整代 update 的一致性做法合理，但刷新完全靠显式命令。CPU 下线后，旧拓扑可能反复选到离线候选，内核只能回退原生提示；重新上线的 CPU 也不会自动进入旧候选池。部署工具可监控 topology/online 变化，合并事件后刷新，并展示拓扑代际。候选集合是偏好，不能当作硬 CPU 隔离规则。

5. **把性能结论分开验证。** 复算 12 轮原始汇总后，提交中的 QPS 中位数和 +32.1%/-0.2% 算术一致，GET miss/error 均为 0。单物理 NUMA 宿主上的双虚拟 NUMA 结果不能证明物理远端访存收益；固定 `-c 5` 也没有验证动态拓扑选核优势。建议增加真实多 NUMA 机器、只返回 DEFAULT 的最小 BPF 程序作为 hook 开销对照，以及多流、多 RX queue、候选 CPU 饱和的场景。与静态 RPS 的小差异还需更多样本。

性能复核结果见 [benchmark-check.json](benchmark-check.json)，原始报告见 [Redis NUMA 性能报告](../test-results/redis-numa/report.md)。本轮没有重新跑 Redis 性能测试。

## 误报排除和范围限制

排除了“回调没有自己的 RCU 锁就会 UAF”、“更新后立即释放旧 trampoline”、“通知器持有 RTNL 再 detach 导致锁序反转”和“无 dev 引用必然悬空”的疑点，依据是完整的调用者锁范围、发布/等待/释放顺序及本轮生命周期验证。没有发现需要修复的对应路径。

未验证 PREEMPT_RT、其他架构、真实物理 SMT、硬件 aRFS、多 RX queue 压力、分配失败注入和全量 BPF selftests。构建关闭 BPF_RFS 的结果引用此前记录，本轮只核对源码中的编译分支和依赖。当前结论是未发现可确认回归，不是对这些场景作无缺陷保证。

详细审查轨迹见 [audit.txt](audit.txt) 和 [change-categories.txt](change-categories.txt)。

```
REACHABILITY: confirmed
BUG FIX DETERMINATION: not a bug fix
Kconfig changes verified
FINAL REGRESSIONS FOUND: 0
FINAL TOKENS USED: unavailable (no session token counter exposed)
Assisted-by: Codex:GPT-6
```
