# Sashiko 审查问题逐项核验

## 结论

本次核验覆盖 `sashiko-review-report.md` 中 10 个不重复的 finding（末尾
Top 3 是前文条目的重复汇总）。以报告对应的最终源码树为准，没有发现仍
然存在的功能正确性缺陷：

- 2 个历史问题已在最终树中修复：file-rmap 的 `VM_BUG_ON_VMA()` 语义、
  inode slab 复用时的动态 shard 状态重置。
- 2 个行为确实存在，但属于实现策略或必要同步代价，不是已证明的回归：
  固定淘汰 batch slot 0、file folio split 阻塞全部 shard writer。
- 2 个事实存在，但只是扩展性/性能范围说明：shard 数目前必须按 2 的幂
  使用、树内没有文件系统设置 `IOP_FASTPERM_MAY_EXEC`。
- 3 个所谓“中等风险”在当前代码中均没有可触发的错误：conditional
  reset、VMA 计数维护、`vp->insert` 的 mapping/shard 一致性。
- Patch 15 的架构 dcache walker 不是当前 bug，但在 NUMA-only 支持约束下
  属于不可配置、不可编译覆盖的死代码；本次已连同专用 helper 和安装锁删除。

因此，报告的总体 `LGTM` 结论可以保留，但把若干仅针对未来修改的假设标
为 Medium 偏重；更准确的定性应是 maintenance note。报告所称 file-THP
“latency regression”和 batch 淘汰性能影响都没有基准数据支撑，只能视为
待测假设。

## 核验范围与版本对应

- Base commit `cece3eeed8c9692d37c9b398eb91f7c69e93a101` 有效，报告中的
  26 个短 commit ID 均可解析，并且正好构成 26 个提交的序列。
- 报告 Metadata 中的完整 Final Tree Commit
  `8b936c9b6503eb576e3b572d91d453a1d3c8e3b4` 是错误的，不是仓库对象。
  短 ID `8b936c9b6503` 实际解析为
  `8b936c9b6503193e0650bb11ded13410dacf12d5`。
- 上述有效 final commit 的 tree 是
  `272dad44794f4b24c2c300bd17fd3ed6874d2809`，在本次修复前与 HEAD
  `2f9ce0c65853` 的 tree 完全一致。本次工作树在该 HEAD 上继续删除了
  Patch 15 的不可达非 NUMA 架构支持；以下源码行号以修复后的工作树为准。
- 报告还把 inode reset 的变更归到了 Patch 18。实际 Patch 18
  `aba6ec3fee7c` 只修改 `mm/mmap.c`；reset 在 Patch 4
  `291661804f19` 中加入，随后在 Patch 19 `f3269756da3d` 中加上
  `i_mmap_opt_enabled()` 条件。

## 本次修复与验证

- 删除 ARM（2 处）、Nios II、PA-RISC 的不可达 shard cache-walker 分支。
- 删除无调用者的 `i_mmap_walk_dcache_locked()` 公共声明/实现，以及只为这些
  walker 增加的 shard-installation dcache lock/unlock。
- 四个架构文件已与 Patch 15 前的对应 blob 逐文件校验一致。
- 当前 x86 配置启用 `CONFIG_NUMA=y` 和 `CONFIG_I_MMAP_SHARDS=y`；
  `make -j8 mm/mmap.o fs/inode.o kernel/fork.o` 通过。
- `git diff --check`、`scripts/checkpatch.pl --no-tree --terse -` 和残留符号
  扫描均通过。
- 已生成 `remove_unreachable_i_mmap_dcache.cocci`；环境缺少 `spatch`，因此
  `make coccicheck ... MODE=patch` 无法执行，精确反向补丁及上述检查作为回退。

## 逐项分析

### 1. Patch 03：batch 满时固定淘汰 slot 0

判定：**行为存在，但不是正确性问题；性能退化仅是可能性。**

`kernel/fork.c:855-881` 先遍历四个 slot 查找相同 mapping，同时记录第一
个空 slot；只有既无相同 mapping 又无空 slot 时，才在
`kernel/fork.c:870-873` flush `file_batches[0]`。因此报告对当前淘汰策略的
描述准确。

该行为不会丢失或重复插入 VMA。被淘汰的 batch 会在对应 mapping 的写锁
和 dcache lock 下完整写入，batch 随后清零；page-table copy 边界和
`arch_dup_mmap()` 前也会 flush 所有 slot。固定 slot 0 确实可能在 5 个以上
mapping 交错访问时形成不公平抖动，但是否比 round-robin/LRU 慢、慢多少，
报告没有测试数据。故存在的是“简单淘汰策略”，不是已确认的性能回归。

### 2. Patch 04：shard 状态 reset 受 `i_mmap_opt_enabled()` 限制

判定：**条件代码存在，但报告所述当前风险不存在。**

`fs/inode.c:208-213` 和 `fs/inode.c:419-424` 的 reset 的确只在 static key
为 true 时执行。但该 key 只能由 `mm/mmap.c:78-92` 的 `early_param` 设置；
`init/main.c:911` 在 `init/main.c:932` 初始化 VFS/inode cache 之前已经完成
early parameter 解析。当前树没有运行期重新开关该 key 的接口。

对生产源码中的三个动态字段做了穷举：

- key 为 true 时，每次 `inode_init_always()` 都重置 pointer、VMA count 和
  contention count；销毁路径还在 `fs/inode.c:289-297` 释放并清空 shards。
- key 为 false 时，shard load、VMA count、写锁快路径及
  `mapping_mapped()` 都由 `i_mmap_opt_enabled()` 拦截；这些字段不会参与运行。
- 首次构造还经过 `address_space_init_once()` 的整结构 `memset()`，所以禁用
  状态下也不是未初始化内存。

因此不存在“当前读者观察到 stale value”的执行路径。无条件 reset 可以
降低未来新增未受 key 保护的 reader 所带来的维护风险，但不能据此把当前
代码定为 Medium 问题。

### 3. Patch 04：`hash_ptr(..., ilog2(I_MMAP_SHARDS_PER_DOMAIN))`
假定 shard 数是 2 的幂

判定：**假定存在，当前完全正确，仅是低等级维护提示。**

`include/linux/fs.h:478-481` 把 `I_MMAP_SHARDS_PER_DOMAIN` 定义为 4，
`kernel/fork.c:1513-1516` 传入 `ilog2(4) == 2`，`hash_ptr()` 因而只可能返回
0..3，和数组范围完全匹配。

若以后把常量改成非 2 的幂，`ilog2()` 向下取整会导致部分 shard 永远不被
选择，但不会越界。例如改成 6 时只会产生 0..3。增加 `BUILD_BUG_ON()`
可以明确设计约束，但当前不存在 bug。

### 4. Patch 06：`i_mmap_nr_vmas` 计数不变量未由统一 helper 强制

判定：**当前不存在计数失配；报告描述的是未来修改风险。**

对最终树所有 file VMA interval-tree insert/remove 调用做了穷举：

- 永久插入：`mm/mmap.c:924-935` 的 `__vma_link_file()` 和
  `kernel/fork.c:694-713` 的 fork batch 都调用
  `i_mmap_vma_count_add()`。
- 永久删除：`mm/mmap.c:545-556` 的
  `__remove_shared_vm_struct()` 调用 `i_mmap_vma_count_sub()`。
- shard 安装在 `mm/mmap.c:289-301` 中只迁移既有 VMA，数量不变。
- split/merge 在 `mm/mmap.c:1069-1109` 中临时 remove 后重新 insert 同一批
  VMA，数量不变；新 split VMA 则通过 `__vma_link_file()` 单独加一。
- 优化关闭时，代码只使用 central tree，`mapping_mapped()` 也回退到该树；
  `mm/nommu.c` 路径因 `I_MMAP_SHARDS` 依赖 `MMU` 而与本配置互斥。

所以报告自己所说“没有漏记的当前路径”是正确的，继而假设“未来调用者可能
绕过 helper”不能构成当前 Medium 缺陷。集中封装 permanent mutation 与
temporary relocation 仍是合理的可维护性改进。

### 5. Patch 08：`vma_prepare()` 假定 `vp->insert` 与 `vp->vma`
使用相同 mapping/shard

判定：**当前不变量由唯一赋值路径保证，不存在错误。**

最终树中 `vp.insert` 只有一个赋值点：`mm/mmap.c:3085-3087` 的
`__split_vma()`。该函数先在 `mm/mmap.c:3052` 用 `vm_area_dup(vma)` 克隆
原 VMA；`kernel/fork.c:516-536` 表明 clone 会复制 `vm_file` 和 `vm_mm`。
随后只调整地址范围/`vm_pgoff`，当前所有 `vm_ops->open()` 实现也没有在这
条 split 路径上替换 `vm_file`。

因此 insert 与原 VMA 必然有相同 `f_mapping`，并因 `vm_mm` 相同而选择相同
NUMA domain 和 shard。其他 `init_multi_vma_prep()` 调用者均不设置
`vp->insert`；报告用 `vma_merge()` 说明此不变量并不必要，因为 merge 路径
根本不会设置该字段。增加 debug assertion 可以作为文档化手段，但当前不
存在错误路径。

### 6. Patch 10：file-rmap callback 弱化 `VM_BUG_ON_VMA()`

判定：**早期版本中存在，最终树已修复。**

基线的 `rmap_walk_file()` 对 `vma_address() == -EFAULT` 使用
`VM_BUG_ON_VMA()`。旧树
`backup/immap_opt_latest-before-rmap-vmbug-20260831` 的 shard callback
曾改成 `VM_WARN_ON_ONCE(true)` 后跳过该 VMA，确实改变了诊断语义。

最终树 `mm/rmap.c:2677-2679` 已恢复：

```c
address = vma_address(&walk->folio->page, vma);
VM_BUG_ON_VMA(address == -EFAULT, vma);
cond_resched();
```

这与非 shard 基线一致。已有 `unixbench/tests/RESULTS.md` 还记录了
`CONFIG_I_MMAP_SHARDS=y/n`、`CONFIG_DEBUG_VM=y` 的 `mm/rmap.o` 编译和启动
smoke test 均通过。最终树中该问题不存在。

### 7. Patch 15：dcache walker 依赖所有 shard-tree writer 都拿
dcache lock

判定：**原代码没有当前可达的竞态，但属于死代码；在 NUMA-only 约束下已删除。**

修复前的锁覆盖是完整的：shard 安装、永久 insert/remove、split/merge 临时
迁移和 fork batch 都在修改 interval tree 时持有
`flush_dcache_mmap_lock()`；walker 侧也持有同一把 mapping `i_pages`
xa_lock。因此原实现没有已证明的未受保护树修改。

`I_MMAP_SHARDS` 在 `mm/Kconfig:5-7` 依赖 `NUMA`，但 `arch/arm`、
`arch/nios2` 和 `arch/parisc` 都没有定义 `CONFIG_NUMA`。Patch 15 新增的
四个 `#ifdef CONFIG_I_MMAP_SHARDS` 分支不可能被选择，实际始终使用原来的
central-tree 实现。

既然功能明确只支持 NUMA 架构，本次删除了这四个分支、失去所有调用者的
`i_mmap_walk_dcache_locked()` 声明/定义，以及仅为这些 walker 序列化 shard
安装而增加的 dcache lock/unlock。四个架构文件恢复为 Patch 15 前的实现；
NUMA shard 的读写锁、遍历和安装逻辑没有改变。

### 8. Patch 17：file folio split 阻塞全部 shard writer

判定：**行为存在且是正确性所必需；没有证据表明构成回归。**

`mm/huge_memory.c:3691-3695` 对 file folio 调用
`i_mmap_lock_read_all()`，锁一直持有到 `mm/huge_memory.c:3786-3791`。
期间 `unmap_folio()` 以 `TTU_RMAP_LOCKED` 调用 `try_to_unmap()`，后者通过
`rmap_walk_locked()` 遍历全部 shard。`mm/mmap.c:337-368` 以固定升序取得
全部 shard read lock、逆序释放。因此每个对应 writer 在 split 期间确实会
被阻塞。

但基线代码在同一完整区间持有 central `i_mmap_rwsem` read lock，同样阻塞
该 mapping 的所有 writer。新实现只是把一次 central lock 变成最多
`nr_domains * 4` 次 read-lock 获取，并在此特殊路径暂时失去 sharding 的并
行收益；若不锁全，`TTU_RMAP_LOCKED` 遍历会与未锁 shard 的 writer 竞态。
所以这是必要语义和潜在额外开销，不是已经证明的 latency regression。需要
专门的 file-folio split 并发基准才能量化。

### 9. Patch 18 描述的 inode stale contention 问题

判定：**旧树中存在，最终树已修复；报告的 patch 归属不准确。**

旧树只在 slab constructor 初始化 `i_mmap_lock_contention`。constructor 不会
在 inode slab object 每次复用时重跑，因此旧 inode 留下的 `-1`（已 claim
安装）或正计数会被新 inode 继承。根据 `mm/mmap.c:193-208`，`-1` 会让新
inode 永远不再 claim shard installation；正计数则会改变其启用阈值。这是
真实的 inode-lifetime 状态隔离/性能行为问题。

最终树在 `inode_init_always()` 的每次分配路径执行
`fs/inode.c:208-213`，key 开启时把 pointer、VMA count、contention count
分别重置为 NULL/0/0；key 关闭时这些字段没有消费者。已有 inode-reuse
回归测试记录为：

```text
destroy_seen=1 reuse_seen=4 reset_ok=4 reset_bad=0 contention=0 nr_vmas=0 shards=0
```

故最终树中该问题不存在。实际代码历史上 reset 由 Patch 4 加入，而不是报告
所写的 Patch 18；Patch 19 只是把它改为 static-key 条件执行。

### 10. Patch 25：没有文件系统设置 `IOP_FASTPERM_MAY_EXEC`

判定：**事实存在，但不是缺陷。**

对最终树穷举 `IOP_FASTPERM_MAY_EXEC`，只有三处：

- `include/linux/fs.h:796` 的定义；
- `fs/namei.c:600` 的说明；
- `fs/namei.c:608` 的检查。

确实没有 in-tree filesystem 主动设置它，因此有自定义 `->permission` 的文
件系统当前不能使用这一 opt-in 分支。不过，没有自定义 hook 的文件系统会
在 `do_inode_permission()` 中自动获得 `IOP_FASTPERM`，仍可进入相同 fast
path。提交说明也明确指出 ext4 benchmark 依赖尚未合入的 companion patch。
所以这是 API 当前覆盖范围的事实，不是 correctness bug；若把基准收益表述
成当前单个 patch 即可获得，则才需要修正文案。

## 最终评价

| 类别 | 数量 | 条目 |
|---|---:|---|
| 最终树中的未修复正确性问题 | 0 | 无 |
| 已验证修复 | 2 | file-rmap diagnostic、inode reuse state |
| 已清理的不可达代码 | 1 | 非 NUMA 架构 dcache shard walker |
| 当前存在但非缺陷的策略/代价 | 4 | slot 0、power-of-two、all-shard split、未设置 fastperm flag |
| 当前安全、仅属未来维护假设 | 3 | conditional reset、VMA count、`vp->insert` |

报告的合入结论总体合理，但建议把未修复条目的严重性统一降为
informational/maintenance，并修正完整 final SHA、Patch 18 的归属说明以及
Patch 15 的 Kconfig 可达性结论。上述 SHA、归属和 Patch 15 清理均已在本次
工作树中完成。
