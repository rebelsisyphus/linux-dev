# CPUMAP 路线工作清单

2026-10-12：用户将此前后置的硬件、SCTP/MPTCP 和负载感知扩展纳入本轮，
暂无物理测试机/发包机。实现完成不代表硬件命中或性能已验证。

## 本轮已实现

- 内核机制与完整策略解耦：内核保留 XDP socket storage、可选 ordered CPUMAP、
  独立 selftests 和文档；完整策略在 `/home/sisyphus/code/rfs-bpf`。
  不依赖 CONFIG_RPS，不访问调度器私有状态。
- 稳定 tuple hash、socket RFS、lookup/storage 测量模式、LLC 负载避让。
  用户态提供 /proc/stat EWMA 和 sysfs LLC 拓扑；BPF 最多看 8 个候选，
  排除应用 CPU，使用 500 ms 驻留、100/1000 收益阈值和 2 s 过期规则。
- TCP/connected UDP IPv4/IPv6；MPTCP master storage 与专用 accept/splice 观察；
  SCTP lookup/release kfunc 与 peeloff 生命周期。可选协议缺失时仍可加载基础策略。
  io_zcrx_recv observer 已实现，真实 NIC 零拷贝数据路径仍需设备验证。
- 停流 attach/drain/detach、原生 RPS/RFS 排他检查、固定 CPU 集合内的热插拔事件、
  online 后重建 worker、事件溢出后的保守重建。
- 硬件：BPF 发布有界、带 generation 的 CPU 意图，可跟随策略 CPU 或 socket CPU。
  RXNFC/ntuple 控制器管理 CPU→RX queue 映射、预留位置、持久意图、安装回读、
  删除确认、老化、重启恢复、设备身份；失败继续使用软件路径。
  这是 RXNFC 实现的硬件 RFS，不接管原生 ndo_rx_flow_steer/filter-id 到期协议。
- 测试：普通 CPUMAP 乱序阳性对照、真实 TCP GRO、多缓冲载荷、worker 定向 failslab、
  迁移/替换/热插拔、SCTP 并发 peeloff 和引用检查；RT/KASAN/lockdep、i386 水位运行、
  可选协议关闭配置和硬件控制器故障测试。准确结果见 `../dev/v2-validation.md`。
- 五组性能对照工具：固定种子、原始计数/perf/ethtool 采集、应用延迟与迁移报告输入。
  本地工具验证不充当物理性能数据。
- 本地 RFC 重组：原功能内修复/增强，独立 generic-XDP mac_len bugfix 前置，
  SCTP 两补丁可单独讨论；应用树校验、验证记录和外部策略随归档保存。

## 仍需设备或额外运行环境

- [ ] native XDP 多队列 NIC 与发包端：无 XDP、普通 CPUMAP、lookup/storage、
  ordered CPUMAP、native RFS 五组，测 pps/cycles/p99、迁移延迟、GRO、丢包/乱序。
- [ ] 量化 domain 锁、共享缓存行、跨 CPU 队列、全 GRO flush 代价，不能预称无劣化。
- [ ] 实际 RXNFC driver/firmware 的安装、回读、命中、迁移、资源耗尽及复位恢复。
- [ ] 实际 io_uring zero-copy RX、多路径 MPTCP/SCTP multihoming 流量验证。
- [ ] modular SCTP 装卸与策略引用运行测试；当前模块/无 BPF 配置只完成编译验证。

## 保留的设计边界和独立扩展

- 无停流交接、map 替换、动态 domain 分配/复用未实现；当前协议要求停流。
- 不宣称完整恢复 checksum/hash/VLAN；native loader 拒绝无法确认已关闭的 VLAN
  stripping。分片、VLAN、隧道、IPv6 扩展头、reuseport/VRF/DNAT 等最终 demux
  不在直接本地 unicast 策略的保序合同内。
- 硬件迁移可能在 XDP 前乱序；软件 barrier 不承诺跨硬件 RX 队列的线序恢复。
- 旧 rx_ext 的 G1–G5 任意策略隔离/自动接管仍是独立暂缓项；本原型有 verifier、
  有界资源和约定回退，没有 sched_ext 式 watchdog 或昂贵策略自动恢复。
- 投稿前由开发者审核设计/成本并确认 DCO，补公开外部仓库链接。当前只生成本地
  RFC 草稿，未发送社区，也未宣称 maintainer 接受。
