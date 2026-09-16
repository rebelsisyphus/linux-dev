# 新增四前置的依赖与 newbugfix 核对

检索主线快照：9b87fdc9af2fbfcdb5c24a64139685ef80f6573f（2026-09-15）。
直接引用检索含完整提交消息，并递归检查 ab0beafd52b9；命令及结果见
fixes-recursive.log。相关主题历史补查见 topic-history.log。

| 新增提交 | 依赖与后续修复结论 |
|---|---|
| a2a0ffb08468 | struct_size() 在目标中可用，match 的尾部 f[] 已存在；替换两处分配大小计算，不要求结构压缩。未找到直接 Fixes。 |
| b9f052dc68f6 | nft_pernet()、per-net commit_mutex 已存在。添加 lockdep 判断；按需克隆及 UPDATE walk 使用同一锁。未找到直接 Fixes。关闭 PROVE_RCU 的配置下，旧 RCU 宏会消去条件表达式，编译提示静态 helper 未使用；保留上游实现。 |
| 9dad402b89e8 | 26cec9d4144e 已排在它前面；统一 opaque 元素指针，保持旧分配失败返回 NULL、单表达式扩展和 bool flush 约定。必须带入 ab0beafd52b9，以移除 GET 中误加的共享静态指针。 |
| f04df573faf9 | 只增加 const 限定。与已回合 791a615b7ad2 的 AVX2 slow lookup 上下文适配，保留原有 match 参数和 resmap 初始化。未找到直接 Fixes。 |

ab0beafd52b9 的 Fixes 明确指向 9dad402b89e8；继续按该修复 SHA 检索，
未找到后续直接 Fixes。并检查 nft_pipapo_get() 错误返回、各 backend GET、
动态集合 update/new、销毁和 GC 的类型传播，没有发现需要新增的逻辑前置。
这表示在上述可用主线历史和调用路径核对范围内未发现遗漏，不是对未来补丁的保证。

原始邮件/patchset：

- 9dad： https://lists.openwall.net/netdev/2023/10/25/241
  是 19 补丁系列的第 16 个；前面的 26cec9d4144e 已包含。
- f04 v2： https://www.spinics.net/lists/netfilter-devel/msg85309.html
- f04 四补丁封面： https://www.spinics.net/lists/netfilter-devel/msg85308.html
  constify 是第一个补丁，不依赖后续 ZERO_SIZE_PTR 简化或结构压缩。
- 后续修复： https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/commit/?id=ab0beafd52b98dfb8b8244b2c6794efbc87478db

未扩大回合范围的同组/上下文提交：

| 提交 | 判断 |
|---|---|
| 6509a2e410c3 | .flush 的 bool 返回值改成 void。元素参数类型转换不依赖该变化；目标继续返回 true，frontend 继续检查返回值。 |
| 0e1ea651c971 | 9dad 后续的事务元素内存压缩。目标继续保存 nft_set_elem，回滚、walk 回调按原接口适配。 |
| 078996fcd657 | insert 的 EEXIST 输出改用 opaque 元素指针。目标原有 ext 输出满足调用者，独立于本次类型调整。 |
| 07ace0bbe03b | 跳过空集 scratch 分配；目标的零长度分配/零规则 lookup 路径仍返回 ENOENT，无额外逻辑依赖。 |
| aac14d516c2b | PIPAPO 结构压缩和循环变量类型调整，作为上下文差异处理。 |
| a590f4760922 | 移动 lockdep helper 的定义位置；在按需克隆适配中调整位置即可。 |
| d8d871a35ca9 | GET 与 packet lookup 合并，未纳入本次四前置请求。保留已适配的独立 GET 实现。 |

目标已有 29b359cf6d95（READ/UPDATE 视图区分）、7395dfacfff6（事务时间戳）、
e79b47a8615d（失败删除恢复元素）以及 9df95785d3d8（GC unlink/reclaim 分离），
均保留。原系列已包含 fb7fb4016300，继续放在主 CVE 修复 47e65eff5069 前面。

用户明确指示 nft 不需要 kABI 修复。新版 17 补丁均来自上游，排除旧版 iterator
KABI_EXTEND_ENUM 补丁，不增加 opaque 指针 kABI 补丁。
