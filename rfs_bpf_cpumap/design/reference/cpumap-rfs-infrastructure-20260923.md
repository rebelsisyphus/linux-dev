# 在 CPUMAP 上实现 RFS：内核与 BPF 基础设施拆分

日期：2026-09-23。状态：设计建议，待用户审阅；没有启动实现。
承接 programmable-rx-community-research-20260923.md 和
cpumap-ordering-napi-research-20260923.md。

## 范围和结论

目标是应用感知的可编程选核，以及正常换核不额外引入流内乱序。
CPU 下线遵循原生 RFS 的例外边界；不是端到端零丢包、零乱序保证。
CPUMAP 的排队、GRO 位置和工作线程与 backlog 不同，不能宣称所有内部
行为或每一个丢包时刻都与原生 RFS 相同。

不改内核即可用 socket 活动观察程序、普通 BPF tuple→CPU map 和现有
CPUMAP 做应用感知选核原型；它不能因此被称为完整的 RFS 保序实现。

推荐的分界是：BPF 定义流、记录应用 CPU 提示、选择期望 CPU；内核
提供可选的受控投递、真实完成状态、迁移交接和对象生命周期。
XDP 访问 socket storage 是采用 sk_storage 路线时的接口缺口，不是所有
实现都必须新增的设施。内核不需要内置 RFS 选核算法或新的全局五元组表。

## 已有基础与增量

| 能力 | 内核侧 | BPF / 用户态侧 |
| --- | --- | --- |
| 源端执行与跨 CPU 投递 | 复用 XDP、redirect、CPUMAP bulk/ring/kthread、GRO | 解析包并请求重定向；控制程序预建目标 entry |
| socket 查找 | 复用 XDP 的 TCP/UDP lookup 和引用释放 | 根据包和网络命名空间上下文查找；处理查找失败 |
| 应用 CPU 提示 | 先复用 socket tracepoint/tracing；覆盖不足才讨论最小观察点 | 独立 observer 记录 socket、CPU、时间/版本，维护 sk_storage 或普通 map |
| XDP 读取 sk_storage | 当前 helper 分派未开放现有 sk_storage_get；需安全访问能力 | RX 路径只读已有提示，创建尽量放 observer/控制侧 |
| 稳定的保序域 | 提供受保护状态对象、代际与引用；不必解析五元组 | 定义哪些包共享同一域，以及域的查找和生命周期请求 |
| 投递与完成记账 | 必须覆盖预约、真正投递、失败、GRO/协议栈交付完成 | 读取统计/可选完成信息；不伪造内核实际完成 |
| 正常迁移 | 原子地检查旧域在途状态、确定实际目标、登记本包 | 提供 desired_cpu；未能迁移时接受继续使用旧 CPU |
| entry 更新、卸载、CPU 下线 | 生命周期和排空/失效协议，目标有效性最终复核 | 控制程序按协议更新/卸载；BPF 做候选过滤和可选抑制抖动 |
| 拓扑/负载策略 | 先复用通用观察设施，不依赖调度器私有策略 | 用户态提供 LLC/NUMA/允许集合；BPF 自定义策略 |
| RX 元数据 | 按支持范围补充传递和 skb 恢复，校验可信性 | 读取驱动 RX hints、保存策略所需信息 |

CPUMAP entries 当前由用户态创建、更新、删除，不能把这些操作当成
BPF 数据面已有的 map_update 能力。BPF 选择预先配置好的目的 entry。
native XDP 仍要求驱动支持 XDP redirect；generic 路径可用于不具备该
支持的设备，但不能假设它具有相同的性能或源端处理成本。

## 应用提示和 socket 的边界

应用 CPU 是消费 socket 的执行上下文提供的提示，不能将 sk_incoming_cpu
直接视为应用 CPU。普通 socket send/recv tracepoint 可以支持初始原型；
当前项目只观察成功 recv 的样例不等价于原生所有学习点。若要求覆盖原生
行为，应列出 send/recv/splice 等调用路径，逐个确认可观察位置与采样语义。
不能将 CONFIG_RPS 下的 sock_rps_record_flow() 当成独立于 CONFIG_RPS
的现成观察接口，也不需要为此重命名 CONFIG_RPS。

现有 XDP helper 支持 socket lookup；这并不自动允许 sk_storage 访问。
采用 storage 路线时，需检查 socket 指针类型、引用、RCU、fullsock、
storage 值的有效期和 verifier 规则。可以评估扩展既有 helper 的可用范围，
或一个范围明确的通用 kfunc；不应先把签名固定为“取得 RFS CPU”。

读路径可只查已有 storage，避免每个 RX 包尝试分配。observer 创建失败、
提示过期、多个线程共享 socket、用户线程迁移，都影响策略新鲜度；它们
不应破坏实际投递域的状态。CPU 提示可以覆盖或过期，保序状态不能随之重置。

XDP 查找发生在后续路由/NAT/完整 socket demux 之前。初版应明确支持
直达本地的简单 TCP 等范围；UDP reuseport、分片、隧道、VRF/DNAT 需单独
验证。不应把任意 XDP lookup 结果都当成最终消费该包的 socket。

Eric 提议可以减少独立应用提示表的查找与碰撞，但“提示表消除”不等于
“所有在途状态消除”。将保序对象与 socket/storage 关联是可探索的优化，
还必须解决无 socket 首包、listener→established、socket 销毁和未完成帧。
当前不能承诺仅用 sk_storage 就完全不需要其他保序域或交接状态。

## 最关键的内核增量：可信投递与迁移

以下描述是接口语义需求，不是已经存在或已经确定的 kfunc ABI。

1. 提交请求携带稳定的域、候选 CPU 和 CPUMAP 实例。内核把“检查能否
   换核、选择实际 entry、登记本包仍在途”组成一个并发安全的操作。
   不能由 BPF 无锁读完完成数后，单独改 CPU 再调用普通 redirect。
2. 请求登记到实际 redirect 之间不能存在未计入的包。需要预约/提交/取消
   或等效协议，与驱动返回后的 redirect 路径接合。不得提前把 frame 交给
   远端 CPU，绕过已有 bulk 对驱动 page-refcnt/recycle 所需的保护。
3. 一个被接管的包最终必须有且仅有一个终结结果：完成受控接收阶段，
   或已确定不会再次交付的丢弃/取消。ring 满、frame 转换失败、skb 分配
   失败、远端程序 DROP 等都要记账。普通 redirect helper 返回成功不代表
   后续 bulk 入 ring 一定成功；其 fallback flags 也不是异步 ring 满回调。
4. 完成边界要覆盖源 bulk、目标 ring、worker 本地批次、GRO 持有的包及
   未提交的 normal list。目标 XDP 执行或 ring dequeue 都太早。当前
   xdp_cpumap_kthread tracepoint 也在 gro_receive_skb() 之前。
5. 可以在适当批次/GRO 交付后保守推进进度，不应逐包强制 GRO flush。
   若采用 ticket/watermark，必须表示连续完成的前缀，不能直接取已完成
   ticket 的最大值；多个源 bulk 的发布顺序与取号顺序可能不同。采用
   outstanding 计数也要证明预约与切换的原子性。具体表示须原型验证。
6. 正常情况下，旧域还有未完成包，就继续投递旧 CPU；旧包到达规定的
   完成边界才采用 desired_cpu。不在 XDP 内睡眠或忙等，不要求应用 recv
   完成，不必为每次迁移额外创建一个无限等待队列。

actual_cpu、entry 代际、未完成请求/完成进度是机制状态。建议由内核
保护，可探索通过 BPF map 持有不透明对象，而不是新建内核全局 tuple 表。
对象具体如何嵌入 map、持有引用、被 verifier 验证仍需设计，不能把这种
表示当成现有能力。BPF 保存域的索引、配置和策略状态。

也可以研究“BPF 协作状态机 + 内核可信记账接口”，但它要求所有相关
程序遵守协议；仅增加 enqueue/dequeue tracepoint 无法自动给出这些保证。
上述推荐将最小检查与提交留在内核，是为避免把并发正确性分散给每个用户。

保序仅适用于始终使用同一域和受控接口的流量。任意 BPF 可以改域或绕过
接口直接 PASS/普通 redirect 时，内核无法仅凭该 API 保证全流顺序。
若要连任意用户策略也无法绕过，需另行限制策略接口/执行入口；本设计
不能冒充已经解决此前 G1–G5 的更强隔离与慢程序容错问题。

## 必须一起考虑的状态转换

### 首包、查找失败、Map 满

首包找不到 socket 就 XDP_PASS，后续找到 socket 再改走 CPUMAP，会产生
源端旧包与远端新包并行的风险。一个支持接管的流应从第一包就进入一致的
投递域：没有提示时用 hash 策略，得到提示后只改变 desired_cpu。

保序域不能随着 NULL→socket、提示过期或策略替换而变化。对不支持的
流量可以选择持续走原路径，但不能因某次查找成功就随意中途接管。
初次挂载也存在原路径的在途包，严格切换需要入口协调或受控停流/排空。

普通 LRU map 不能随意淘汰活跃保序状态。可评估预分配固定域/保守哈希
桶，碰撞时共享约束、可能推迟迁移；这比状态丢失后重建更容易给出边界，
代价是不能宣称精确无碰撞。精确流对象则必须设计容量耗尽和安全准入。
不能简单把“新流 map 满则 PASS，稍后有空间再接管”称为保序 fallback。

### entry、程序与 CPU 生命周期

当前 CPUMAP 更新同一个 CPU 的 entry 也会创建新对象并替换指针，旧对象
经过 RCU 后排空释放。它保证内存生命周期，不代表新旧 entry 的包有序。
所以目标身份需要包含实例/代际，不能只存 CPU 编号。策略热更新尽量复用
传输实例和保序域；entry 替换、map 删除、最终 detach 都要有交接约束。

优雅退出要先阻止新请求进入将退休的域，再完成旧请求和 GRO 交付，最后
释放状态/恢复其他路径。首版可以明确要求控制侧停流/排空；若要求无停流
detach，需要额外的持久入口协调。普通 bpf_link detach 不自动具备该语义。

CPU online 候选表由控制程序维护，但最终投递不能只信任该表的快照。
内核应校验 entry 有效性并参与 CPU 下线处理，避免对旧对象继续投递或
等待永远不会推进的进度。原生 get_rps_cpu() 对旧 CPU offline 有跳过
旧水位等待的例外；dev_cpu_dead() 尝试用 netif_rx() 重新投递旧 backlog，
不是无条件清空丢弃。新方案应明确迁出/继续处理与失败处置，不承诺离线
窗口绝对保序/无丢包，也不能将主动丢弃全部残包称为与原生行为一致。

首版还应限制远端 XDP 的二次 REDIRECT，或继续跟踪其后续投递；不能将
成功再重定向误认为已经进入协议栈。先在 CONFIG_RPS=n 验证主路径；若
启用原生 RPS 等后续异步转移，需说明完成边界和策略叠加的影响。

## BPF 策略与性能边界

BPF 分成独立 observer、入口分类/域查找、selector、统计几部分。
RPS selector 使用稳定 hash，RFS selector 使用 socket CPU hint，LLC 和
负载感知 selector 后续按需加入。拓扑与配置可由用户态注入，CPU 活动可
通过适当的 BPF 观察程序采样；无需把网络机制直接绑定调度器选核算法。

可选保序应显式启用，普通 CPUMAP 使用者不应承担完整流状态/每包原子
同步开销。启用后尽量保留 bulk、GRO、批次完成发布，避免每包完成 BPF
回调、跨核全局锁、逐包动态分配或全 CPU 扫描。策略可以按需限频/加迁移
迟滞，但不能把限频当成保序证明，也不能未经测试承诺性能不下降。

RX hash、checksum、VLAN、queue metadata 的传递/恢复应与保序分开评估。
当前可读取一部分 driver hints，不代表重定向构造 skb 时自动恢复标准字段。
hash/checksum 恢复主要关联性能；被硬件剥离的 VLAN 等也可能影响正确性，
必须按支持的流量范围处理。修改报文后不能盲目信任原 checksum 状态。

硬件 aRFS/ntuple 不列为软件 RFS 的前置条件。未来可共用 socket hint、
流标识、CPU/队列拓扑和策略输出；硬件规则安装确认、老 RX ring 排空与
软硬路径交接仍需另一个协议，不能假设软件完成水位自动涵盖硬件在途包。

## 建议验证顺序与当前状态

先固定简单本地 TCP 和稳定入口的实验范围，验证 observer/lookup/hint
访问及现有 CPUMAP 的成本；无内核补丁的普通 map 原型作为功能/性能基线。
接着原型化 opt-in 投递、完成、迁移机制，同时覆盖首包和容量失败协议；
生命周期语义不能等成品后再补。再扩大 UDP/IPv6、generic/multi-buffer
和 metadata 范围。仍保留 v3 backlog 实现作为对照，不预先宣告路线胜负。

必要用例：旧 CPU 堆积时换核、GRO 持包、并发源提交、bulk/ring 满、
frame/skb 失败、提示首次出现/过期、socket 销毁、状态耗尽、同 CPU entry
替换、策略更新、卸载、CPU offline。性能比较无 XDP、现有 CPUMAP、
CPUMAP+lookup、CPUMAP+可选保序与原生 RFS，记录 pps、CPU cycles、尾延迟、
迁移延迟及乱序/丢包，而非只比较吞吐量。

本轮只分析和记录。未修改源码、现有补丁、TODO 或构建配置，未运行测试。
既有14补丁 v3 仍为最新验证系列；G1–G5 继续暂不实施。

## 核对依据

本地工作区 /home/sisyphus/code/worktree/rfs_bpf，固定基线
45c13f3f9e3bb15fd89ff2864c6f627a3b4b4229。索引对应 HEAD；项目新增行为
直接读未提交工作区核对，未当作上游能力。关键位置：

- net/core/filter.c：xdp_func_proto()、bpf_sk_base_func_proto()。
- net/core/bpf_sk_storage.c：bpf_sk_storage_get()、helper proto。
- net/socket.c：sock_recvmsg_nosec()、sock send/recv tracepoint。
- include/net/rps.h、net/ipv4/af_inet.c：原生学习点和 CONFIG_RPS 边界。
- kernel/bpf/cpumap.c：cpu_map_kthread_run()、bq_enqueue()、
  bq_flush_to_queue()、cpu_map_generic_redirect()、entry 更新和释放。
- net/core/dev.c：get_rps_cpu()、process_backlog()、dev_cpu_dead()。
- Documentation/bpf/kfuncs.rst：上下文、指针可信性和接口生命周期要求。

在线复核：

- [CPUMAP 官方文档](https://docs.kernel.org/bpf/map_cpumap.html)。
- [RFS 官方说明](https://docs.kernel.org/networking/scaling.html)。
- [上游 XDP helper 分派](https://github.com/torvalds/linux/blob/master/net/core/filter.c)。
- [上游 CPUMAP](https://github.com/torvalds/linux/blob/master/kernel/bpf/cpumap.c)。
- [RX metadata 官方文档](https://docs.kernel.org/networking/xdp-rx-metadata.html)。

在线 master 会变化。新增接口、机制归属和实现步骤均为本项目设计建议，
不是上游已合入的承诺，也不是 maintainers 已认可的方案。
