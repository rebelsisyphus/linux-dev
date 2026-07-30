# `i_mmap` 优化补丁逐项实现说明

## 1. 文档范围

本文按 `mmap_opt_latest/patches` 中 18 个补丁的顺序，说明每个补丁的实现原理、代码细节、
并发语义以及它在整个系列中的作用。

补丁基线为 `3d5d2ff074a2`，系列最终提交为 `4207a728a280`。系列只优化文件 VMA 的
link/unlink、`i_mmap` interval tree 和相关 reader，不包含 page fault、filemap fault、
folio lock 或 folio waitqueue 修改。

## 2. 补丁系列组织原则

整个系列按以下顺序逐步建立能力：

```text
补丁 01～03：先减少 fork/exit 中 i_mmap 写锁获取次数
      │
补丁 04～07：准备分片数据结构、NUMA 分配、计数和发布机制
      │
补丁 08～09：建立统一 writer/reader 接口
      │
补丁 10～15：转换所有普通文件 i_mmap reader
      │
补丁 16～17：补齐跨操作持锁和 all-lock 契约
      │
补丁 18：确认读写路径完整后，最后连接动态激活入口
```

这种拆分使批处理优化能够独立使用，也确保 shard 真正启用之前，所有可能访问
`mapping->i_mmap` 的路径已经具备分片语义。

## 3. 补丁总览

| 序号 | 提交 | 主要作用 |
| ---: | --- | --- |
| 01 | `a28668deafa5` | 在 `dup_mmap()` 中批量插入同一 mapping 的文件 VMA |
| 02 | `81b1d11c3fae` | 退出批处理改用 `address_space` 作为聚合键 |
| 03 | `a146514f726d` | fork 同时维护 4 个 mapping batch |
| 04 | `07e9453316cc` | 增加 Kconfig、KABI 状态和分片拓扑 |
| 05 | `353f99f1a77b` | 按 NUMA 节点分配和释放 shard block |
| 06 | `3259a872c064` | 使用独立 VMA 计数支持无窗口 `mapping_mapped()` |
| 07 | `cd9937d3701d` | 实现稳定 shard 选择、迁移和 release/acquire 发布 |
| 08 | `60972142e2b5` | 将文件 VMA writer 路由到中央树或目标 shard |
| 09 | `2b11e066fd62` | 增加完整、trylock 和 caller-locked reader helper |
| 10 | `ff8c28eb45fd` | 转换核心 file rmap 和 unmap walker |
| 11 | `9520f2e6ed56` | 转换 memory-failure 文件映射收集路径 |
| 12 | `01a709215a13` | 转换 khugepaged 页表回收路径 |
| 13 | `c9ac5926f454` | 转换 `walk_page_mapping()` 及 dirty helper |
| 14 | `6c880d66929d` | 转换 uprobe 注册映射收集路径 |
| 15 | `13c6e328e0af` | 转换 IRQ-safe 架构 cache walker |
| 16 | `cc97ce4eedb9` | 补齐 `mm_take_all_locks()` 的全部 shard 写锁语义 |
| 17 | `bf83a857d998` | 为 locked file rmap 增加跨操作全 root 读锁上下文 |
| 18 | `4207a728a280` | 达到争用阈值后动态启用 NUMA 本地 shard |

## 4. 补丁 01：批量插入 fork 产生的文件 VMA

补丁文件：[0001-mm-batch-file-VMA-insertions-in-dup_mmap.patch](patches/0001-mm-batch-file-VMA-insertions-in-dup_mmap.patch)

### 4.1 优化原理

`dup_mmap()` 复制父进程 VMA 时，原路径每复制一个文件 VMA 就获取一次
`mapping->i_mmap_rwsem`，更新 writable 计数并插入 interval tree。动态链接进程通常有
多个连续 VMA 指向同一个 libc、动态加载器或可执行文件，因此一次 fork 会反复获取同一把
写锁。

补丁将同一 `address_space` 的连续插入延迟到一个 batch 中，在一次写锁临界区内最多
插入 8 个 VMA，将锁获取、释放和唤醒成本按批次摊薄。

### 4.2 数据结构

新增 `struct dup_mmap_file_batch`：

- `mapping`：当前批次对应的 `address_space`；
- `count`：已缓存的 VMA 数；
- `vmas[8]`：child mm 中待插入的 VMA；
- `prev[8]`：对应的 parent VMA，用作 `vma_interval_tree_insert_after()` 插入提示。

此时 `dup_mmap()` 只维护一个 batch。遇到不同 mapping、batch 达到 8 项或语义边界时
flush。

### 4.3 flush 实现

`dup_mmap_file_batch_flush()` 的操作顺序是：

1. 获取该 mapping 的 `i_mmap_rwsem` 写锁；
2. 获取 `flush_dcache_mmap_lock()`；
3. 逐个处理 VMA；
4. 对 `VM_SHARED` VMA 调用 `mapping_allow_writable()`；
5. 使用 `vma_interval_tree_insert_after()` 插入中央树；
6. 释放 dcache mapping lock 和 `i_mmap_rwsem`；
7. 清空 batch。

`get_file()` 仍在 VMA 复制时立即执行，延迟的只是 rmap interval tree link，不改变文件引用
生命周期。

### 4.4 page-table-copy 边界

新增 `dup_mmap_needs_page_copy()`，在以下 VMA 上要求先 flush 再执行
`copy_page_range()`：

- child VMA 启用了 userfaultfd write-protect；
- source VMA 设置 `VM_PFNMAP` 或 `VM_MIXEDMAP`；
- source VMA 已经存在 `anon_vma`。

这些条件表示 fork 可能实际复制或处理页表。先把 child VMA 加入文件 rmap，保持原路径中
“建立 rmap 后再复制页表”的顺序。`VM_WIPEONFORK` 不复制页表，因此可继续延迟。

本补丁的单 batch 最终在 `loop_out` flush；补丁 03 会进一步在
`arch_dup_mmap()` 之前显式 flush 所有 batch。

### 4.5 预期效果和成本

- 同一 mapping 连续 VMA 的写锁次数最多降低到约 `1/8`；
- 增加一个 144 字节左右的栈上 batch；
- interval tree 插入次数不变，只减少锁临界区进出次数；
- 不改变中央树布局，也不依赖后续 shard 功能。

## 5. 补丁 02：退出批处理按 `address_space` 聚合

补丁文件：[0002-mm-batch-file-VMA-unlink-by-address_space.patch](patches/0002-mm-batch-file-VMA-unlink-by-address_space.patch)

### 5.1 优化原理

`free_pgtables()` 已经能够批量 unlink 附近的文件 VMA，但原实现用 `struct file *` 判断
batch 是否连续。真正拥有 interval tree 和 `i_mmap_rwsem` 的对象是
`file->f_mapping`，多个不同 `struct file` 可以指向同一个 `address_space`。

补丁将聚合键改为 `address_space`，使不同 file object 但相同 mapping 的 VMA 可以共用
一次写锁临界区。

### 5.2 实现细节

- `struct unlink_vma_file_batch` 新增 `mapping` 字段；
- `unlink_file_vma_batch_init()` 同时清零 `mapping` 和 `count`；
- `unlink_file_vma_batch_add()` 从 VMA 取出 `vm_file->f_mapping`；
- 只有 mapping 改变或 8 项数组已满时才处理当前 batch；
- `unlink_file_vma_batch_process()` 直接使用 batch 保存的 mapping；
- 保留 `VM_WARN_ON_ONCE()`，检查每个 VMA 的 mapping 与 batch key 一致。

### 5.3 并发语义

补丁不改变原有加锁顺序和 VMA 删除顺序，只扩大了可以合法合批的范围。由于锁、writable
计数和 interval tree 都属于 `address_space`，使用 mapping 作为 key 与保护对象完全一致。

## 6. 补丁 03：fork 同时维护多个文件 batch

补丁文件：[0003-mm-keep-multiple-file-VMA-batches-in-dup_mmap.patch](patches/0003-mm-keep-multiple-file-VMA-batches-in-dup_mmap.patch)

### 6.1 优化原理

实际进程的 VMA 通常由多个文件交错组成。只有一个 batch 时，只要 mapping 发生切换就会
立即 flush，难以累积足够多的同 mapping VMA。

补丁在 `dup_mmap()` 栈上保留 4 个活跃 batch，每个仍最多容纳 8 个 VMA，使少量热点
mapping 交错出现时仍可分别累积。

### 6.2 槽位选择

`dup_mmap_file_batch_get()` 按以下顺序选择槽位：

1. 查找 `count != 0` 且 mapping 相同的已有槽位；
2. 查找第一个空槽位；
3. 四个槽位都被占用时，flush `batches[0]` 并复用它。

这里采用固定槽位而不是 LRU，不增加链表、时间戳或动态分配。热点 mapping 数不超过 4
时不会因 mapping 交错而提前 flush。

### 6.3 全量 flush 边界

新增 `dup_mmap_file_batch_flush_all()`，在以下位置 flush 全部 4 个 batch：

- 即将对需要 page-table copy 的 VMA 调用 `copy_page_range()`；
- 调用 `arch_dup_mmap()` 之前；
- 正常或错误路径进入 `loop_out` 时。

`arch_dup_mmap()` 前的 flush 确保架构代码看到完整的 child 文件 rmap。`loop_out` 再次
flush 是统一清理保障，空 batch 直接返回，不会重复插入。

### 6.4 成本

当前构建中单个 batch 为 144 字节，4 个 batch 固定占用 `dup_mmap()` 约 576 字节内核
栈。查找槽位最多扫描 4 项，属于常量开销，不进行动态内存分配。

## 7. 补丁 04：增加可选的 NUMA 本地分片状态

补丁文件：[0004-mm-add-optional-NUMA-local-i_mmap-shard-state.patch](patches/0004-mm-add-optional-NUMA-local-i_mmap-shard-state.patch)

### 7.1 配置入口

新增 `CONFIG_I_MMAP_SHARDS`：

```text
depends on MMU && SMP && NUMA && EXPERT
default n
```

配置默认关闭，并明确标记为实验功能。该补丁只增加状态和类型，不会发布 shard，因此单独
应用时运行行为不变。

### 7.2 分片拓扑

定义最多 8 个 NUMA domain，每个 domain 固定 4 个 shard，总上限 32：

- `i_mmap_shard`：一个 `rb_root_cached` 和一把 `rw_semaphore`；
- `i_mmap_domain_shards`：NUMA NID 和 4 个本域 shard；
- `i_mmap_shards`：domain 数量和最多 8 个 domain 指针。

`i_mmap_shard` 使用 `____cacheline_aligned_in_smp`，使相邻 shard 的锁状态和树根不共享
cacheline。

### 7.3 KABI 状态

`address_space` 使用现有 KABI reserve 保存：

- `i_mmap_shards`：发布后的顶层分片指针；
- `i_mmap_nr_vmas`：独立于树布局的 VMA 总数；
- `i_mmap_lock_contention`：中央写锁失败计数和安装认领状态。

`mm_struct` 使用 KABI reserve 保存 `i_mmap_home_nid`。不扩大
`address_space`、`mm_struct` 或 `vm_area_struct` 的 ABI 尺寸。

### 7.4 稳定 home NID

`mm_init()` 使用 `numa_node_id()` 记录创建 mm 时的节点。该值在 mm 生命周期内保持不变，
不会随线程或进程调度迁移改变。共享同一 mm 的线程自然共享相同 home NID。

稳定值是后续 insert/remove 计算相同 shard 的基础。

## 8. 补丁 05：在对应 NUMA 节点分配 shard

补丁文件：[0005-mm-allocate-i_mmap-shards-on-their-NUMA-nodes.patch](patches/0005-mm-allocate-i_mmap-shards-on-their-NUMA-nodes.patch)

### 8.1 优化原理

如果把全部 shard 放在一次普通 `kzalloc()` 中，整块内存通常位于触发分配的单个节点。
其他 NUMA 节点虽然使用不同锁，仍然需要远程写锁 cacheline。

补丁只用普通 `kzalloc()` 分配只读为主的顶层 descriptor；每个 domain block 则通过
`kzalloc_node(sizeof(*domain), gfp, nid)` 分配到对应节点。

### 8.2 分配过程

`i_mmap_shards_alloc()`：

1. 若 `num_possible_nodes() > 8`，直接返回 `NULL`；
2. 分配并清零顶层对象；
3. 遍历 `N_POSSIBLE` 节点；
4. 在每个 NID 上分配一个四 shard domain block；
5. 初始化 domain NID、4 个空 interval tree 和 4 把 rwsem；
6. 初始化完成后才把 domain 指针加入顶层数组并增加 `nr_domains`；
7. 任一分配失败，释放此前已经登记的全部 domain 和顶层对象。

分配接口接收 `gfp_t`，实际动态激活补丁会传入 `GFP_KERNEL`。

### 8.3 生命周期

`i_mmap_shards_free()` 允许传入 `NULL`，按已登记的 `nr_domains` 逐块释放，最后释放顶层
对象。`__destroy_inode()` 在 mapping 已失去 VMA 用户后释放已发布对象并清空指针。

发布后的 shard 不做运行期回收或反向合并，因此不需要为普通 reader 增加引用计数。

## 9. 补丁 06：建立独立于树布局的文件 VMA 计数

补丁文件：[0006-mm-account-file-VMAs-independently-of-i_mmap-layout.patch](patches/0006-mm-account-file-VMAs-independently-of-i_mmap-layout.patch)

### 9.1 解决的问题

中央树向 shard 迁移时，必然出现“中央树最后一个节点已经删除、shard 指针尚未发布”的
内部阶段。`mapping_mapped()` 是无锁查询，如果只检查中央树，会在该窗口错误返回未映射。

补丁从功能启用之初就维护逻辑 VMA 总数，使计数不依赖节点当前存放在哪棵树中。

### 9.2 计数更新点

- fork batch 将 child VMA 正式插入树后执行 `i_mmap_vma_count_add()`；
- 普通 `__vma_link_file()` 插入后加一；
- `__remove_shared_vm_struct()` 正式删除后减一；
- VMA 范围更新中的临时 remove/reinsert 不改变计数；
- 中央树向 shard 的布局迁移也不改变计数。

`CONFIG_I_MMAP_SHARDS=n` 时 add/sub helper 是空函数，不增加运行期开销。

### 9.3 `mapping_mapped()` 行为

配置开启时先读取原子计数，只要大于 0 就返回 true；否则继续执行原有中央树非空检查。
保留中央树检查作为兼容和保守 fallback。

该计数的目标是回答“可能仍被映射”，并在迁移期间避免假阴性；它不用于替代 interval tree
定位具体 VMA。

## 10. 补丁 07：实现 NUMA-aware shard 选择和原子发布

补丁文件：[0007-mm-add-NUMA-aware-i_mmap-shard-installation.patch](patches/0007-mm-add-NUMA-aware-i_mmap-shard-installation.patch)

### 10.1 shard 选择算法

`i_mmap_shard_for_vma()` 分两级选择：

1. 读取 `vma->vm_mm->i_mmap_home_nid`；
2. 在线性 domain 数组中查找相同 NID；
3. 如果 home NID 无效，用 `hash_ptr(vm_mm, 32) % nr_domains` 选择确定性 fallback domain；
4. 使用 `hash_ptr(vm_mm, ilog2(4))` 选择域内 0～3 号 shard。

同一 mm 的全部 VMA 使用同一 home NID 和 `vm_mm`，因此同一 mapping 中属于该 mm 的
VMA 会稳定落到一个 shard。

### 10.2 安装过程

`i_mmap_shards_install_locked()` 要求调用者持有中央 `i_mmap_rwsem` 写锁：

1. acquire-load 检查是否已有其他线程完成安装；
2. 反复取得中央树当前第一个 VMA；
3. 从中央树删除该 VMA；
4. 用统一选择函数计算目标 shard；
5. 插入目标 shard root；
6. 中央树清空后，以 `smp_store_release()` 发布顶层指针。

迁移复用原有 VMA interval-tree node，不复制 VMA，也不改变 VMA 总计数。

### 10.3 发布内存序

reader 使用 `i_mmap_shards_load()` 中的 `smp_load_acquire()`，与安装端 release-store
配对。只要 reader 看到非空指针，就必须同时看到全部 domain、rwsem、root 及已经迁移的
VMA。

若锁内发现指针已经发布，函数返回 false，调用者应释放自己预分配但未使用的对象。

此补丁仍未连接安装触发入口，运行时不会主动转换 mapping。

## 11. 补丁 08：将文件 VMA writer 路由到正确的 root

补丁文件：[0008-mm-route-file-VMA-updates-through-shard-aware-locks.patch](patches/0008-mm-route-file-VMA-updates-through-shard-aware-locks.patch)

### 11.1 写锁上下文

新增 `struct i_mmap_write_lock`：

- `root`：本次操作应修改的 interval-tree root；
- `shard`：非空表示持有该 shard 写锁，空表示持有中央写锁。

调用者不再自行假定 `&mapping->i_mmap`，而是始终使用上下文返回的 `root`。

### 11.2 获取锁时处理发布竞态

`i_mmap_lock_write_vma()` 的初始实现执行双重检查：

1. acquire-load shard 指针；
2. 已发布则直接选择目标 shard 并获取其写锁；
3. 未发布则获取中央写锁；
4. 在中央锁内再次 acquire-load；
5. 仍未发布则保持中央锁并返回中央 root；
6. 若安装线程已发布，释放中央锁并转而获取目标 shard。

中央写锁与安装过程互斥，锁内复查消除了 reader 在“第一次读取为空、随后发生转换”时继续
修改已废弃中央树的竞态。

### 11.3 转换的 writer 路径

以下路径改用统一写锁上下文：

- 普通 `vma_link_file()` / `__vma_link_file()`；
- 单 VMA `unlink_file_vma()`；
- exit/unmap unlink batch；
- `dup_mmap()` 的文件 VMA batch；
- VMA split、merge、边界调整所使用的 `vma_prepare()` / `vma_complete()`。

底层 link/unlink helper 增加 `root` 参数，计数和 writable accounting 逻辑保持原位。

### 11.4 fork batch 的插入方式

中央树中 parent VMA 和新 child VMA 位于同一棵树，可以继续用
`vma_interval_tree_insert_after(child, parent, root)`。

分片后 parent mm 和 child mm 可能被哈希到不同 shard，`prev` 不再保证属于 child 的
目标树，因此分片路径必须使用普通 `vma_interval_tree_insert()`，不能使用 parent node
作为插入提示。

一个 batch 内的 VMA 都属于同一个 child mm 和 mapping，所以它们会落入同一个 shard，
可以安全共用一次 shard 写锁。

### 11.5 VMA 调整原子性

`vma_prepare()` 把写锁上下文保存在 `struct vma_prepare` 中，并保持锁直到
`vma_complete()`：

- prepare 阶段从目标 root 临时删除待调整 VMA；
- 修改 VMA 范围；
- complete 阶段重新插入同一 root；
- 完成所有 remove/insert 后才释放写锁。

reader 因此不会观察到临时 remove/reinsert 的中间状态。配置关闭时 inline helper 退化为
原中央写锁行为。

## 12. 补丁 09：增加完整的全 shard reader 接口

补丁文件：[0009-mm-add-all-shard-i_mmap-read-iteration.patch](patches/0009-mm-add-all-shard-i_mmap-read-iteration.patch)

### 12.1 通用 callback 模型

定义 `i_mmap_walk_fn(vma, arg)`，把单棵 root 的 interval-tree 遍历封装为
`i_mmap_walk_root()`。callback 返回非零时停止后续 root 和 VMA 遍历，并把返回值传给
调用者。

### 12.2 固定 shard 顺序

`i_mmap_shard_at()` 将线性 index 映射成：

```text
domain_idx = index / 4
shard_idx  = index % 4
```

所有全局加锁按 domain、shard 升序，释放按逆序。每个 shard rwsem 在初始化时分配独立
`lock_class_key`，让 lockdep 能识别固定嵌套次序。

### 12.3 阻塞与 trylock reader

`i_mmap_lock_shards_read()` 支持两种模式：

- blocking：依次 `down_read()` 全部 shard；
- trylock：依次 `down_read_trylock()`，任一失败立即逆序释放已取得的锁并返回
  `-EAGAIN`。

只有全部锁成功后才执行第一个 callback。这一点避免 trylock caller 在前几个 shard 已
执行有副作用 callback、后面获取失败后重试，从而重复处理同一 VMA。

### 12.4 `i_mmap_read_walk()`

统一 reader 流程为：

1. acquire-load shard 指针；
2. 指针为空则获取中央读锁；
3. 中央锁内再次检查指针；
4. 仍为空，在中央 root 上遍历并释放中央锁；
5. 已发布则释放中央锁，获取全部 shard 读锁后遍历所有 root。

trylock 模式下，中央锁或任一 shard 获取失败都返回 `-EAGAIN`。

### 12.5 caller-locked 接口

`i_mmap_walk_locked()` 不获取锁：

- mapping 已分片时，lockdep 断言调用者持有全部 shard rwsem，然后遍历所有 root；
- 未分片时，断言调用者持有中央 `i_mmap_rwsem`。

该接口用于调用者必须跨多个操作保持 rmap 锁的场景，避免递归获取 rwsem。

## 13. 补丁 10：转换核心 file rmap 和 unmap walker

补丁文件：[0010-mm-make-core-file-rmap-walkers-shard-aware.patch](patches/0010-mm-make-core-file-rmap-walkers-shard-aware.patch)

### 13.1 `unmap_mapping_folio()` 和 `unmap_mapping_pages()`

原 `unmap_mapping_range_tree()` 直接遍历中央 root。补丁将每个 VMA 的处理拆成
`unmap_mapping_range_vma_walk()` callback，并通过 context 传入：

- `first_index` / `last_index`；
- `zap_details`。

callback 计算 VMA 文件 offset 范围与目标范围的交集，再换算成虚拟地址区间并调用
`unmap_mapping_range_vma()`。两个外部入口都改用阻塞式 `i_mmap_read_walk()`。

原中央树 empty fast-path 被移除；空 mapping 由通用 walker 自然完成零次 callback。

### 13.2 `rmap_walk_file()`

每个 VMA 的逻辑迁入 `rmap_walk_file_vma()`：

1. 用 `vma_address()` 计算 folio 在该 VMA 中的虚拟地址；
2. 保留 `cond_resched()`；
3. 执行可选 `invalid_vma()` 过滤；
4. 调用 `rmap_one()`；
5. 检查可选 `done()`；
6. 需要提前停止时返回 1。

普通调用使用 `i_mmap_read_walk()`；`locked=true` 使用
`i_mmap_walk_locked()`，不重复获取调用者已持有的锁。

### 13.3 trylock 传播

当 `rwc->try_lock` 为 true 时，通用 helper 必须一次性 trylock 全部 shard。返回
`-EAGAIN` 后设置 `rwc->contended = true`，并且没有任何 callback 已经执行。

这保持 rmap retry 的全有或全无语义，也防止重试时重复 unmap、引用计数或迁移操作。

## 14. 补丁 11：转换 memory-failure 文件映射收集

补丁文件：[0011-mm-memory-failure-walk-sharded-file-mappings.patch](patches/0011-mm-memory-failure-walk-sharded-file-mappings.patch)

### 14.1 解决的问题

文件页硬件错误需要找到所有覆盖该文件 offset 的进程并加入 early-kill 列表。只遍历中央
树会漏掉已经迁入 shard 的 VMA，使受影响进程收不到通知。

### 14.2 callback 重构

新增 `collect_procs_file_walk` 保存 page、待 kill 链表和 `force_early`。通用 walker 先按
损坏 page 的 `pgoff` 找到每个覆盖 VMA，callback 再：

1. 进入 RCU read-side critical section；
2. 遍历所有进程；
3. 用 `task_early_kill()` 找到目标 task；
4. 比较 `vma->vm_mm` 与 task mm；
5. 调用 `add_to_kill_anon_file()`。

### 14.3 RCU 与睡眠锁

原代码在持有中央 mapping 读锁期间进入一次 RCU。通用 walker 可能在执行 callback 前
睡眠等待多个 shard rwsem，因此不能在调用 `i_mmap_read_walk()` 外层持有 RCU。

补丁把 RCU 临界区移动到 callback 内部：mapping 锁已经获取完成后才进入 RCU，避免 RCU
读侧区域跨越可能睡眠的锁获取过程。

算法量级仍然是 VMA 数与进程数的乘积，补丁目标是覆盖完整性和锁语义，不是降低该路径
复杂度。

## 15. 补丁 12：转换 khugepaged 页表回收 walker

补丁文件：[0012-mm-khugepaged-find-retractable-mappings-in-all-shard.patch](patches/0012-mm-khugepaged-find-retractable-mappings-in-all-shard.patch)

### 15.1 解决的问题

文件页折叠后，`retract_page_tables()` 需要找到所有映射同一文件 offset 的 VMA，回收可以
被 PMD huge mapping 替代的 PTE page table。漏掉 shard 会留下旧 PTE table。

### 15.2 VMA callback 处理

`retract_page_tables_vma()` 对每个命中 VMA 保留原检查顺序：

- 跳过已经有 `anon_vma` 的私有写映射；
- 检查计算出的虚拟地址和 VMA 长度是否覆盖一个完整、对齐的 PMD；
- 用 `find_pmd_or_thp_or_none()` 确认存在可回收 PTE page table；
- 检查 mm 是否正在退出；
- 跳过 userfaultfd write-protected VMA；
- 建立并发送 MMU notifier invalidate range；
- 获取 PMD/PTE lock；
- flush PMD、更新 PTE 计数并延迟释放页表页。

### 15.3 二次竞态检查

该路径没有持有目标 mm 的 `mmap_lock`。取得 page-table lock 后，仍需再次检查
`vma->anon_vma` 和 `userfaultfd_wp(vma)`，防止并发私有写或 userfaultfd 注册在第一次检查
后改变状态。

若二次检查失败，只结束 notifier 并释放锁，不减少 PTE 计数或释放页表。

`retract_page_tables()` 最终使用目标 `pgoff` 调用阻塞式 `i_mmap_read_walk()`，在全部 shard
中执行相同 callback。

## 16. 补丁 13：转换 `walk_page_mapping()`

补丁文件：[0013-mm-pagewalk-traverse-every-i_mmap-shard.patch](patches/0013-mm-pagewalk-traverse-every-i_mmap-shard.patch)

### 16.1 接口锁语义变化

原接口要求 caller 持有 `mapping->i_mmap_rwsem`。补丁后
`walk_page_mapping()` 自己调用 `i_mmap_read_walk()` 获取中央或全部 shard 读锁。

`wp_shared_mapping_range()` 和 `clean_record_shared_mapping_range()` 删除外层中央读锁，避免
分片时只锁中央树，也避免新接口内部再次获取同一锁。

### 16.2 VMA 范围裁剪

`walk_page_mapping_vma()` 对每个命中 VMA：

1. 计算 VMA 文件 offset 区间 `[vba, vea)`；
2. 与请求区间 `[first_index, first_index + nr)` 取交集；
3. 将交集换算为 VMA 内的虚拟地址 `[start_addr, end_addr)`；
4. 空区间直接跳过；
5. 更新 `mm_walk.vma` 和 `mm_walk.mm`；
6. 执行 `walk_page_test()` 和 `__walk_page_range()`。

### 16.3 返回值保持

通用 i_mmap callback 的非零返回值只表示“停止遍历”。真正需要返回给
`walk_page_mapping()` caller 的结果保存在 `walk_page_mapping_args::err` 中：

- `walk_page_test()` 返回正值表示正常提前停止，转换为最终返回 0；
- 返回负值时保存错误并停止；
- `__walk_page_range()` 的非零结果同样保存后停止；
- 公共函数最终返回保存的 `walk.err`。

这样既能控制跨 shard 遍历停止，又保持原 pagewalk API 的返回约定。

## 17. 补丁 14：转换 uprobe registration map 构建

补丁文件：[0014-uprobes-build-registration-maps-from-all-i_mmap-shar.patch](patches/0014-uprobes-build-registration-maps-from-all-i_mmap-shar.patch)

### 17.1 解决的问题

uprobe 注册或注销需要枚举覆盖 probe offset 的全部 VMA，为每个 mm 建立 `map_info`。
中央树遗漏会导致部分进程没有安装或移除 probe。

### 17.2 callback context

`build_map_info_walk` 保存：

- probe 文件 offset；
- 当前是 register 还是 unregister；
- `curr` 结果链表指针；
- `prev` 可复用空闲节点链表指针；
- `more` 缺少节点数量。

callback 先执行 `valid_vma()`。有可用 `map_info` 后，通过
`mmget_not_zero(vma->vm_mm)` 固定 mm 生命周期，填写 mm 和
`offset_to_vaddr()` 计算出的虚拟地址，再加入结果链表。

### 17.3 不在 mapping 锁内阻塞分配

在持有 i_mmap 读锁的 callback 中，只允许使用：

```text
GFP_NOWAIT | __GFP_NOMEMALLOC | __GFP_NOWARN
```

这样不会进入 reclaim 并递归获取 mapping 锁。若 NOWAIT 分配失败，callback 只增加
`more`。

walker 返回并释放所有 mapping 锁后，`build_map_info()` 使用 `GFP_KERNEL` 分配缺少的
节点，释放本轮取得的 mm 引用，然后重新遍历。原有“两阶段预分配后重试”模型得到保留，
只是 VMA 来源改成全部 shard。

## 18. 补丁 15：转换架构 aliasing-cache walker

补丁文件：[0015-mm-make-architecture-i_mmap-cache-walkers-shard-awar.patch](patches/0015-mm-make-architecture-i_mmap-cache-walkers-shard-awar.patch)

### 18.1 特殊约束

ARM、Nios II 和 PA-RISC 的部分 cache-maintenance 路径持有
`flush_dcache_mmap_lock()`，其中 Nios II 和 PA-RISC 还可能使用 irqsave 形式。这些临界区
不能获取可能睡眠的 shard rwsem。

因此不能直接使用阻塞式 `i_mmap_read_walk()`，也不能继续只访问中央 root。

### 18.2 dcache-locked walker

新增 `i_mmap_walk_dcache_locked()`：

- caller 已持有架构 dcache mapping lock；
- shard 已发布时直接遍历全部 shard root；
- 未发布时遍历中央 root；
- helper 本身不获取 rwsem，因此 IRQ-safe 临界区内不会睡眠。

该模型成立的原因是所有普通 VMA tree insert/remove 原本就在
`flush_dcache_mmap_lock()` 保护范围内。

### 18.3 安装过程与 dcache walker 互斥

补丁给 `i_mmap_shards_install_locked()` 的完整迁移和 release publication 外围增加
`flush_dcache_mmap_lock()`。

因此 dcache-locked reader 只会观察到两种完整状态之一：

- 安装前：指针为空，全部 VMA 位于中央树；
- 安装后：指针非空，全部 VMA 位于 shard root。

它不会在无需 rwsem 的遍历中碰到一半位于中央树、一半位于 shard 的迁移状态。

### 18.4 架构路径转换

- ARM `make_coherent()`：callback 只处理同一 mm、不是当前 VMA 且带
  `VM_MAYSHARE` 的别名映射；累计 alias 后决定是否调整当前 PTE。
- ARM `__flush_dcache_aliases()`：按 folio offset 范围遍历同一 active mm 的共享映射，
  保留 folio/VMA 边界裁剪和 `flush_cache_pages()`。
- Nios II `flush_aliases()`：在 irqsave dcache lock 内遍历同一 mm 的共享别名并执行
  `flush_cache_range()`。
- PA-RISC `flush_dcache_folio()`：callback 保留颜色比较、共享/私有映射处理、逐页 flush、
  inequivalent alias 报错和 4096 次告警计数。

该补丁解决的是架构正确性，不是普通 rmap 性能路径。

## 19. 补丁 16：补齐 `mm_take_all_locks()` 语义

补丁文件：[0016-mm-preserve-all-lock-semantics-for-sharded-i_mmap.patch](patches/0016-mm-preserve-all-lock-semantics-for-sharded-i_mmap.patch)

### 19.1 解决的问题

`mm_take_all_locks()` 的契约是阻止目标 mm 相关的全部 VMA、PTE 和 rmap 修改。分片后，
普通 writer 不再获取中央 `i_mmap_rwsem`，只锁中央树无法满足该契约。

### 19.2 获取顺序

`vm_lock_mapping()` 在 `AS_MM_ALL_LOCKS` 去重后：

1. 获取中央 `i_mmap_rwsem` 写锁；
2. acquire-load shard 指针；
3. 若已分片，按 domain/shard 升序获取全部 shard 写锁；
4. 每次使用 `down_write_nest_lock(..., &mm->mmap_lock)` 保留原 lockdep 嵌套关系。

中央锁在最前面充当 publication gate。持有它之后不会再发生新安装，因此读取到的 shard
集合在整个 all-lock 临界区保持稳定。

### 19.3 释放顺序

`vm_unlock_mapping()` 增加 `mm` 参数并断言持有 mm 写锁。若已分片，先逆序释放全部 shard
写锁，再释放中央写锁，最后清除 `AS_MM_ALL_LOCKS`。

`mm_all_locks_mutex` 和 `AS_MM_ALL_LOCKS` 继续避免同一 mapping 因一个 mm 中存在多个 VMA
而被重复加锁或解锁。

完成该补丁后，持有 all-lock 集合的 caller 可以合法使用
`i_mmap_walk_locked()`，无需递归获取 shard 锁。

## 20. 补丁 17：为 locked file rmap 增加跨操作全 root 读锁

补丁文件：[0017-mm-hold-all-i_mmap-shards-for-locked-file-rmap.patch](patches/0017-mm-hold-all-i_mmap-shards-for-locked-file-rmap.patch)

### 20.1 解决的问题

文件大 folio split 会在调用 `try_to_unmap()` 时传入 `TTU_RMAP_LOCKED`。这表示 caller 必须
在整个 split/unmap 操作期间保持文件 rmap 锁，而不是只在某次 walker callback 中临时
加锁。

分片后只持有中央读锁不能排斥 shard writer，因此需要显式的全 root 读锁上下文。

### 20.2 读锁上下文

新增 `struct i_mmap_read_lock`：

- `central=true` 表示当前保持中央读锁；
- `shards` 非空表示当前保持该集合的全部 shard 读锁。

`i_mmap_lock_read_all()`：

1. acquire-load shard 指针；
2. 为空则获取中央读锁并在锁内复查；
3. 仍为空时保持中央锁返回，安装线程被中央读锁阻止；
4. 若已发布，释放中央锁并按固定顺序获取全部 shard 读锁；
5. 入口已经看到 shard 时直接获取全部 shard 读锁。

`i_mmap_unlock_read_all()` 根据 context 选择逆序释放全部 shard 或释放中央锁，并用
`VM_WARN_ON_ONCE()` 检查无效状态。

### 20.3 huge folio split 接入

`split_huge_page_to_list_to_order()` 的文件 folio 分支从中央
`i_mmap_lock_read()` 改为 `i_mmap_lock_read_all()`，并在操作完成后对应释放。

在这段持锁区间内，`rmap_walk_locked()` 可以调用补丁 09 的 caller-locked walker，且所有
shard writer 都被排斥。匿名 folio 仍使用原 anon_vma 锁路径。

## 21. 补丁 18：按写锁争用动态启用 shard

补丁文件：[0018-mm-enable-NUMA-local-i_mmap-shards-after-contention.patch](patches/0018-mm-enable-NUMA-local-i_mmap-shards-after-contention.patch)

### 21.1 系列中的位置

这是第一个真正从运行时写路径触发 shard 安装的补丁。它被放在系列最后，确保 activation
发生前，writer、普通 reader、trylock reader、caller-locked reader、架构 dcache walker
和 all-lock 路径均已转换完成。

### 21.2 适用范围检查

`i_mmap_sharding_eligible()` 只允许：

- `mapping->host` 存在且为普通规则文件；
- VMA 的 `vm_file->f_mapping` 正是目标 mapping；
- 非 shmem；
- 非 DAX；
- 非 hugetlbfs；
- possible NUMA 节点数不超过 8。

NOMMU 已由 Kconfig 依赖排除。不合格 mapping 永远保持中央树。

### 21.3 争用计数与唯一认领

阈值定义为 4。只有中央写锁 trylock 失败才调用
`i_mmap_claim_shard_install()`：

```text
初始状态：0
前三次失败：0 -> 1 -> 2 -> 3
第四次失败：3 -> -1
```

更新使用 `atomic_cmpxchg()` 循环。负值表示已经有线程取得安装资格；其他 writer 看到
负值后不会重复分配，而是走正常阻塞加锁和复查路径。

### 21.4 安装过程

唯一认领者调用 `i_mmap_install_shards()`：

1. 不持有中央写锁，以 `GFP_KERNEL` 分配全部 NUMA domain；
2. 分配失败时把争用状态恢复为 0，中央树保持原状；
3. 分配成功后获取中央写锁；
4. 调用补丁 07 的 install helper 再次检查是否已有 shard；
5. 成功则迁移并发布，随后将本地变量置空，避免释放已发布对象；
6. 安装竞争失败则释放自己未发布的对象；
7. 释放中央写锁后，writer 回到 retry 标签重新选择中央树或 shard。

分配放在中央锁外，避免慢速内存分配长时间阻塞该 mapping 的其他 writer。分配期间其他
线程仍可修改中央树；安装者取得中央写锁后迁移的是当时完整、最新的中央树。

### 21.5 最终 writer 快路径

最终 `i_mmap_lock_write_vma()` 的路径为：

1. acquire-load 已发布 shard；
2. 已发布则直接选择并锁定一个 shard；
3. 未发布则先 trylock 中央写锁；
4. trylock 成功后在锁内复查，未发布即保持中央锁返回；
5. trylock 失败且 mapping 合格时累计争用；
6. 达到阈值的唯一线程执行安装并 retry；
7. 其他线程阻塞获取中央写锁，锁内复查后选择中央或 shard。

已分片 mapping 不再访问争用计数，也不再获取中央写锁完成普通单 VMA 更新。

### 21.6 预期效果和边界

- 冷 mapping 不分配 shard，仍只有中央树；
- 4-NUMA 机器上的热点 mapping 建立 16 个 shard；
- 8-NUMA 机器上的热点 mapping 建立 32 个 shard；
- 普通 writer 只修改 home NUMA domain 内的一个 shard；
- 全局 reader 仍需获取并遍历全部 shard；
- `i_mmap_nr_vmas` 仍是跨 shard 共享原子变量；
- shard 一旦发布，直到 inode 销毁都不会收缩或撤销；
- 该补丁不会减少每个新 mm 的 demand page fault，也不会直接缓解 folio waitqueue 竞争。

## 22. 最终系列的关键不变量

18 个补丁合并后必须持续满足：

1. 同一 VMA 的 link、临时调整和 unlink 始终选择同一 root。
2. shard 指针发布前，全部 root、锁和既有 VMA 已经初始化或迁移完成。
3. writer 在未看到 shard 时，必须在中央锁内复查发布状态。
4. trylock reader 在成功获得完整锁集合前不执行 callback。
5. `rmap_walk_locked()` 不递归获取调用者已经持有的锁。
6. dcache-locked 架构 walker 不获取 sleeping rwsem，但与所有树修改和迁移互斥。
7. `mm_take_all_locks()` 和 `TTU_RMAP_LOCKED` caller 覆盖全部 shard。
8. 聚合 VMA 计数不因中央树到 shard 的布局迁移而变化。
9. 任一分配失败都保持完整中央树，不发布半初始化对象。
10. 动态激活只在最后一个补丁连接，系列中间提交不会提前产生分片 mapping。
