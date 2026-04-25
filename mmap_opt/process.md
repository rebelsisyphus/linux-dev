# i_mmap_rwsem 优化开发进展

## 已完成

### Phase 1：fork 路径批量插入 ✅

**Commit: mm: batch vma insertions in dup_mmap**

- [x] 新增 `dup_mmap_file_batch` 批量结构（容量8）
- [x] 实现 `dup_mmap_file_batch_flush()` 批量插入函数
- [x] 修改 `dup_mmap()` 循环按 mapping 分组收集 VMA
- [x] 正确路径 `loop_out` 处 flush 保证错误处理一致性
- [x] 编译通过

### Phase 2：exec 路径锁缓存 ✅

**Commit: mm: defer i_mmap insertion to batch across consecutive mmap calls**

- [x] `mm_struct` 新增 `i_mmap_cached_mapping` + `i_mmap_batching`
- [x] `vma_link_file()` 支持三种模式（hold_rmap_lock / batching / 默认）
- [x] `vma_link_file_flush()` 释放缓存锁
- [x] `mmap_region()` 非 batching 模式自动 flush
- [x] `load_elf_binary()` 启用/关闭 batching + 错误路径 flush
- [x] 编译通过

### Phase 3：惰性分配 i_mmap 树锁分片 ✅

**Commit: mm: shard i_mmap interval tree with per-shard locks**

- [x] `struct i_mmap_shard` 定义（64 bytes/分片）
- [x] `address_space` 新增 `i_mmap_shards` 指针（NULL=未分片，零开销）
- [x] `vm_area_struct` 新增 `i_mmap_shard_idx`
- [x] `CONFIG_MM_IMMAP_SHARD` Kconfig 选项（默认 n）
- [x] 锁协议：顶层 read + 分片 write（ops）/ 顶层 write（遍历）
- [x] `vma_interval_tree_foreach_sharded()` 透明迭代 1~4 树
- [x] `__vma_link_file` / `__remove_shared_vm_struct` / `vma_prepare` / `vma_complete` 分片感知
- [x] `dup_mmap_file_batch_flush` 分片感知
- [x] 20个遍历点全部改为新宏（12 文件）
- [x] `i_mmap_shard_init` 用 `cmpxchg` 防止并发重复分配
- [x] `i_mmap_is_sharded` 用 `READ_ONCE` 保证读序
- [x] CONFIG_MM_IMMAP_SHARD=n 编译通过
- [x] CONFIG_MM_IMMAP_SHARD=y 编译通过

## 待实施

### Phase 4：动态 trylock 提升分片（设计完成）

Mateusz Guzik 建议（`mmap_opt/advise.md`）：集中状态起步，`down_write_trylock`
检测争用，失败超阈值则提升为分片。冷文件零额外内存。

- [ ] `i_mmap_lock_write` 改为 trylock 路径（仅非分片模式）
- [ ] 新增 per-mapping 争用计数器（或复用 i_mmap_shards 低 bit）
- [ ] 达到阈值后提升：摘除集中树全体 VMA，分发到分片树
- [ ] `synchronize_rcu` 确保遍历者完成
- [ ] 可选降级：争用消失后释放分片

### 性能验证

- [ ] 在 4 NUMA ≥320 核机器上运行 UnixBench execl 测试
- [ ] 对比优化前后 execs/s（Phase 1 + 2 + 3）
- [ ] 对比 `CONFIG_MM_IMMAP_SHARD=n` vs `=y` 的 perf lock contention 数据
- [ ] 单节点高核数场景回归测试

## 风险与注意事项

| 风险 | 状态 |
|------|------|
| dup_mmap 批量 flush 错误路径 | ✅ 多 goto 路径均经 loop_out 统一处理 |
| vma_link_file batching 不影响非 exec 路径 | ✅ `mm->i_mmap_batching` 仅 exec 期间为1 |
| shard 并发分配泄漏 | ✅ cmpxchg 解决 |
| 锁宏语义翻转（分片时 write→read, read→write） | ✅ 非分片时行为完全不变，分片时所有路径经审查 |
| mm_struct 字段布局 | ✅ 无关字段，不影响 mmap_lock 缓存行 |
