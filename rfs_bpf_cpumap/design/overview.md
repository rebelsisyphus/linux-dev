# CPUMAP RFS infrastructure v1

新系列基于 `9b87fdc9af2fbfcdb5c24a64139685ef80f6573f`，分支
`develop/rfs_bpf_cpumap`。旧 `rfs_bpf` v3 保留，不作为依赖。
前序方案：reference/cpumap-rfs-infrastructure-20260923.md。

## 实现边界

* XDP 增加 sk_storage_get，复用现有 socket lookup/refcount 验证。
* CPUMAP map_extra 非零选择可选保序模式，值为预分配 domain 数量。
  BPF 选择 domain 和候选 CPU；内核不存五元组、不决定策略。
* bpf_cpumap_redirect 只记录重定向意图；实际 redirect 时才保留 pending。
  源 bulk 尚未发布、旧目标完成水位未到时保留旧 CPU。
* 成功入队分配递增 ticket，失败只归还 pending。
  目标按 ring 顺序处理 native/generic 包；GRO 全量交付后发布完成水位。
* entry 引用保留旧 generation 的 progress，退休 entry 不接受新请求；
  替换/删除复用 CPU 号不会绕过旧队列。CPU 下线保留 native RFS 的例外。
* get_state 只提供快照，不能作为并发迁移许可。真正卸载/重建 domain
  需先停止入口并 drain。任意策略绕过 ordered redirect 不保证保序。
* 远端 CPUMAP 程序可 PASS/DROP；第二次 REDIRECT 丢弃，避免无法追踪的尾部。

## 性能与限制

普通 CPUMAP 不分配 domain/progress，仍有新增分支的成本。
有序模式每次提交需要 domain 锁与队列 producer 锁，无逐包分配。
GRO 每 64 包或空队列全量 flush，以明确完成边界；可能影响聚合率，
需真实多队列网卡测量。配置独立于 CONFIG_RPS。
完成边界到同步协议栈交付；后续 native RPS、NFQUEUE 等异步路径另论。
硬件 aRFS/ntuple、选核策略产品化和 G1–G5 策略隔离未纳入本系列。


## v1 完成状态

四个提交已导出到 patch/cpumap-infrastructure-v1。构建、15 个 QEMU
子用例、相关对象配置矩阵和补丁应用校验已完成，详见 dev/validation.md。
CPU online 后控制侧需重建 entry 以恢复绑定；初始化、卸载和域重用仍需
停流/drain。后续策略、硬件能力与扩大验证范围见 design/todo.md。

## BPF RFS 策略 v1（2026-09-28）

基础设施 v1 后追加三个策略/测试/文档补丁，末端 9de54e62c380。
实现与边界见 rfs-policy.md，最新补丁在 ../patch/cpumap-rfs-policy-v1。
完整回归 22 子用例与加载器专项通过，详情 ../dev/rfs-validation.md。
