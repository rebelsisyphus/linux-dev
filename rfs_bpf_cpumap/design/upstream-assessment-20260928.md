# CPUMAP / BPF RFS 上游投稿评估

日期：2026-09-28。项目分支：develop/rfs_bpf_cpumap。
评估版本：9de54e62c380e308927758207e9507fcb107e479，共七个补丁。
范围：技术价值、维护边界、投稿组织和证据缺口；不替代逐行正确性审查。
本次没有重排补丁、修改实现、重新运行测试或对外发送。

## 判断

有 RFC 讨论价值。建议内核邮件系列包含通用机制、对应 selftests 和完整
接口说明；完整 RFS 策略、加载器及部署文档可以在公开独立仓库维护。
不建议机械地删去补丁 5–7：应保留补丁 6 的关键组合回归，并将其依赖的
最小 BPF 消费者移到 selftests 中。完整生产策略不需要因此进入内核树。

前四个补丁已经包括独立接口测试，因此“前四个 + 公开 PoC”可以作为初始
RFC 形式，不是违反社区规范。建议补充最小 socket 提示 → CPUMAP 迁移测试，
是为了增强评审和长期回归价值，并非某条规范强制要求名为 RFS 的测试。

当前适合讨论机制和接口，不宜宣称已经证明性能优势或具备替代原生 RFS
全部功能的条件。社区未审阅本系列；下述接受阻力均为基于资料和源码的判断。

## 社区依据

1. 2018-10-10 Eric Dumazet 主张可扩展的 BPF RPS/RFS 基础设施，把用户策略
   留给 BPF，并提出使用 socket lookup 减少额外流提示表。John Fastabend
   指向 CPUMAP 与 socket lookup，并说明还需额外工作。该讨论支持组合现有
   机制的方向，不构成对本系列 domain/kfunc 设计的认可。
   https://lists.openwall.net/netdev/2018/10/10/161

2. XDP 项目历史计划专门提出动态选核的 OoO-safe 问题，并讨论是否需要暴露
   in-flight 状态。页面仍含已经过时的 generic/GRO TODO，仅作为历史需求证据。
   https://xdp-project.net/areas/cpumap.html

3. 2024-08-13 Jesper 说明 CPUMAP 的动机包括避免 RPS/RFS 在 RX CPU 上的
   状态维护及流解析瓶颈。因此选择性启用是必要论据，但仍需测量普通路径
   和 ordered 模式的真实成本，不能凭 opt-in 宣称零回退。
   https://lists.openwall.net/linux-kernel/2024/08/13/567

4. 2023-08-24 已合并提交 91dda69b08de
   ("samples/bpf: Remove the xdp_redirect* utilities") 将包括 xdp_redirect_cpu
   在内的工具迁至 xdp-tools / xdp-bench。当前 samples/bpf/README.rst 也有
   此归属说明。完整 XDP 工具外置有直接先例，但不是禁止所有 BPF 样例入树。
   https://github.com/torvalds/linux/commit/91dda69b08de93465632561701a337ec18321ea4

5. BPF 投稿文档偏好 selftests 而非 samples，因为 CI 会执行前者；
   要求用例说明、测试和多补丁 cover letter。新特性通常面向 bpf-next。
   https://docs.kernel.org/bpf/bpf_devel_QA.html

6. netdev 文档要求同时展示新接口的用户；对不在 netdev 维护的外部用户态
   项目允许提供公开仓库链接；相关 selftests 应与内核变化一起提交。
   这些规定支持“外置工具 + 内核自测试”，不支持仅用 URL 替代补丁说明。
   https://docs.kernel.org/process/maintainer-netdev.html#co-posting-changes-to-user-space-components
   https://docs.kernel.org/process/maintainer-netdev.html#co-posting-selftests

7. kfunc 生命周期文档明确考虑树外 BPF 调用者。kfunc 不是稳定用户 ABI，
   但仍需说明真实用户和维护成本。使用 kfunc 不等于任何接口设计都容易合入；
   本系列 map_extra 的创建语义还属于用户可见配置。
   https://docs.kernel.org/bpf/kfuncs.html#kfunc-lifecycle-expectations

8. 提交规范要求说明可观察的问题、量化性能主张及代价，补丁说明应自包含。
   https://docs.kernel.org/process/submitting-patches.html

9. 2025-02-25 已合并提交 4f8ab26a034f
   ("bpf: cpumap: switch to GRO from netif_receive_skb_list()") 给 CPUMAP 加入
   GRO，提交说明报告了测试收益。当前 ordered 模式每 64 包/队列空时进行完整
   GRO flush，因此 GRO 聚合率和协议栈吞吐是实际演进背景下的重点。
   https://github.com/torvalds/linux/commit/4f8ab26a034f04f918c8d36c051ec84e4082eed5

## 核对的上游快照

通过 GitHub API 获取不可变 SHA 后读取对应 cpumap.c 与 filter.c：
- torvalds/linux master：72d3fcf802c45d00b300f25b848a93c3a2bd7c7e，
  提交时间 2026-09-27T20:55:01Z。
- kernel-patches/bpf bpf-next_base（BPF CI 镜像）：
  0309bfd7d7780327db8f54a483c27b1ae66c1606，
  提交时间 2026-09-26T12:46:42Z。

两处 XDP helper 分发表都未开放 BPF_FUNC_sk_storage_get，CPUMAP 文件未出现
本系列的 bpf_cpumap_redirect / bpf_cpu_map_domain。完整结果及源码哈希见
upstream-snapshot-20260928.json。这是对具体版本的核对，不是对所有未合并邮件
或其他等价设计的穷尽排查；正式发送前仍需对目标 bpf-next 重新检查并重基。

## 当前实现的上游价值

- 补丁 1 让 XDP 已有的 socket lookup 与已有 sk_storage 组合，能力通用，
  改动较小，且与 ordered CPUMAP 在技术上可以独立评审。
- 补丁 2 的主要价值是可信提交和完成边界：实际入队成功、源 bulk 预留、
  GRO/正常接收链完成、并发切换和 entry 代际，无法靠观察一次远端 BPF
  执行或读取 ring 空状态就安全推导。
- domain 由 BPF 分配，内核只维护保序状态，可服务不同动态 CPU 策略；
  不要用尚未实现的 aRFS/ntuple 作为当前合入理由。
- 不引入新主收包 hook/program type，不拆改 CONFIG_RPS，不把具体 CPU
  策略加入 net/core/dev.c，减少了引入另一套收包框架的评审面。

但“没有五元组哈希表”不能写成“没有流状态或缓存开销”。domain 仍保存
迁移状态，socket lookup 和 sk_storage 也有访问成本；4096 个域可能碰撞，
影响迁移及时性及局部性。RFS 样例复刻应用同核目标本身尚不足以证明优于
原生 RFS，必须有真实 workload 或新增策略能力支撑。

## 主要评审问题与未完成证据

1. 热路径和 GRO 成本。
   cpu_map_reserve / cpu_map_commit 都访问域锁和共享状态；
   cpu_map_kthread_run 在 ordered 分支发布完成前执行完整 GRO flush。
   需要区分未启用扩展的回退、启用后的成本和动态迁移收益。

2. 内核机制的最小性。
   需解释为什么 snapshot 或 tracepoint 不能原子串起准入与迁移，
   比较“只暴露完成原语、状态在 BPF”和当前内核域状态方案；
   不能仅以已经实现为理由断定当前 API 是唯一形式。

3. 接口选择和可组合性。
   map_extra 作为域数量是否合适、域容量和碰撞如何影响成本；
   get_state 是否暴露过多内部状态；远端 REDIRECT 被拒绝的代价；
   热插拔和 entry 重建的例外是否清晰、能否长期维护。
   kfunc 的非稳定性不消除 map 创建语义的长期维护责任。

4. 保证范围。
   保证的是同一 map/domain 的迁移交接，终点不包括任意下游异步路径；
   多队列输入已乱序、分片/不支持格式 PASS、程序直接卸载、CPU offline
   均有明确边界。发送时应描述为可选迁移保序，不称全路径/无损保序。

5. 真实使用者与维护。
   需要展示至少一个具体场景确实需要在线换核，固定哈希、仅新流采用
   新映射或已有原生 RFS 不足以满足目标。无需虚构第二个生产用户。
   外部仓库应公开可编译源码、明确对应版本、测试命令和维护联系人。

现有证据：CONFIG_RPS=n、KASAN/lockdep QEMU 下 22 子测试通过；
基础设施已有若干配置对象编译检查。未有物理 NIC 性能/真实高负载 TCP GRO
结果，RT 编译不等于 RT 运行，不能用本轮只读分析扩张原验证范围。

建议的最小性能对照：
- 未打补丁普通 CPUMAP vs 打补丁但普通模式：检查现有用户回退。
- ordered 固定目标 vs 普通 CPUMAP：分离机制本身成本。
- ordered 动态目标 vs 普通 CPUMAP 动态目标：同时报乱序和性能。
- BPF RFS vs 原生 RFS、合理配置的 RSS：报告应用吞吐和延迟。

配置应固定并披露包长、流数、队列数、源/目标 CPU、GRO、NUMA；
指标至少包括 PPS/吞吐、CPU 开销、P99 延迟、丢包/乱序、GRO 聚合和迁移等待。
低流数到高碰撞负载都应覆盖，避免只报 UDP_DROP 极限数据。

## 七个补丁的建议去向

| 原补丁 | 建议 |
| --- | --- |
| 1 XDP sk_storage | 内核；可以独立小系列先讨论，带自测试和具体使用者 |
| 2 ordered CPUMAP | 内核核心 RFC；重点征求机制、性能和 API 反馈 |
| 3 ordered 自测试 | 保留在 tools/testing/selftests/bpf |
| 4 CPUMAP 文档 | 保留，吸收必要的组合使用约束 |
| 5 完整 RFS 样例/loader | 独立公开仓库；将自测试需要的最小 BPF 逻辑留在内核测试树 |
| 6 RFS 测试 | 保留关键组合覆盖，移除对 samples/外部仓库的编译依赖 |
| 7 RFS 文档 | 接口及保序约束留内核；安装、CLI、运维和完整策略说明外置 |

原补丁 6 的 progs/test_xdp_rfs.c 第二行直接包含
../../../../../samples/bpf/xdp_rfs.bpf.c，Makefile 也追踪 samples 依赖。
不能只丢弃补丁 5 而原样保留补丁 6；应重组测试 BPF 文件及编译依赖，
确保测试不需下载 GitHub 代码。树内回归验证接口语义，外部仓库验证完整策略，
两边可各自维护其测试，不必追求长期源码字节完全一致。

## 建议发送形态

初次使用 [RFC PATCH bpf-next 0/N]，主题聚焦
“bpf: cpumap: optional ordering domains for CPU migration”。
主送 bpf@vger.kernel.org，抄送 netdev 和目标树 get_maintainer.pl 给出的
BPF/XDP/CPUMAP 相关维护者；额外考虑当前 GRO 代码作者和评审者。

cover letter 自包含：具体问题、普通 CPUMAP 迁核乱序示例、现有设施为何不足、
机制/策略边界、保证与例外、测试、已测成本和未测项、公开 PoC 的固定版本链接。
不要以新一套 sched_ext 式 RX 框架或全面替代 RPS/RFS/aRFS 作为第一轮主张。

公开链接宜为固定 tag/commit，提供基础内核 SHA、构建和运行步骤、
应用 CPU 迁移复现、原始性能数据以及与 RFC 版本对应关系。链接是补充材料，
邮件仍须能独立完成机制和 API 评审。

可先独立提交 XDP storage 的小系列，避免它被 ordered 模式的设计争议绑定；
ordered 系列再引用其版本或合入提交。是否合并成一轮 RFC 取决于展示整体
用例的需要，不应为了坚持四个或七个补丁而牺牲可读性和可构建性。

