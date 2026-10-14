# BPF 可编程收包选核：社区调研与路线评估

调研日期：2026-09-23。项目：rfs_bpf。本文是待审阅的研究结论，不表示已变更开发路线。

**结论：用户提出的“通用设施 + 用户自行编写策略 + 按需组合能力”成立，而且已有直接先例。**
XDP/CPUMAP 已经提供其中相当一部分设施。值得继续研究的是 socket/应用感知、配置与状态生命周期、动态迁移的可验证语义，以及低成本地组合这些能力。仅以“BPF 可选 CPU”或“可编程收包路径”为理由新增整套 rx_ext，论据不足。

此前调研不够完整，尤其没有充分展开 Jesper 2017 年对 CPUMAP 保序扩展的提案、Suricata 策略进入内核样例的历史、2023 年样例迁往 xdp-tools 的事实，以及实际项目如何维护 CPU 选择策略。此前因保序成本而优先推荐独立 backlog 框架，作为上游路线判断下得过早。现有 backlog 实现可保留为功能基线和对照，但不应预先确定最终上游形态。

**证据范围与限制。**

- 查阅 netdev/BPF/XDP 邮件原文及其归档、XDP 项目计划和演讲、当前 Linux 文档和代码、项目自身文档、公开论文和作者代码仓库。
- 重点检索 CPUMAP + socket lookup、BPF RPS/RFS、CPU steering、dynamic load balancing、ordering，以及 2025/2026 年相关讨论。
- 本地源码使用注册工作区 `/home/sisyphus/code/worktree/rfs_bpf`。固定基线为 `45c13f3f9e3bb15fd89ff2864c6f627a3b4b4229`；HEAD `4626ecd482a8` 不是最新实现，另行读取工作区中的 v3 文件。运行 `develop rfs_bpf index` 确认索引对应 HEAD，语义分析以文件现状为准。
- 上游现状另核对 torvalds/linux 的公开 master 文件和官方文档。在线分支链接可变化，不能把抓取日期等同于某个发行版的引入时间。
- 一些邮件归档和 GitHub 单文件页面直接读取失败，邮件通过搜索索引中的完整相关段落交叉核对；当前 xdp-bench 的策略清单以维护者 README 为证据，并用本地 Git 中已合并的历史实现/提交验证其来源。未声称审计所有外部项目的全部源码。
- 没有运行外部项目或进行实网性能复现；外部项目的吞吐、延迟数字不作为本项目预期收益。
- 本轮未找到已上游、可直接取代原生 RFS 且覆盖全部动态迁移语义的通用 BPF 产品；这是检索范围内的结果，不是“全世界没有其他实现”的证明。

**1. 社区说过什么，以及每条证据能证明什么。**

2017 年 NetConf，Jesper 已把 CPUMAP 的动态流迁移列为问题，并在第 29–30 页提出通过多个协作 BPF 程序观察正常收包、入队丢包和出队，维护流级在途计数。这是“扩展底层设施，使 BPF 能自行实现策略和可选保序”的直接先例；它是提案，不是已合并接口，也不是针对当前 GRO 路径的完整正确性证明。[原始幻灯片，第 29–30 页](https://people.netfilter.org/hawk/presentations/NetConf2017_Seoul/XDP_devel_update_NetConf2017_Seoul.pdf)

2018-10-10，在 VLAN RPS 扩展讨论中，Eric Dumazet 建议可扩展的 BPF 基础设施和 socket lookup，以避免额外的流信息哈希表；John Fastabend 进一步指向 socket lookup helper 和 CPUMAP，并明确表示还需要额外工作。这支持优先考察现有设施组合，不能解释成对我们现在这组 struct_ops/hook 的认可，也没有宣告完整 RFS 已经可用。[John 回复，包含 Eric 原文](https://lists.openwall.net/netdev/2018/10/10/161)

2019 年 Matt Cover 的 XPS BPF hook RFC 讨论进一步提及 RPS hook、用 BPF maps 实现仅作用于部分流量的 RFS，以及未来硬件 RSS 扩展。本轮证据仅能确认有人提出过这些设想，不能把 RFC 当成已采用方案，更不能推断没有合并的具体原因。[原始讨论](https://lists.openwall.net/netdev/2019/09/20/21)

XDP 项目计划专门列出动态负载均衡的 OoO-safe 目标，并提出研究是否需要向 BPF 暴露在途状态。该页面同时含有过时的 GRO/generic-XDP TODO，因此只用它证明研究方向的历史存在，不用它判断当前功能缺失。[XDP CPUMAP 计划](https://xdp-project.net/areas/cpumap.html)

2024-08-13，Jesper 解释 CPUMAP 的动机包括 RX CPU 上的 RPS/RFS 状态维护和流解析瓶颈。这个性能约束与其早年“按需提供保序扩展”的提案可以同时成立：它们不支持给所有使用者强制增加一套流状态，也不支持断言任何可选扩展都不值得做。[原始邮件](https://lists.openwall.net/linux-kernel/2024/08/13/567)

2024-11 的 GRO 测试讨论报告了实际容器场景：把某容器的收包导向其绑定 CPU 集合，与应用共享 L3，限制不同容器之间的干扰。该邮件证明 CPU/LLC 隔离需求和 CPUMAP 用途确实存在；它没有证明 socket 跟踪、调度记账的所有语义或严格迁移保序已经解决。[使用者描述](https://lists.openwall.net/netdev/2024/11/25/161)

**2. 内核源码中有没有这种 BPF 策略程序。**

有明确的历史实现，而且相应工具仍在社区维护：

- `fad3917e361b` 引入 `samples/bpf/xdp_redirect_cpu`。
- `1bca4e6b1863c0a006fde6a66720a87823109294`，2018-08-10，主题为 `samples/bpf: xdp_redirect_cpu load balance like Suricata`，实现 IP 地址对与 L4 协议的对称哈希，并把它设为样例默认策略。这里不能把“l4-hash”理解为必然包含端口的完整五元组。
- `91dda69b08de93465632561701a337ec18321ea4`，2023-08-24，将这些工具迁至 xdp-tools 的 `xdp-bench`，不是删除 CPUMAP 能力。[迁移提交](https://git.zx2c4.com/wireguard-linux/commit/?id=91dda69b08de93465632561701a337ec18321ea4)
- 当前 `xdp-bench redirect-cpu` 提供固定 CPU、逐包轮询、按 L4 协议、地址哈希、源端口和目的端口等选择方式。逐包轮询并不承诺同流保序。这些是可运行的 BPF 策略样例/基准，并不是自动启用的通用 RFS 服务。[当前工具说明](https://github.com/xdp-project/xdp-tools/tree/main/xdp-bench)
- 当前 Linux 仍有 `tools/testing/selftests/bpf/progs/test_xdp_with_cpumap_helpers.c` 和 `prog_tests/xdp_cpumap_attach.c`，检验重定向、远端程序、类型和挂载约束等。不能把这些测试说成自适应生产选核器。[内核样例归属说明](https://github.com/torvalds/linux/blob/master/samples/bpf/README.rst)

相邻接口还包括 TUN 的 `TUNSETSTEERINGEBPF`、reuseport BPF、`BPF_PROG_TYPE_SK_LOOKUP`、BPF flow dissector。这些分别选择队列、socket 或解析流键；并不都负责把协议栈执行迁到另一个 CPU。特别是 `BPF_PROG_TYPE_SK_LOOKUP` 与 XDP 中调用 `bpf_sk_lookup_tcp/udp()` 是不同概念。[SK_LOOKUP 官方说明](https://docs.kernel.org/bpf/prog_sk_lookup.html)

**3. Linux 邮件列表以外的实际探索。**

| 项目/社区 | 可确认的内容与成熟度 | 对本项目的意义和边界 |
| --- | --- | --- |
| Suricata/OISF | 官方提供 XDP CPU redirect，结合 CPU affinity 和 AF_PACKET 的 cluster_cpu；有实际实现 | 证明应用可自行维护流量分配策略。官方配置针对其场景建议单 RSS 队列避免乱序；不是任意多队列和在线换核保序的证明。[官方文档](https://docs.suricata.io/en/latest/capture-hardware/ebpf-xdp.html) |
| xdp-cpumap-tc | 用 CPUMAP 把客户/服务流量送到相应 CPU，TC 再选择对应 TX queue/HTB 类；公开实现 | 展示接收选核能服务于后续网络处理和隔离，而不局限于 socket 的最后执行 CPU。[项目](https://github.com/xdp-project/xdp-cpumap-tc) |
| LibreQoS/Bufferbloat | 实際使用 BPF 分类、CPUMAP 与多核整形；维护者在 2026-02-27 记录流缓存、元数据传递和代际更新的改造 | 热更新、重复查表与同步成本是工程重点；该报告没有给出等价原生 RFS 的迁移保序证明。[维护者记录](https://devblog.libreqos.com/posts/0013-no-more-locks/) |
| QEMU/KVM | eBPF RSS 根据 Toeplitz 哈希和可配置间接表选择 virtqueue；策略程序位于 QEMU 的 tools/ebpf/rss.bpf.c | 非内核项目维护策略、内核提供执行接口的直接范例。目标是 guest 队列，不是直接选择 host 协议栈 CPU。[官方文档](https://www.qemu.org/docs/master/devel/ebpf_rss.html) |
| RSS++/FastClick/DPDK | CoNEXT 2019 论文和开源实现：依据负载调整 RSS bucket 分配，处理流状态迁移；论文同时研究 Linux 用户态控制器 | 对负载估计、迁移成本和硬件分流有直接参考价值。它不是 BPF RFS，DPDK 路径的迁移结论不能直接套给 Linux CPUMAP。[论文](https://dejankostic.com/documents/publications/rsspp-conext19.pdf)、[代码与实验](https://github.com/rsspp/experiments) |
| DPDK Distributor | 公共库按 flow tag 分配到 worker，并定义 worker 完成处理的交接语义 | 说明选核和完成协议需要配套。文档明确指出 worker 隐藏批处理会破坏原先的保序假设；不能只观察出队。[官方文档](https://fast.dpdk.org/doc/guides/prog_guide/packet_distrib_lib.html) |
| Partition-Aware Packet Steering，ENCP 2019 | 论文提出按应用数据分区/L7 key 选择处理核心，结合 XDP/eBPF、可编程 NIC、AF_XDP | 证明策略可以表达应用语义，超出传统五元组哈希。按提案对待，不能当作原生 TCP/RFS 的成熟替代品。[作者论文](https://penberg.org/papers/xdp-steering-encp19.pdf) |
| eTran，NSDI 2025 | 公开论文与研究实现，扩展 eBPF hook/maps，并实现 TCP/Homa 数据路径 | 是“通用机制 + 可编程网络处理”的邻近研究。需要修改内核及用户库，改变传输实现，不能视为普通 Linux socket 的 RFS 已解决。[论文](https://www.usenix.org/conference/nsdi25/presentation/chen-zhongjie)、[实现](https://github.com/eTran-NSDI25/eTran) |

上述项目已经足以否定“没有其他社区探索”的判断，但它们实现的对象不同。尤其不能把远端服务器负载均衡、AF_PACKET socket fanout、AF_XDP 用户态分片、host CPU steering 和网卡队列 steering 都算成同一种 RFS。

**4. CPUMAP + socket lookup 目前能做到什么。**

当前 XDP 可以调用 TCP/UDP socket lookup helper，并用 CPUMAP 把后续处理移到目标 CPU。内核现有 CPUMAP 实现已有远端 XDP 程序、GRO 和 generic-XDP 路径；不能继续引用早期文章宣称它不支持 GRO。[当前 helper 分派](https://github.com/torvalds/linux/blob/master/net/core/filter.c)、[CPUMAP 代码](https://github.com/torvalds/linux/blob/master/kernel/bpf/cpumap.c)、[接口文档](https://docs.kernel.org/bpf/map_cpumap.html)

要实现应用感知的 RFS，还需要区分三个事实：

1. socket lookup 回答“按当前上下文和 tuple 能找到哪个 socket”；它不自动回答应用最近在哪个 CPU 执行。共享 socket 可能有多个 reader，异步 I/O 的执行上下文也需要明确。
2. “期望 CPU”属于策略提示；“该流旧包实际在哪个队列、何时可以切换”属于交接状态。用 socket 本地存储替换提示哈希表，不能自动消除后一类状态。
3. XDP 上下文早于部分协议解析、解封装和 NAT，lookup 结果不能一律当成最终接收 socket。连接初期、监听/reuseport、未连接 UDP、多 netns 和封装流量需要明确支持范围。

项目已有的 `tp_btf/sock_recv_length` observer 是可复用资产，它记录正长度 recv 完成所在 CPU，存入 sk_storage；但这还不是原生 RFS 所有学习路径的完全复现，例如发送/splice/异步路径覆盖需另核对，零长度 UDP 会被现有 ret<=0 条件跳过。

本轮源码核对确认 XDP 的 `xdp_func_proto()` 支持 socket lookup，但未开放 `bpf_sk_storage_get/delete`；继承的 helper 分派也不能让现有 observer/selector 原样搬过去。可研究两种路径：先用限定 namespace/tuple 范围的共享普通 map 做无内核修改原型，或者以真实使用者证明 XDP 读取 socket local storage 的通用接口扩展价值。后者必须审计引用类型、上下文、并发和生命周期，不能把它描述成“加一行白名单即可完成”。[上游 helper 实现](https://github.com/torvalds/linux/blob/master/net/core/filter.c)

Eric 的想法可减少额外的 flow→socket/CPU 提示表和相应碰撞，不等于所有哈希访问都消失，也不证明 socket lookup + storage 一定更快：成本还包括传输层查找、storage 访问、引用处理和后续协议栈再次查找。

**5. 保序扩展有研究价值，但不能只补一个计数器。**

稳定的同流输入与目标映射本身就有价值；无需让所有哈希分流用户承担在线迁移的成本。若提供与原生 RFS 接近的在线交接保证，必须定义完成边界并证明以下环节：

- 包是否成功提交到旧目标；`bpf_redirect_map()` 返回动作不等于后续实际入队成功。
- 旧包是否仍在源 CPU bulk buffer、目标 ring、本地处理批次或 GRO 中。
- 切换决定与并发 producer 是否按同一流域串行化。
- 入队丢弃、远端丢弃、程序更换、map entry 重建及 CPU 下线如何影响状态。
- 保证的终点是什么。原生 RFS 的迁移判断也不等待应用 recv 完成；不应无意升级为任意下游异步处理都不乱序的全局保证。[原生 RFS 说明](https://docs.kernel.org/networking/scaling.html)

当前 `trace_xdp_cpumap_kthread` 位于 GRO/协议栈处理之前，因此计数相等、ring 暂时为空或目标 XDP 已运行，都不足以证明旧流可以切换。2017 年 enqueue/dequeue 提案可以借鉴，不能忽略现今的批处理和 GRO 后原样照搬。[当前调用顺序](https://github.com/torvalds/linux/blob/master/kernel/bpf/cpumap.c)

提示缓存允许过期；未排空的交接状态不能因普通 LRU 淘汰而丢失。保序模式下提示缺失或降级时，也不能无条件跳到 XDP_PASS/另一 CPU，让新包越过旧包。CPU 下线仍按用户要求保留原生 RFS 的例外边界。

迁移很少发生不代表成本只发生在迁移瞬间。采用计数/水位协议通常需要在常规包或批次路径记录提交和完成；减少迁移频率、抽样观测和按需启用分别解决不同开销，不能相互替代。比较时也必须计入 GRO 位置、批量效率以及应用与网络处理的 CPU 竞争。

可以选择由 BPF 公共库维护交接状态，内核提供可信的提交/完成原语；但这依赖策略遵守公共库契约。如果承诺任意用户 BPF 都无法破坏保序和降级语义，最小实际状态及切换授权就需要内核控制或等价的受验证约束。当前 v3 的保序主要是公共 engine 契约，不能冒充后一种强保证。

**6. 建议的模块边界。**

| 层次 | 责任 | 建议归属 |
| --- | --- | --- |
| 观测与配置 | reader CPU 提示、拓扑、允许 CPU 集合、负载摘要、配置 generation | 独立 BPF observer + 用户态管理；按需加载 |
| 策略 | 哈希、应用同核/同 LLC、CPU 隔离、负载滞回；提出期望目标 | 用户 BPF 程序与可复用库 |
| 迁移契约 | 固定映射、仅新流采用新配置、或有完成协议的在线切换 | 按需求选择，明确每种保证；严格模式需要可信底层支撑 |
| 数据投递 | 实际队列、批量投递、内存所有权、目标执行和生命周期 | 优先复用 XDP/CPUMAP；必要时评估 backlog 后端 |

这里的“按需取用”应落实为不使用 socket 策略就不查 socket、不使用负载策略就不做相应监控、不要求在线迁移就不强制维护流级迁移状态。未开启扩展的 CPUMAP 路径是否无可测回退仍需实测，不能仅以 static key 或可选配置推定。

这可以先是 BPF 库、独立 observer、loader 和少量必要接口的组合，不要求先新增 BPF program type 或 struct_ops。若未来同时支持 CPUMAP/backlog，也应先共享策略输入、提示和观测模型，分别保留后端的完成和生命周期语义；不同 ctx/helper 能力不能用一个虚假的统一 ABI 掩盖。

硬件 aRFS/ntuple 后端继续后置。CPU→RX queue 映射、规则容量、异步更新和软件/硬件交接是独立问题，不能把硬件规则写入当成与 CPUMAP 重定向同一条同步指令。

**7. 哪些内容适合推到 Linux 社区。**

可以提交此方向，但投稿材料应围绕真实缺口组织。社区接受过选核 BPF 样例，也已将实用 XDP 工具迁出内核树。Linux BPF 官方规范通常更偏好 selftests，允许适当的简单 samples，并要求说明 use case 和完成测试；这不是全面禁止策略程序进源码树。[BPF 投稿规范](https://docs.kernel.org/bpf/bpf_devel_QA.html)

| 交付内容 | 合适的讨论/维护位置 | 判断 |
| --- | --- | --- |
| 通用 socket 状态访问或可信队列原语 | Linux bpf/netdev，通常按 bpf-next 特性讨论 | 值得以具体使用者和测试提出；需要证明现有 API 不足 |
| 对接口和迁移保证的 BPF 回归程序 | tools/testing/selftests/bpf | 适合与对应内核补丁一起提交 |
| 可运行参考策略、loader、benchmark | xdp-tools、bpf-examples，或独立项目 | 与现有 XDP 工具维护方式一致；具体收录需与维护者讨论 |
| 针对某应用/集群的复杂负载、LLC、租户策略 | 应用项目或独立策略库 | 不必绑定内核发布；不宜要求 Linux 核心维护所有业务策略 |
| 新的通用 RX hook / 整套 rx_ext | 先 RFC，并同时比较 CPUMAP 扩展与现有 backlog | 尚无足够证据认定这是必要的上游形式 |

维护者可能关注的问题是基于现有讨论和实现作出的推断，不是已收到的 review：为何现有 XDP/CPUMAP、RSS、TUN 等路径不能满足目标；新扩展是否增加所有用户的热路径成本；能否支持多个实际策略；生命周期和降级边界是否可测试；由谁持续维护。

程序属于 BPF 并不会自动获得接收侧任意修改权。tracepoint 可用于观测；要改变投递位置和持有包，需要明确的机制、权限和所有权契约。若确需新 hook，应该说明这些语义，而不是仅把现有函数暴露给任意 tracing 程序。

**8. 对 rfs_bpf 的建议下一步，待用户审阅后再实施。**

1. 先把目标改为“为用户策略提供可组合的收包 CPU steering 能力”，并列出明确使用场景与保证等级；不先承诺替换所有 RPS/RFS。
2. 做 CPUMAP 基线，覆盖稳定哈希与应用/容器 CPU 集合策略。先在限定协议和 namespace 范围内验证现有 API 的组合能力，再形成可复现的缺口清单。
3. 将现有 socket observer、L1 滞回策略、不可变配置、统计和测试复用出来。socket storage 的 XDP 可访问性可以单独研究和投稿，不必绑定整套 RX hook。
4. 独立研究在线保序的最小扩展，比较 BPF 状态 + 可信原语与内核最小交接状态；给出反例、证明边界及 opt-in 成本。
5. 在语义相同的情况下比较原生 RFS、现有 BPF backlog、CPUMAP 方案；若 backlog 在特定场景有明确价值，再以该场景论证后端或 hook。
6. 先提交窄 RFC：真实使用场景、可运行 BPF 使用者、必要内核差异、回归测试、性能/乱序结果。避免把所有策略和未来硬件规则塞进第一组补丁。

至少需要以下验证矩阵，不能只报重定向 Mpps：

| 维度 | 必须回答的问题 |
| --- | --- |
| 开销隔离 | 无 BPF、普通 CPUMAP、启用各可选模块分别增加多少 CPU/包和尾延迟 |
| 语义对齐 | 固定映射、应用感知、动态有序迁移分别与同等语义基线比较 |
| 多队列与拓扑 | 单/多 RX queue，同 CPU/同 LLC/跨 LLC/跨 NUMA，应用与收包是否争抢 |
| 业务结果 | TCP 吞吐/重传/乱序、UDP 丢包、应用 P99/P99.9 和总 CPU，而非只看 RX CPU |
| 生命周期 | map 满/提示过期、程序更新/卸载、CPU 下线、设备/netns 注销和队列拥塞 |
| 网络覆盖 | IPv4/IPv6、连接建立、UDP、分片、封装/NAT；不支持的路径明确界定 |

**当前状态没有因此改变。** v3 backlog 框架仍是已做功能验证的实现，实机性能未验收。G1–G5 健壮性改造仍按用户要求仅在 TODO，暂不实施。本轮只做调研和记录，没有改生产源码、补丁、配置或测试运行状态。
