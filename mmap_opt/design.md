# i_mmap_rwsem 锁竞争优化设计方案

## 1. 问题背景

在4 NUMA、320核服务器上运行 UnixBench execl 测试（动态链接二进制循环调用execve），
`i_mmap_rwsem` 锁竞争严重，成为性能瓶颈。

### 1.1 热点路径

execve 调用链中每个二进制/.so 产生约5个映射（text/data/bss等），
每个映射在 fork+exec+exit 周期中经历多次锁操作：

| 路径 | 锁操作 | 优化状态 |
|------|--------|---------|
| fork: dup_mmap | N次 per-VMA 锁获取 | ✅ 已批量（Phase 1） |
| exec: vma_link_file | ~15次（5映射×3文件） | ✅ 已缓存锁（Phase 2） |
| exec: vma_prepare (split) | 多树操作持锁 | ✅ 分片锁并行（Phase 3） |
| exit: free_pgtables | 已批量处理 | 3577dbb19241 |

### 1.2 火焰图数据（来自社区讨论）

```
24.71% vma_link_file       --> down_write(&mapping->i_mmap_rwsem)
24.82% free_pgtables batch --> unlink_file_vma_batch_process
18.50% __split_vma         --> vma_prepare
12.44% _dl_map_project     --> mprotect --> __split_vma
 6.15% exit_mmap batch     --> unlink_file_vma_batch_process
 6.13% _dl_main            --> mprotect --> __split_vma
```

## 2. 社区已有尝试及反馈

### 2.1 NUMA 级别 i_mmap 树拆分（Huang Shijie, 2026-04）
- 将 i_mmap 按 NUMA 节点拆分为兄弟树
- **反馈 (Mateusz Guzik)**: 单节点高核数系统无效，应做锁分片而非仅数据分片
- **其他问题**: 每个 inode 预分配 nr_node_ids 个树根、GFP_KERNEL 硬编码、foreach 宏类型混淆

### 2.2 跳过 .so 的 rmap 操作（Yibin Liu, 2026-04）
- 添加 RWH_RMAP_EXCLUDE 标志跳过 rmap 树操作
- **反馈**: 被社区坚决 NAK（破坏 reclaim/migration/truncation）

### 2.3 Mateusz Guzik 核心建议
- **批量处理**: exit 已批量，fork 应同样批量
- **锁分片**: 每子集独立锁 + 顶层遍历 rwsem ← Phase 3
- **动态升降级**: 先集中状态，trylock 检测争用，超阈值提升为分片 ← Phase 4（待实现）
- **非 NUMA 依赖**: 分片按每 8 CPU 一组，而非 NUMA 节点

## 3. 优化设计

### 3.1 第一阶段：fork 路径批量插入（Commit 1） ✅

**dup_mmap 批量 VMA 插入**

- 新增 `dup_mmap_file_batch` 结构，容量8对 (tmp, mpnt)
- 循环中按 `file->f_mapping` 分组收集 VMA
- mapping 变化或批次满时，一次 `i_mmap_lock_write` 批量插入
- 错误路径通过 `loop_out` 处 flush 保证一致性

**效果**: fork 路径 i_mmap 锁获取次数从 N 次降至 ~1次/文件

### 3.2 第二阶段：exec 路径批量插入（Commit 2） ✅

**vma_link_file 锁缓存机制**

- `mm_struct` 新增 `i_mmap_cached_mapping` + `i_mmap_batching`
- 仅 exec 期间 `i_mmap_batching=1`，其他路径不受影响
- 同文件连续 mmap 复用已持有锁，跨文件自动切换
- `mmap_region()` 非 batching 模式自动 flush

**ELF 加载器集成**: `load_elf_binary()` 段映射前启用，完成后 flush（含错误路径）

**效果**: exec 路径 i_mmap 锁获取从 ~15次（5段×3文件）降至 ~3次（1次/文件）

### 3.3 第三阶段：i_mmap 树锁分片（Commit 3） ✅

**惰性分配 + 4 分片锁**

```
struct i_mmap_shard {
    struct rb_root_cached tree;      // 24 bytes
    struct rw_semaphore rwsem;       // 40 bytes
};
// 4 shards × 64 bytes = 256 bytes per hot mapping
```

**关键设计决策（对比 Huang Shijie NUMA 方案）**:

| 问题 | Huang 方案 | 本方案 |
|------|-----------|--------|
| 分片依据 | NUMA 节点 | vm_pgoff 哈希（单节点也有效） |
| 内存模型 | 每个 inode 预分配 | 惰性分配：首次 mmap 时 `kcalloc` |
| 非 mmap inode | 有开销 | 零开销（i_mmap_shards=NULL） |
| foreach 宏 | (void*) 类型转换 | `vma_interval_tree_foreach_sharded` |
| 并发分配 | 无保护 | `cmpxchg` 原子发布，输者释放 |

**锁协议**:
```
非分片（i_mmap_shards==NULL）: 行为完全不变
分片模式:
  i_mmap_lock_write  → down_read(i_mmap_rwsem)  顶层共享
    i_mmap_shard_lock_write(shard)                分片独占 → 树操作
  i_mmap_lock_read   → down_write(i_mmap_rwsem)  顶层独占（遍历）
```

**20个遍历点**全部改用 `vma_interval_tree_foreach_sharded()` 透明迭代。

**效果**: 不同分片的并发 insert/remove 不再互斥，锁竞争降低 N 倍（N≈活跃分片数）

### 3.4 第四阶段：动态提升/降级分片（待实现）

Mateusz Guzik 建议的核心思路（`mmap_opt/advise.md`）：

> Start with the current centralized state and trylock on addition.
> If trylocks go past a threshold, convert it to the distributed state.
> Then future additions/removals are largely deserialized, while
> comparatively rarely used binaries don't use extra memory.

**设计要点**:
- 非分片映射用 `down_write_trylock` 尝试获取锁
- trylock 失败次数达到阈值 → 提升为分片模式
- 冷文件永久停留在集中模式（零额外内存）
- 可选降级：争用消失后释放分片，回到集中模式

**与当前 Phase 3 的差异**: Phase 3 首次 mmap 即分配分片（热/冷文件均分配），
Phase 4 仅热文件分配，冷文件零开销。但在 execl 场景下所有参与争用的文件
本身就会被 mmap，Phase 3 的额外内存已可忽略。

## 4. 文件修改清单

### Phase 1 (fork path batch)
| 文件 | 修改内容 |
|------|---------|
| mm/mmap.c | `dup_mmap_file_batch` 结构 + `dup_mmap` 批量插入逻辑 |

### Phase 2 (exec path cache)
| 文件 | 修改内容 |
|------|---------|
| include/linux/mm_types.h | mm_struct 新增 `i_mmap_cached_mapping` + `i_mmap_batching` |
| include/linux/mm.h | `vma_link_file_flush` 声明 |
| kernel/fork.c | mm_init 初始化新字段 |
| mm/vma.c | `vma_link_file` 锁缓存 + `vma_link_file_flush` + `mmap_region` flush |
| mm/vma.h | `vma_link_file_flush` 声明 |
| fs/binfmt_elf.c | ELF 加载器启用/关闭 batching |

### Phase 3 (lock sharding)
| 文件 | 修改内容 |
|------|---------|
| include/linux/fs.h | `struct i_mmap_shard`、分片辅助函数、锁宏条件编译、`vma_interval_tree_foreach_sharded` |
| include/linux/mm_types.h | `vm_area_struct` 新增 `i_mmap_shard_idx` |
| mm/Kconfig | `CONFIG_MM_IMMAP_SHARD` 选项 |
| mm/vma.c | `i_mmap_shard_init`（cmpxchg 原子发布）、`__vma_link_file`/`__remove_shared_vm_struct`/`vma_prepare`/`vma_complete` 分片感知 |
| mm/vma_init.c | `vm_area_init_from` 拷贝 `i_mmap_shard_idx` |
| mm/mmap.c | `dup_mmap_file_batch_flush` 分片感知 |
| 12 文件（arch/fs/mm） | 20 个遍历点改为 `vma_interval_tree_foreach_sharded` |
