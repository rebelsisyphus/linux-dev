# `mmap_olk_opt` 正式优化方案设计

## 1. 方案定位

本方案以 `mmap_olk_opt/` 为主线重新适配当前 `unixbench` 分支，而不是继续扩展
`mmap_numa/`。

保留 `mmap_olk_opt` 的核心思路：

- 只对真正发生写锁争用的热点 `address_space` 启用分片；
- 使用稳定的 `vm_mm` 哈希，避免一个 NUMA 节点内的所有进程继续串行；
- 保留 fork 多 mapping 批处理和按 `address_space` 的退出批处理；
- 使用 KABI 保留字段承载新增状态；
- 分片一旦发布，在 inode 生命周期内保持稳定。

在此基础上吸收 `mmap_numa` 中合理的部分：

- 使用实验性 Kconfig，默认关闭；
- 按 NUMA 域隔离 shard，减少跨 socket 的锁 cacheline bouncing；
- 为全局 reader 和 `mm_take_all_locks()` 提供完整的全 shard 锁语义；
- 暂不支持 DAX、shmem 和 hugetlbfs 分片。

同时修复两个原方案和单补丁适配版本中已经发现的正确性、NUMA、本地性和可评审性
问题。`page_fault`/VMA-lock retry 修改不进入本系列，后续如有需要必须单独提交和测试。

第一阶段目标机器：

- 8 个 NUMA 节点；
- 所有节点都会并发执行 mmap/fork/exec/exit；
- 热点主要集中在 libc、动态加载器和可执行文件等少量普通文件 mapping。

## 2. 现有补丁问题清单

### 2.1 `mmap_numa` 的问题

1. **节点内仍然串行**

   每个 NUMA 域只有一个 shard。同一节点上的所有 `mm` 仍竞争同一把 rwsem，无法解决
   单节点拥有大量 CPU 时的写侧扩展问题。

2. **首次使用即分片，冷 mapping 也付出成本**

   普通文件第一次符合条件的 VMA 更新就尝试分配 shard，没有确认该 mapping 是否真正
   存在 `i_mmap_rwsem` 争用。

3. **“按 NUMA 分片”不等于 NUMA 本地内存**

   原实现通过一次普通 `kzalloc()` 连续分配所有 shard。整块内存通常位于触发转换的
   NUMA 节点，其他节点对应的 shard 仍可能是远程内存。

4. **相邻 shard 可能伪共享**

   `rb_root_cached` 和 `rw_semaphore` 合计接近 56 字节，未做 cacheline 对齐时，相邻
   shard 的锁或树根可能落在同一 cacheline。

5. **`vma_prepare()`/`vma_complete()` 原子性退化**

   原适配在 remove 和 reinsert 之间释放目标 shard 锁。分片发布后，中央锁不能阻止
   shard reader，这会暴露 VMA 暂时不在树中的中间状态。

6. **rmap trylock 语义不完整**

   部分路径只 trylock 中央 gate，随后阻塞获取所有 shard，违背调用者要求“不阻塞”的
   trylock 语义。

7. **遗漏直接访问 `mapping->i_mmap` 的 reader**

   `kernel/events/uprobes.c` 等路径仍只遍历中央树。mapping 分片后可能漏掉全部 VMA。

8. **KABI 使用不完整**

   直接向 `address_space` 和 `mm_struct` 增加字段，没有优先消耗当前分支提供的
   `KABI_RESERVE()`。

9. **调试统计侵入热路径**

   原 debugfs 统计在每次普通 VMA insert/remove 上执行原子操作，并使用固定 mapping
   指针哈希槽，既增加热路径成本，又存在碰撞和地址复用导致统计混淆的问题。

10. **补丁并非独立系列**

    `mmap_numa` 目录中的单补丁依赖旧分支已有的 task-domain 和 fork batching 提交，不能
    作为当前分支的完整可重放方案。

### 2.2 `mmap_olk_opt` 的问题

1. **纯 `vm_mm` 哈希可能引入跨 NUMA bouncing**

   不同 NUMA 节点上的进程会共同访问全部 8 个 shard，同一 shard 的锁 cacheline 可能
   在多个 socket 之间迁移。

2. **固定总计 8 个 shard 不适合 8-NUMA 节点内并发**

   如果为了 NUMA 隔离退化成每节点一个 shard，就失去了节点内扩展能力。

3. **缺少 Kconfig 隔离**

   原系列的动态分片代码常驻，不能通过默认关闭的实验配置控制正式部署范围。

4. **`mapping_mapped()` 存在发布窗口**

   原实现从 shard 指针中的 `nr_vmas` 判断映射状态。转换时最后一个 VMA 已从中央树
   移除、shard 指针尚未发布的短窗口可能产生假阴性；简单 `READ_ONCE()` 也没有和
   release publication 完整配对。

5. **trylock walker 可能先执行部分回调再返回 `-EAGAIN`**

   原 `i_mmap_read_walk_shards()` 逐 shard trylock 和遍历。后面的 shard 获取失败时，
   前面 shard 的回调已经执行，rmap 调用者重试后可能重复处理已有副作用。

6. **`rmap_walk_locked()` 重复加锁**

   `locked=true` 表示调用者已经持有相关 rmap 锁；原适配仍进入统一 reader 再次获取
   `i_mmap` 锁，可能形成递归读锁或 writer 排队下的自锁。

7. **`mm_take_all_locks()` 语义未补齐**

   原系列没有完整修改 `vm_lock_mapping()`/`vm_unlock_mapping()`。分片 writer 不再获取
   中央 `i_mmap_rwsem` 后，仅锁中央树无法阻止 shard 修改。

8. **仍有未转换 reader**

   uprobes 和部分架构 cache-maintenance walker 仍直接访问中央树；在非 x86 架构上
   尤其容易形成静默漏遍历。

9. **中间提交不是运行时安全状态**

   原系列先连接动态启用入口，后续补丁才补读侧 walker。若在中间提交二分或部署，
   reader 可能观察不到已迁移 VMA。

10. **特殊映射支持范围过大**

    原方案尝试同时覆盖 DAX 等路径，扩大首版正确性和测试面。正式首版应先排除这些
    mapping，待普通文件方案稳定后单独扩展。

### 2.3 `mmap_olk_opt_adapted` 单补丁的问题

1. 将批处理、分片、读写侧转换和 `page_fault` retry 合并为一个大补丁，无法独立评审、
   回退或性能归因。
2. 继承了 `mmap_olk_opt` 的 trylock、rmap locked、全局锁和遗漏 reader 问题。
3. 混入 `VM_FAULT_RETRY_VMA` 及多个架构 fault handler 修改，与 `i_mmap` 分片没有必要的
   功能依赖。
4. 适合作为快速测试产物，不适合作为正式提交格式。

## 3. 新方案如何解决上述问题

| 已知问题 | 新方案处理方式 |
| --- | --- |
| 纯 NUMA 方案节点内串行 | 每个 NUMA 域内再按 `vm_mm` 哈希到 4 个 shard |
| 纯哈希跨 NUMA bouncing | 不同 NUMA 域使用互不重叠的 shard 集合 |
| shard 内存仍在远端 | 每个域的 shard block 使用 `kzalloc_node()` 本地分配 |
| 相邻 shard 伪共享 | 每个 shard 按 `L1_CACHE_BYTES` 对齐 |
| 冷 mapping 内存和遍历开销 | 只有中央写锁出现持续争用后才转换 |
| `mapping_mapped()` 发布竞态 | `address_space` 维护转换前后连续有效的聚合 VMA 计数 |
| VMA remove/reinsert 中间状态 | `vma_prepare()` 获取的 shard 锁保持到 `vma_complete()` |
| trylock 部分执行 | 调用第一个 callback 前先成功获取完整锁集合 |
| `rmap_walk_locked()` 重复加锁 | 提供 caller-locked walker，不重新获取锁 |
| `mm_take_all_locks()` 失效 | 中央 gate 加全部 shard 写锁，逆序释放 |
| uprobes/架构 walker 漏转换 | 全树访问审计并统一使用 shard-aware helper |
| KABI 增长 | 优先通过 `KABI_USE()` 消耗现有 reserve 字段 |
| 中间提交不可运行 | 最后一个功能补丁才连接动态启用入口 |
| page-fault 修改混杂 | 完全移出本系列 |

## 4. 分片拓扑

### 4.1 8 NUMA × 每节点 4 shard

第一版推荐总计 32 个 shard：

```text
NUMA node 0 -> shard  0..3
NUMA node 1 -> shard  4..7
...
NUMA node 7 -> shard 28..31
```

初始常量：

```c
#define I_MMAP_MAX_DOMAINS		8
#define I_MMAP_SHARDS_PER_DOMAIN	4
#define I_MMAP_MAX_SHARDS		32
```

选择过程：

```text
domain = lookup_domain(vma->vm_mm->i_mmap_home_nid)
local  = hash_ptr(vma->vm_mm) & (I_MMAP_SHARDS_PER_DOMAIN - 1)
shard  = domain->shard[local]
```

对比关系：

```text
纯 NUMA：
  node0 -> shard0
  node1 -> shard1
  节点间隔离，但节点内串行

纯哈希：
  node0/node1 -> shard0..7
  分布均匀，但 shard 可能跨 socket bouncing

本方案：
  node0 -> shard0..3
  node1 -> shard4..7
  节点间隔离，同时提供节点内四路并行
```

### 4.2 稳定的 `mm` home NID

在 `mm_init()` 时记录 `mm_struct::i_mmap_home_nid`，并在整个 `mm` 生命周期保持不变。
`CLONE_VM` 线程共享已有值。

不能在每次 VMA 更新时使用当前 CPU 的 NID，否则任务迁移后，insert 和 remove 可能计算
出不同 shard，直接破坏 interval tree。

若 NID 无效或不在 mapping 的 domain 集合中，使用确定性的 fallback domain。首版假设
sharded mapping 存活期间机器的 possible-node 拓扑不变化；超过 8 个 possible node 时
保持中央树，不启用分片。

### 4.3 NUMA 本地分配

逻辑结构如下，具体字段可在实现时调整：

```c
struct i_mmap_shard {
	struct rb_root_cached root;
	struct rw_semaphore rwsem;
} ____cacheline_aligned_in_smp;

struct i_mmap_domain_shards {
	int nid;
	struct i_mmap_shard shard[I_MMAP_SHARDS_PER_DOMAIN];
};

struct i_mmap_shards {
	unsigned int nr_domains;
	struct i_mmap_domain_shards *domain[I_MMAP_MAX_DOMAINS];
};
```

每个 `i_mmap_domain_shards` 使用对应 NID 的 `kzalloc_node()` 分配。只有全部域分配并初始化
成功后才允许发布顶层指针。任何分配失败都会释放未发布对象并继续使用中央树，不允许
发布部分 shard 集合。

## 5. 激活策略与适用范围

新增实验配置：

```text
CONFIG_I_MMAP_SHARDS
  depends on MMU && SMP && NUMA && EXPERT
  default n
```

mapping 必须同时满足：

- 存在普通规则文件 inode；
- 不是 shmem；
- 不是 hugetlbfs；
- 不是 DAX；
- possible NUMA domain 数不超过 8。

写路径先 trylock 传统中央写锁。失败时增加饱和争用计数，初始阈值为 4；达到阈值后尝试
分配和安装 shard。阈值属于内部调优参数，不形成用户 ABI，正式测试必须同时比较 4 和
8。

一旦转换成功，在 `address_space` 生命周期内不反向合并，避免反复迁移 interval-tree
节点和引入新的状态机。

## 6. 数据结构、KABI 和 mapped 计数

`address_space` 需要三个状态：

- 已发布的 `i_mmap_shards` 指针；
- 在中央树迁移前后连续有效的聚合 VMA 数量；
- 中央写锁争用计数。

这些字段通过 `KABI_USE()` 消耗当前 `address_space` 的 reserve 字段。
`mm_struct::i_mmap_home_nid` 同样优先使用当前分支已有的 KABI reserve。不向
`vm_area_struct` 增加字段，因此没有每 VMA 内存开销。

`mapping_mapped()` 不能仅根据 shard 指针中的计数判断。转换过程中，最后一个 VMA 可能
已经离开中央树，而 shard 指针还没有对无锁 reader 可见。为消除这个窗口，聚合计数从
普通文件 VMA 第一次插入开始就维护，中央树迁移时保持不变。对于被排除的特殊 mapping，
仍保留中央树非空检查作为 fallback。

## 7. 发布与生命周期

转换流程：

1. 不持有 `i_mmap_rwsem`，为全部 NUMA 域分配并初始化本地 shard block；
2. 获取中央 `i_mmap_rwsem` 写锁；
3. 重新检查其他 writer 是否已经安装 shard；
4. 按稳定 home NID 和域内 `vm_mm` 哈希迁移所有中央树 VMA；
5. 聚合 mapped 计数保持不变；
6. 使用 `smp_store_release()` 发布顶层指针；
7. reader 使用 `smp_load_acquire()`；
8. inode 销毁、确定不存在 VMA 用户后释放全部 domain block。

发布后的指针不会恢复为 `NULL`。

## 8. 锁模型

### 8.1 全局锁顺序

```text
mm->mmap_lock
  -> mapping->i_mmap_rwsem（中央树/发布 gate）
    -> NUMA domain index 升序
      -> domain 内 shard index 升序
```

分片发布后，普通单 VMA writer 只获取目标 shard 写锁。任何路径都不能先持有 shard，再
反向获取中央 gate。

### 8.2 普通 writer

下列热路径常见情况下只涉及一个 shard：

- VMA link/unlink；
- 同一 child `mm` 的 fork 插入批次；
- 同一 `mm` 的 exit unlink 批次；
- 大部分 VMA split/merge。

若一次操作涉及多个 `mm`，先收集唯一 shard，按 domain/shard 编号排序后升序获取，逆序
释放，避免 ABBA。

### 8.3 `vma_prepare()` 原子性

`vma_prepare()` 一次收集 `vma`、`adj_next`、`insert`、`remove`、`remove2` 涉及的全部
shard，并保持这些锁直到 `vma_complete()` 完成所有 remove/reinsert。

严禁在 remove 和 reinsert 之间临时释放 shard 锁，因为分片发布后中央 gate 不能排斥
普通 shard reader。

### 8.4 `mm_take_all_locks()`

`vm_lock_mapping()` 对 sharded mapping 获取：

1. 中央 gate 写锁，阻止转换或中央树修改；
2. 按固定顺序获取全部 shard 写锁。

`vm_unlock_mapping()` 逆序释放。这样才能维持“阻止所有 rmap 修改”的原有契约。

## 9. reader 模型

提供两类接口：

- helper 自己获取完整锁集合并遍历；
- caller 已持有完整 mapping 锁集合时直接遍历 roots。

trylock walker 必须在执行第一个 callback 之前成功获取所需的全部读锁。任一 shard 获取
失败时，释放已获得的所有锁并返回 `-EAGAIN`，不允许先处理前几个 shard 再报告失败。

允许阻塞且不要求全 mapping 快照的 reader，可以逐 shard 加锁、遍历、解锁，以缩短锁
持有时间。要求一致快照或带副作用 trylock 的 rmap 路径应先锁定全部 shard。

`rmap_walk_locked()` 使用 caller 已持有的锁直接遍历，不能再次递归获取中央或 shard 锁。

必须审计和转换的直接访问者包括：

- `mm/rmap.c`；
- `mm/memory.c` 的 unmap walker；
- `mm/memory-failure.c`；
- `mm/khugepaged.c`；
- `mm/pagewalk.c`；
- `kernel/events/uprobes.c`；
- `fs/dax.c`，尽管首版仍排除 DAX 分片；
- ARM、parisc、nios2 等架构 cache-maintenance walker；
- `mm_take_all_locks()` 及其 locked rmap 用户。

NOMMU、shmem、hugetlbfs 和其他明确排除的路径继续使用中央树。

## 10. fork/exit 批处理

当前分支已经存在单个延迟 file-VMA link batch，因此正式适配是在现有实现上扩展：

- 同时保留 4 个活跃 `address_space` batch；
- 每个 batch 最多 8 个 VMA；
- 在需要 child VMA 已对 rmap 可见的 page-table-copy 边界前 flush 全部批次；
- 在 `arch_dup_mmap()` 前和所有退出路径 flush；
- unlink batch 以 `address_space` 而不是 `struct file` 为 key。

page-table-copy 判断需覆盖当前分支要求的 userfaultfd write-protect、`VM_PFNMAP`、
`VM_MIXEDMAP` 和 anon-vma-backed mapping。

## 11. 内存和遍历开销

x86-64 上，每个 cacheline-aligned shard 约占 64 字节。32 个 shard 约为 2 KiB，顶层
descriptor 和 8 个 domain 指针约 80～128 字节。考虑 slab 对齐和 8 次节点本地分配，
每个真正发生争用并转换的 mapping 预计额外使用约 2.2～2.5 KiB。

| 已分片热点 mapping 数 | 预计额外内存 |
| ---: | ---: |
| 50 | 110～125 KiB |
| 100 | 220～250 KiB |
| 500 | 1.1～1.25 MiB |
| 1000 | 2.2～2.5 MiB |

普通 write 更新仍通常只获取一把 shard 锁。

完整 walker 从一次锁和一棵树查询变成最多 32 次读锁、32 棵树查询和 32 次解锁：

```text
传统中央树：O(log N + K)
32 shard： O(32 * log(N / 32) + K)
```

`K` 是实际命中并处理的 VMA 数。空查询和低命中查询最容易暴露固定开销；libc/loader
等拥有大量匹配 VMA 的热点 mapping 中，callback 处理成本通常会摊薄额外 root probe。

一个 node 上运行的全局 walker 最多访问 4 个本地 shard 和 28 个远程 shard。完整的
无争用锁扫描预计增加数微秒到十几微秒，必须在目标 8-NUMA 机器上实测，不能仅依赖
双节点 QEMU。

## 12. 正式补丁拆分

实际实现拆分为以下 18 个提交。直到最后一个提交才连接争用触发入口，之前的提交只准备
状态、锁和 reader，避免出现“已迁移 VMA 但 reader 尚未转换”的不可运行中间状态：

1. `mm: batch file VMA insertions in dup_mmap`
2. `mm: batch file VMA unlink by address_space`
3. `mm: keep multiple file VMA batches in dup_mmap`
4. `mm: add optional NUMA-local i_mmap shard state`
5. `mm: allocate i_mmap shards on their NUMA nodes`
6. `mm: account file VMAs independently of i_mmap layout`
7. `mm: add NUMA-aware i_mmap shard installation`
8. `mm: route file VMA updates through shard-aware locks`
9. `mm: add all-shard i_mmap read iteration`
10. `mm: make core file rmap walkers shard-aware`
11. `mm/memory-failure: walk sharded file mappings`
12. `mm/khugepaged: find retractable mappings in all shards`
13. `mm/pagewalk: traverse every i_mmap shard`
14. `uprobes: build registration maps from all i_mmap shards`
15. `mm: make architecture i_mmap cache walkers shard-aware`
16. `mm: preserve all-lock semantics for sharded i_mmap`
17. `mm: hold all i_mmap shards for locked file rmap`
18. `mm: enable NUMA-local i_mmap shards after contention`

首版没有加入 debugfs ABI。运行验证使用 kprobe 只观测低频的安装函数，从而确认压测确实
触发了动态转换，又不会在正式热路径上保留每次 insert/remove 的统计开销。若目标机调优
确实需要常驻统计，应作为独立后续补丁，只记录 trylock 失败、转换、分配失败等低频事件。

## 13. 验证方案

### 13.1 编译和静态检查

- `CONFIG_I_MMAP_SHARDS=n` 构建；
- `CONFIG_I_MMAP_SHARDS=y` 和 `CONFIG_PROVE_LOCKING=y` 构建；
- 每组逻辑补丁完成后构建相关目标对象；
- 完整 x86-64 内核构建；
- 对每个独立 patch 运行 `scripts/checkpatch.pl --strict`；
- 重新执行全树审计，确保支持的普通文件路径不再直接遍历中央 `mapping->i_mmap`。

### 13.2 功能和并发测试

- fork/exec/exit 和 spawn 压测；
- 并发 mmap/munmap/mprotect/mremap、VMA split/merge；
- mapping churn 下的 truncate 和 `unmap_mapping_range()`；
- page reclaim、migration 和 file rmap；
- mapping 创建/删除期间的 uprobe 注册和注销；
- khugepaged 和 pagewalk；
- 验证 shmem、hugetlbfs、DAX 始终保留中央树；
- lockdep、KASAN 以及可用的竞态检测配置；
- 使用仓库 kernel development loop 完成 QEMU 启动和 spawn 测试。

QEMU 只能做功能性 NUMA 验证，不能替代目标 8-NUMA 机器的性能和远程访存验证。

### 13.3 目标机器性能矩阵

| 总 shard | 每 NUMA 节点 shard | 目的 |
| ---: | ---: | --- |
| 8 | 1 | 纯 NUMA 基线 |
| 16 | 2 | 保守混合方案 |
| 32 | 4 | 推荐初始配置 |
| 64 | 8 | 只用于确认收益是否饱和 |

至少测试两种绑核方式：

- 所有 worker 固定在同一 NUMA 节点，验证节点内哈希扩展性；
- worker 均匀分布在全部 8 个节点，验证跨 socket cacheline 流量。

采集指标：

- workload 吞吐和尾延迟；
- 中央 `i_mmap_rwsem` 和各 shard rwsem 争用；
- shard 分布；
- `perf c2c` cacheline bouncing；
- NUMA 本地/远程访存计数；
- reclaim/rmap-heavy workload；
- 空、低命中和高命中 full walker 延迟。

只有在 32 shard 相比 16 shard 明显提高写密集负载，且没有不可接受的 rmap/reclaim 回退
时才选 32。如果二者性能接近，应选 16 以减半完整遍历开销。除非 32 下仍存在明确的
节点内 shard rwsem 争用，否则不选 64。

## 14. 合入必须满足的正确性约束

1. 同一 VMA 的 insert/remove 永远计算出相同 shard。
2. 所有 root 和锁完成初始化前，其他 CPU 看不到 shard 指针。
3. trylock walker 获取完整锁集合前不执行任何 callback。
4. `vma_prepare()` 不暴露 remove/reinsert 中间状态。
5. `mm_take_all_locks()` 能阻止中央和全部 shard modifier。
6. 被排除的 mapping 不允许中央树和 shard 树混存普通 VMA。
7. 任意分配失败都完整回退中央路径。
8. Kconfig 关闭时保持传统行为，不给热路径增加分片开销。
9. 所有支持的直接 `i_mmap` reader 均已转换或被明确证明只处理排除类型。
10. 正式系列不包含任何 page-fault retry 修改。

## 15. 当前分支实施与验证记录

实现基于 `unixbench` 分支的 `3d5d2ff074a2`，补丁范围不包含 page-fault retry。

- 18 个提交逐提交通过 `scripts/checkpatch.pl --strict`；
- `CONFIG_I_MMAP_SHARDS=n` 时完成 x86-64 全量构建并生成 `bzImage`；原始配置曾在最终
  链接处暴露已有的 `__x64_sys_xsched_setattr`/`__x64_sys_xsched_getattr` 缺失，启用该
  分支 syscall 实现所属的 `CONFIG_XCU_SCHEDULER` 后基线问题消失；
- `CONFIG_I_MMAP_SHARDS=y`、THP、memory-failure 配置下完成 x86-64 全量构建；
- 增加 `CONFIG_PROVE_LOCKING=y` 后再次完成全量构建并生成 `bzImage`；
- QEMU 使用 2 个 NUMA 节点、4 个 vCPU 启动，lockdep/KASAN 内核成功进入用户态并建立
  SSH；
- 同一普通文件上的 12 进程 mmap/fork/mprotect/madvise/munmap 压测通过；
- kprobe 观测到 `i_mmap_shards_install_locked()` 执行，证明测试覆盖了实际 shard 转换，
  而不是只运行中央树 fallback；
- 压测后 `kernel.tainted=0`、`debug_locks=1`，串口和 dmesg 未发现 panic、BUG、WARNING、
  lockdep circular dependency 或 KASAN 报告。

QEMU 只覆盖功能和锁正确性。32 shard 相对 8/16 shard 的吞吐、遍历延迟和远程访存收益，
仍必须按 13.3 节在目标 8-NUMA 机器上完成性能矩阵后才能确定正式部署参数。
