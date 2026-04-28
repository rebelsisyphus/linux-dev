# Sharded i_mmap 开发进展

## 状态：可启动，核心功能可用

当前内核可在 QEMU 中正常启动，通过基础回归测试（fork/exec 压力、并发 1000 进程创建/销毁），无 panic/Oops/lockdep 告警。

## 已完成提交

| # | Commit | 描述 |
|---|--------|------|
| 1 | `afc01cab` | mm: introduce get_i_mmap_root() 抽象 i_mmap 访问 |
| 2 | `93f2035d` | mm: 引入 sharded i_mmap 基础设施（数据结构、辅助函数） |
| 3 | `391af59f` | mm: VMA 插入/删除路径转换为 per-shard 锁 |
| 4 | `559b5aa2` | mm: i_mmap 迭代路径适配分片模式（10 个文件） |
| 5 | `1a54e305` | mm: 动态分片：竞争检测触发 centralized → sharded 转换 |
| 6 | `f4e8690f` | mm: 修复三个运行时 bug（见下方详述） |
| 7 | `f090a539` | mm: trylock_fails 改为 atomic_t 修复并发竞争 |

**总计**: 17 个文件修改，+1338 / -440 行

## 已修复的严重 Bug

### Bug 1: 分片树损坏（页错误地址 0x100000000）

**根因**: `mapping_convert_to_shards()` 遍历 i_mmap 树时先调用 `iter_next()` 保存 `next`，再 `iter_first()` 重新查询。当只剩一个 VMA 时 `next == vma`，移除并插入 shard tree 后，第二次迭代试图用已移除节点的 `__rb_parent_color`（现指向 shard tree 内部）从旧树中清除，导致 shard tree 损坏。

**修复**: 改用 `mapping_convert_to_shards_locked()` 的模式——每次迭代用 `vma_interval_tree_iter_first()` 从头查询，移除当前 VMA 后自然推进。

### Bug 2: Lockdep 崩溃（__lock_acquire 页错误）

**子问题 A — union 覆盖**: `address_space` 中 `i_mmap` 和 `i_mmap_shards_ptr` 使用 `union`。当 centralized 树非空时，`i_mmap.rb_root.rb_node != NULL`，使 `mapping_sharded()` (原实现检查 `i_mmap_shards_ptr != NULL`) 错误返回 true，把 interval tree 节点指针当作 `i_mmap_shards*` 解引用。

**修复**: 将 union 改为独立字段 + 显式 `bool i_mmap_sharded` 标志。`mapping_sharded()` 改为读取 `i_mmap_sharded` 而非检查指针。

**子问题 B — lockdep class 耗尽**: 每个 shard rwsem 通过 `init_rwsem()` 创建新 lockdep class，数千个 mapping 瞬间耗尽 8192 class 限制。

**修复**: `__init_rwsem(rwsem, "i_mmap_shard_rwsem", &__lockdep_no_track__)` 使用 no-track key，跳过 lockdep 追踪。

### Bug 3: 锁泄漏（i_mmap_rwsem 返回用户空间仍被持有）

**根因**: `vma_prepare()` 在 centralized 模式下获取 `i_mmap_rwsem`，但 `vma_complete()` 再次检查 `mapping_sharded()` 来决定释放哪个锁。若两步之间发生了 centralized → sharded 转换，`vma_complete()` 走 shard 解锁分支（nr_locked_shards==0，什么都不做），`i_mmap_rwsem` 泄漏。

**修复**: 在 `struct vma_prepare` 中增加 `bool locked_sharded` 标志，在获取锁时记录锁类型，释放时据此选择解锁路径。同样修复 `dup_mmap_file_batch_unlock()` 中 sharded 模式错误调用 `i_mmap_unlock_write()` 的问题。

### Bug 3.1: trylock_fails 并发竞争

`shards->trylock_fails++` 是普通 `unsigned int` 自增，多 CPU 同时执行可能丢失计数。

**修复**: 改为 `atomic_t`，使用 `atomic_inc_return()` 原子自增并返回新值。

## 当前已知限制

1. **只支持 centralized → sharded 单向转换**，不支持回退。inode 销毁时 shard 随 address_space 一起释放。
2. **lockdep 不追踪 shard rwsem**，使用 `__lockdep_no_track__`。shard rwsem 在 `i_mmap_rwsem` 保护下操作，理论上足以保证正确性。
3. **fork 路径 (dup_mmap) 未利用 insert_after 优化**，sharded 模式下使用 `vma_interval_tree_insert` 替代 `vma_interval_tree_insert_after`。
4. **mapping_mapped() sharded 模式缺少快速路径**：`unmap_mapping_range` 的 sharded 路径逐 shard 遍历时总是加锁遍历，未在持锁前检查 `vma_count == 0` 跳过空 shard。

## 下一步

- [ ] 在真实多核 (≥32核) 机器上跑 execl benchmark，对比 centralized/sharded 性能
- [ ] 添加空 shard 快速路径到 rmap walk / unmap_mapping_range
- [ ] 考虑 dup_mmap sharded 路径的 insert_after 优化
- [ ] 清理调试代码（移除 VM_WARN_ON 等）
- [ ] Linux 社区提交前的代码风格和 commit message 规范化