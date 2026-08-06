# NUMA 本地化 `i_mmap` 动态分片设计

## 1. 文档目的

本文描述普通文件反向映射树 `address_space::i_mmap` 的动态分片优化。设计面向多 NUMA、
大量 CPU 并发执行 `fork`、`exec`、`mmap` 和进程退出的场景，目标是在保持文件 rmap
完整语义的前提下，降低热点文件上 `i_mmap_rwsem` 的写锁串行和跨 NUMA cacheline
迁移。

本文对应当前分支从 `3d5d2ff074a2` 到 `4207a728a280` 的实现，范围包括：

- fork 文件 VMA 插入批处理；
- 退出路径文件 VMA 删除批处理；
- 普通文件 `i_mmap` 的 NUMA 本地动态分片；
- rmap、memory-failure、khugepaged、pagewalk、uprobes 和架构 cache walker 的分片遍历；
- `mm_take_all_locks()` 和 caller-locked rmap 的完整锁语义。

本设计不修改 page fault、filemap fault、folio lock 和 folio waitqueue。优化后如果性能热点
转移到这些路径，应将其作为独立问题分析和优化。

## 2. 设计目标

设计需要同时满足以下目标：

1. 热点普通文件的 VMA 插入、删除可以在不同 NUMA 节点间并行。
2. 同一 NUMA 节点内提供多路写并行，适配单节点包含大量 CPU 的机器。
3. shard 锁和树根尽量位于写入者所在 NUMA 节点，减少远程写和锁 cacheline bouncing。
4. 只为已经表现出写锁争用的 `address_space` 分配 shard，避免冷文件承担大块常驻内存。
5. 一个 `mm` 的同一文件 VMA 在整个生命周期内始终落入同一 shard。
6. 所有全局 reader 必须覆盖全部 shard，不允许漏遍历或重复执行有副作用的 callback。
7. 转换失败时继续使用中央树，不能暴露部分初始化或部分迁移状态。
8. `CONFIG_I_MMAP_SHARDS=n` 时保持原有中央树行为。

## 3. 优化原理

### 3.1 将单点写锁拆成 NUMA 域内并行锁

每个可分片的 `address_space` 最多为 8 个 NUMA 域建立分片，每个域固定包含 4 个
shard：

```text
shard 总数 = possible NUMA domain 数 × 4
```

普通 VMA writer 根据 `mm` 的稳定 home NID 选择 NUMA 域，再使用 `vm_mm` 指针哈希选择
域内 shard：

```text
domain = domain_for(mm->i_mmap_home_nid)
shard  = domain->shard[hash_ptr(mm) & 3]
```

不同 NUMA 域上的 writer 使用互不重叠的锁集合。同一域中的多个 `mm` 被分散到 4 把锁，
将单个热点 `address_space` 的普通写路径扩展为最多 `4 × nr_domains` 个并行通道。

同一个 `mm` 的 home NID 在 `mm_init()` 时确定，之后不随任务迁移而改变；域内选择只依赖
稳定的 `vm_mm` 地址。因此同一 VMA 的插入、修改和删除会重复计算出同一 shard。

### 3.2 NUMA 本地分配和 cacheline 隔离

每个 `i_mmap_domain_shards` 使用 `kzalloc_node(..., nid)` 在对应 NUMA 节点分配。普通
writer 访问本域的 shard root 和 rwsem，锁状态的写入通常留在本地节点。

`struct i_mmap_shard` 使用 `____cacheline_aligned_in_smp` 对齐，避免相邻 shard 的锁状态
共享 cacheline。顶层 descriptor 在发布后只读，其他节点读取它可能产生远程读或缓存副本，
但不会像共享写锁那样持续发生所有权迁移。

### 3.3 按争用动态激活

mapping 初始始终使用中央 `i_mmap` 树。写路径先尝试获取中央写锁：

- trylock 成功，直接在中央树完成操作；
- trylock 失败，对该 mapping 的饱和争用计数加一；
- 计数达到 4，由一个 writer 取得安装资格并创建 shard；
- 分配或安装失败，计数恢复为 0，mapping 继续使用中央树。

转换成功后 shard 指针在 inode 生命周期内保持不变，不执行反向合并。这样避免频繁迁移
interval-tree 节点，也使 reader 和 writer 的状态判断保持简单。

### 3.4 批处理摊薄锁操作

fork 和退出会在很短时间内对同一批文件 VMA 执行连续的 link/unlink。设计将相同
`address_space` 的操作聚合后，在一次写锁临界区内处理最多 8 个 VMA：

- `dup_mmap()` 同时保留 4 个活跃 mapping batch，每个 batch 最多容纳 8 个 VMA；
- 退出路径按 `address_space` 聚合连续 VMA，每批最多容纳 8 个 VMA；
- 在 page-table-copy、`arch_dup_mmap()` 以及错误退出等语义边界前强制 flush。

批处理减少 rwsem 获取、释放和唤醒次数，也减少高并发下反复进入同一 interval tree 的成本。

## 4. 总体架构

```text
                              一个热点普通文件
                         struct address_space
┌──────────────────────────────────────────────────────────────────────┐
│ 控制与兼容层                                                         │
│                                                                      │
│  i_mmap_rwsem          中央树锁、转换 gate                           │
│  i_mmap                分片发布前承载全部 VMA                        │
│  i_mmap_shards ──────┐ release/acquire 发布的只增不减指针            │
│  i_mmap_nr_vmas       跨中央树/分片树连续有效的 VMA 总数             │
│  i_mmap_lock_contention  中央写锁争用与安装认领状态                  │
└───────────────────────│──────────────────────────────────────────────┘
                        │
                        ▼
              struct i_mmap_shards
        ┌─────────────────────────────────┐
        │ nr_domains                      │
        │ domain[0..nr_domains-1]         │
        └───────┬───────────────┬─────────┘
                │               │
       kzalloc_node(node 0)     │             kzalloc_node(node D-1)
                ▼               ▼                         ▼
        ┌──────────────┐  ┌──────────────┐       ┌──────────────┐
        │ NUMA domain 0│  │ NUMA domain 1│  ...  │NUMA domain D-1│
        │ shard[0]     │  │ shard[0]     │       │ shard[0]      │
        │ shard[1]     │  │ shard[1]     │       │ shard[1]      │
        │ shard[2]     │  │ shard[2]     │       │ shard[2]      │
        │ shard[3]     │  │ shard[3]     │       │ shard[3]      │
        └──────────────┘  └──────────────┘       └──────────────┘
             每个 shard = cacheline 对齐的 interval-tree root + rwsem

 普通 writer：mm home NID ──► 本地 domain ──► hash(vm_mm) ──► 一个 shard
 全局 reader：按 domain/shard 固定顺序获取全部锁 ──► 遍历全部 root
```

在 4-NUMA 机器上，一个热点 mapping 建立 16 个 shard；在 8-NUMA 机器上建立 32 个
shard。domain 数取自 `N_POSSIBLE` 节点集合，而不是当前 workload 实际使用的节点数。

## 5. 核心数据结构

```c
#define I_MMAP_MAX_DOMAINS          8
#define I_MMAP_SHARDS_PER_DOMAIN    4

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

`address_space` 使用已有 KABI reserve 保存以下状态，不增加结构体尺寸：

- `struct i_mmap_shards *i_mmap_shards`；
- `atomic_t i_mmap_nr_vmas`；
- `atomic_t i_mmap_lock_contention`。

`mm_struct` 同样使用 KABI reserve 保存 `int i_mmap_home_nid`。设计不向
`vm_area_struct` 增加字段，因此不存在按 VMA 数量增长的额外对象内存。

## 6. 关键流程

### 6.1 VMA 写路径和动态转换流程

```text
[A] acquire-load mapping->i_mmap_shards
      │
      ├─ 已发布 ──────────────────────────────────────────────► [S]
      │
      └─ 未发布 ─► [B] trylock 中央 i_mmap_rwsem
                         │
                         ├─ 成功 ─────────────────────────────► [C]
                         │
                         └─ 失败 ─► [D] mapping 合格且争用计数达到 4？
                                           │
                                           ├─ 否 ─► 阻塞获取中央写锁 ─► [C]
                                           │
                                           └─ 是（唯一认领者）
                                                    │
                                                    ▼
                                             分配全部 NUMA domain
                                                    │
                                             获取中央写锁并再次检查
                                                    │
                                             迁移全部中央树 VMA
                                                    │
                                             release-store 发布指针
                                                    │
                                                    └─────────► 重试 [A]

[C] 已持有中央写锁，在锁内重新检查 i_mmap_shards
      │
      ├─ 仍未发布 ─► 在中央 root 操作 ─► 释放中央锁 ─► 结束
      │
      └─ 已发布 ───► 释放中央锁 ──────────────────────────────► [S]

[S] 用 home NID 选择 domain，用 hash(vm_mm) 选择 shard
      │
      └─► 获取一个 shard 写锁 ─► 在 shard root 操作 ─► 释放 ─► 结束
```

安装过程中只有在所有 domain、root 和 rwsem 初始化成功后才获取中央写锁。中央写锁保护
中央树迁移，`smp_store_release()` 在迁移完成后发布指针；reader 使用
`smp_load_acquire()`，因此不可能看到未初始化或只迁移了一部分的 shard 集合。

`i_mmap_nr_vmas` 在 VMA 初次进入中央树时已经开始维护，中央树向 shard 迁移时不改变该
计数。因此 `mapping_mapped()` 在转换窗口中仍能得到连续有效的结果。

### 6.2 全局 reader 流程

```text
开始按文件 offset 范围执行 rmap/walker
                │
                ▼
 acquire-load i_mmap_shards
                │
          ┌─────┴─────┐
          │已发布？   │
          └─────┬─────┘
          否    │     是
          │     │      │
          ▼     │      └─────────────────────┐
 获取中央读锁   │                            │
          │     │                            │
 锁内再次检查指针                            │
          │                                  │
    ┌─────┴─────┐                            │
    │仍未发布？ │                            │
    └─────┬─────┘                            │
    是    │     否                           │
    │     │      │                           │
    ▼     │      └─► 释放中央读锁 ───────────┤
 遍历中央 root                                │
    │                                        ▼
    │                          按固定顺序获取全部 shard 读锁
    │                                        │
    │                          遍历全部 domain 的全部 root
    │                                        │
    ▼                                        ▼
 释放中央读锁                     逆序释放全部 shard 读锁
```

trylock reader 在执行第一个 callback 前必须成功获取全部 shard 读锁。如果任一锁获取
失败，则逆序释放已经取得的锁并返回 `-EAGAIN`。这样重试不会重复前面 shard 上已经执行的
副作用。

`rmap_walk_locked()` 等 caller-locked 路径直接遍历调用者已经锁定的全部 root，不会递归
获取 rwsem。`mm_take_all_locks()` 先获取中央 gate，再按固定顺序获取全部 shard 写锁，
释放时使用逆序，维持“阻止该 mapping 全部 rmap 修改”的原有契约。

### 6.3 fork/exit 批处理流程

```text
dup_mmap() 扫描父进程 VMA
          │
          ▼
按 address_space 查找 4 个活跃 batch 槽位
          │
          ├─ 命中槽位 ─► 追加 VMA
          ├─ 空槽位   ─► 建立新 batch 后追加
          └─ 无空槽位 ─► flush 一个槽位后复用
                              │
            每批达到 8 个 VMA ┤
            需要 copy_page_range() 边界 ┤
            进入 arch_dup_mmap() 前     ┤──► 获取一次目标写锁
            错误或函数退出              ┘    批量插入并更新计数

exit/unmap 扫描 VMA
          │
          ▼
聚合同一 address_space 的连续 VMA，最多 8 个
          │
          ├─ mapping 改变
          ├─ batch 已满
          └─ 扫描结束
                 │
                 └──► 获取一次目标写锁，批量删除并更新计数
```

当 mapping 已分片时，同一 child `mm` 的文件 VMA 具有相同 home NID 和 `vm_mm` 哈希，
同一个 batch 内所有 VMA 必然属于同一 shard，因此一次写锁可以安全覆盖整个批次。

## 7. 锁模型与并发约束

### 7.1 锁顺序

全局顺序为：

```text
mm->mmap_lock
  -> mapping->i_mmap_rwsem（中央树和转换 gate）
    -> domain index 升序
      -> shard index 升序
```

全部 shard 的解锁顺序与加锁顺序相反。普通分片 writer 只获取目标 shard，不再反向获取
中央 gate。

### 7.2 VMA 更新原子性

VMA split、merge 和边界调整可能先从 interval tree 删除 VMA，再修改范围并重新插入。
`vma_prepare()` 获取目标 mapping/shard 写锁后一直保持到 `vma_complete()` 完成，reader
不会观察到 VMA 临时消失的中间状态。

同一次更新涉及的文件 VMA属于同一个 `mm` 和 `address_space`，稳定的 home NID 和
`vm_mm` 哈希保证它们位于同一个 shard。

### 7.3 生命周期

shard 集合具有以下生命周期：

```text
CENTRAL
  │ 第 4 次可分片写锁争用，由一个 writer 认领
  ▼
ALLOCATING（尚未发布，其他线程继续看到 CENTRAL）
  │ 全部分配成功，并在中央写锁下完成迁移
  ▼
SHARDED（release 发布，inode 生命周期内保持稳定）
  │ inode 已无 VMA 用户并进入销毁
  ▼
FREE
```

分配失败会从 `ALLOCATING` 回到 `CENTRAL`。已经发布的对象只在 `__destroy_inode()` 中
释放，因此 reader 获取指针后不需要额外引用计数或 RCU 回收。

## 8. 启用条件和支持范围

功能由以下实验配置控制，默认关闭：

```text
CONFIG_I_MMAP_SHARDS
  depends on MMU && SMP && NUMA && EXPERT
  default n
```

动态转换要求 mapping 同时满足：

- inode 为普通规则文件；
- VMA 的 `vm_file->f_mapping` 与目标 mapping 一致；
- 不是 shmem；
- 不是 DAX；
- 不是 hugetlbfs；
- possible NUMA 节点数不超过 8。

不满足条件的 mapping 始终使用中央 `i_mmap` 树。分片数量目前是内核编译期常量，不提供
用户 ABI 或运行时调整接口。

## 9. 预期开销

### 9.1 常驻内存

以下尺寸来自当前分支构建出的 `vmlinux`：

```text
sizeof(struct i_mmap_shard)          = 192 B
sizeof(struct i_mmap_domain_shards)  = 832 B
sizeof(struct i_mmap_shards)         = 72 B
```

当前配置中的 `rw_semaphore` 为 152 字节，所以每个 cacheline 对齐 shard 占 192 字节。
一个已转换 mapping 的结构体请求内存为：

```text
Mrequested(D) = 72 + 832 × D 字节
```

| possible NUMA 域数 | shard 数 | 结构体请求内存 | 典型 slab 实际占用估算 |
| ---: | ---: | ---: | ---: |
| 4 | 16 | 3,400 B（3.32 KiB） | 约 4.09 KiB |
| 8 | 32 | 6,728 B（6.57 KiB） | 约 8.09 KiB |

slab 估算按 72 字节顶层对象进入约 96 字节缓存、每个 832 字节 domain block 进入约
1 KiB 缓存计算；精确值取决于目标内核的 slab 配置、调试选项和 kmalloc size class。

只有达到争用阈值并成功转换的 mapping 才承担这部分内存。中央状态使用 KABI reserve，
`address_space` 和 `mm_struct` 不因启用配置扩大，`vm_area_struct` 没有新增字段。

批处理使用固定的栈内存：

- `dup_mmap()` 的 4 个 batch 共 576 字节；
- unlink batch 为 80 字节。

### 9.2 普通写路径 CPU 开销

已分片后的单 VMA 写操作包含：

1. 一次 acquire-load；
2. 最多扫描 8 个 domain descriptor 以匹配 home NID；
3. 一次 `vm_mm` 哈希；
4. 一次目标 shard rwsem 获取和释放；
5. 一次 shard interval-tree 更新；
6. VMA link/unlink 时对聚合计数执行一次原子加减。

若一个 mapping 共有 `V` 个 VMA，分布近似均匀，则目标树平均包含
`V / (4D)` 个节点，树操作复杂度约为 `O(log(V / (4D)))`。domain descriptor 在发布后
只读，扫描本身不会造成写侧 cacheline bouncing。

聚合 VMA 计数仍是所有 shard 共享的原子变量，跨 NUMA link/unlink 会写同一个
cacheline。这是换取 `mapping_mapped()` 在迁移期间无窗口正确性的固定成本，需要在极端
VMA churn 场景中单独观察其 cacheline 流量。

### 9.3 转换开销

每个热点 mapping 最多转换一次。转换包含：

- `D + 1` 次内存分配；
- 初始化 `4D` 个 interval-tree root 和 rwsem；
- 在中央写锁下迁移 `V` 个 VMA；
- 一次 release publication。

迁移需要对中央树执行逐节点删除并插入目标 shard，属于一次性的 `O(V log V)` 级工作。
转换期间该 mapping 的中央 writer 会等待，因此争用阈值用于避免短暂抖动触发不必要的
转换。

### 9.4 全局遍历开销

完整 reader 需要访问全部 shard：

```text
锁操作数：4D 次加读锁 + 4D 次解读锁
树查询：  4D 个 interval-tree root
复杂度：  O(4D × log(V / (4D)) + K)
```

其中 `K` 是 offset 范围内实际命中的 VMA 数。在 4-NUMA 机器上最多处理 16 把锁和
16 个 root，在 8-NUMA 机器上最多处理 32 把锁和 32 个 root。

空查询、低命中 pagewalk/rmap 最容易暴露固定 root probe 开销；高命中 walker 的成本更
多由 `K` 个 callback 决定。全局 reader 还会访问其他 NUMA 节点的 domain block，因此
这类低频全局操作允许发生远程读，普通高频 writer 的本地性是优先优化目标。

## 10. 预期优化效果

### 10.1 写锁并行能力

设一个 NUMA 节点同时有 `M` 个不同 `mm` 更新同一热点 mapping，域内 4 个 shard 的理想
平均负载为：

```text
每个 shard 的并发 writer 数约为 M / 4
任意两个独立 mm 哈希到同一 shard 的概率为 1 / 4
```

不同 NUMA 域之间不会选择同一 shard，因而普通 writer 的锁 cacheline 不需要在 NUMA
域间反复转移。最大写并行通道数为 4-NUMA 机器 16 路、8-NUMA 机器 32 路；实际扩展性
还会受哈希分布、VMA 数量、内存分配位置和后续瓶颈限制。

### 10.2 锁临界区摊薄

对同一 mapping 的连续 VMA 操作，单次 batch 最多用一次锁处理 8 个 VMA。理想情况下，
rwsem 获取/释放次数可降低到原调用次数的约 `1/8`。fork 同时缓存 4 个 mapping，可在
libc、动态加载器、可执行文件及其他常见映射交错出现时继续形成批次。

### 10.3 NUMA 访问优化

domain block 和 shard 锁在目标节点本地分配。完成转换后，普通 writer 的主要可写状态
包括 rwsem 和树根，这些状态位于 `mm` home NID 对应节点，预期降低：

- `i_mmap` 写锁 cacheline 的跨 socket HITM；
- writer 在共享锁上的排队时间；
- 大量进程并发 fork/exec/exit 时的锁唤醒和调度开销；
- 单棵大 interval tree 的修改深度。

### 10.4 可观测的性能结果

该设计预期直接改善以下指标：

- fork/exec/exit 或 mmap churn workload 的吞吐；
- `i_mmap_rwsem` 写锁等待时间和竞争采样占比；
- 热点 mapping 上单次 VMA link/unlink 的尾延迟；
- `perf c2c` 中中央锁 cacheline 的跨 NUMA bouncing。

收益不应只用 `i_mmap` 锁占比判断。解除该锁的串行后，吞吐可能继续受 page fault、页表
分配、`filemap_fault()`、folio lock 或 folio waitqueue 限制；这些热点的上升表示系统到达
下一层瓶颈，不一定表示本设计发生性能回归，也不属于本设计直接优化的范围。

本设计不承诺固定百分比收益。最终收益由热点 mapping 数、每节点并发 `mm` 数、fork/exit
比例、全局 rmap walker 频率以及后续瓶颈共同决定。

## 11. 正确性不变量

实现必须始终保持以下不变量：

1. 一个 VMA 的 insert、update 和 remove 使用同一个 root。
2. shard 发布前，全部 domain、root、锁和已存在 VMA 已完成初始化与迁移。
3. shard 指针只从 `NULL` 单向转换为有效对象，在 inode 销毁前不撤销。
4. 中央树和 shard 树不能同时承载已发布 mapping 的普通 VMA。
5. 聚合 VMA 计数在中央树迁移过程中保持不变。
6. trylock walker 在拿到完整锁集合之前不得执行 callback。
7. caller-locked walker 不得重复获取 mapping 锁。
8. `vma_prepare()` 到 `vma_complete()` 之间不得释放保护 remove/reinsert 的 shard 锁。
9. 全局加锁按 domain/shard 升序，解锁使用逆序。
10. 任意分配失败或安装竞争失败都必须完整回退中央路径。

## 12. 验证与验收

### 12.1 功能正确性

- 分别构建 `CONFIG_I_MMAP_SHARDS=n/y`；
- 启用 `CONFIG_PROVE_LOCKING`、KASAN 后执行 fork/exec/exit 压测；
- 并发执行 mmap/munmap/mprotect/mremap、truncate、reclaim 和 page migration；
- 覆盖 VMA split/merge 和 `vma_prepare()` remove/reinsert；
- 覆盖 rmap、memory-failure、khugepaged、pagewalk、uprobes 和架构 cache walker；
- 验证 shmem、DAX、hugetlbfs 始终停留在中央树；
- 注入分配失败，验证不发布部分 shard 且 mapping 仍可继续工作；
- 检查 lockdep、KASAN、kernel taint、panic、BUG 和 WARNING。

### 12.2 性能验收

至少采集以下指标：

- workload 吞吐和 P95/P99 延迟；
- 中央 rwsem 与每个 shard rwsem 的等待时间、持锁时间和分布；
- 每个 mapping 的 shard 激活次数、分配失败数和 VMA 分布；
- `perf c2c` HITM、NUMA 本地/远程访问；
- fork/exit 每次操作的锁获取次数；
- rmap/pagewalk 空查询、低命中和高命中延迟；
- page fault、filemap fault 和 folio waitqueue 指标，用于识别瓶颈转移。

目标机器测试应同时覆盖：

- worker 集中在一个 NUMA 节点，验证域内 4 shard 的扩展能力；
- worker 均匀分布在全部 NUMA 节点，验证锁本地性；
- 单实例和多实例，区分固定开销、真实并行收益和后续共享资源竞争。

验收标准是热点 `i_mmap` 写锁竞争显著下降、目标 workload 吞吐提高，并且未在全局
rmap/pagewalk、内存占用和正确性测试中产生不可接受的退化。
