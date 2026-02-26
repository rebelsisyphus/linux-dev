# mm percpu cache 开发进展

## 当前状态：v2补丁已生成，编译和QEMU引导测试通过

最后一次代码变更：2026-04-16

## 补丁文件（v2）

```
mm_pcpu_dev/
├── v2-0001-lib-percpu_counter-add-init_many_pcpu-and-detach_.patch  (124行新增)
├── v2-0002-mm-add-mm_stat_ext-struct-for-caching-percpu-data.patch  (13行新增，跨2文件)
└── v2-0003-fork-implement-per-cpu-cache-for-mm-percpu-data.patch     (194行新增, 7行删除)
```

已验证：全量内核编译通过，QEMU引导测试通过（无panic）。

## v1 → v2 修改内容

### 修复

1. **编译错误**：`struct mm_stat_ext`定义移至`struct mm_struct`之前（v1中定义在
   `mm_struct`之后导致"incomplete type"错误）
2. **!SMP编译错误**：在`percpu_counter.h`的`!CONFIG_SMP`段新增
   `percpu_counter_init_many_cached`宏（v1中缺失，导致!SMP编译失败）
3. **checkpatch修复**：
   - 将`if (false || #ifdef...)`重构为`mm_stat_ext_cached()`辅助函数
   - 添加声明后空行（checkpatch WARNING）

### 新增功能

4. **缓存统计**：在`CONFIG_SMP`下新增debugfs接口
   `/sys/kernel/debug/mm_pcpu_cache/`，提供四个计数器：
   - `get_hits`: 缓存命中次数
   - `get_misses`: 缓存未命中次数
   - `put_stores`: 成功存入缓存次数
   - `put_overflows`: 缓存满溢出(free_percpu)次数
   - !SMP下为空内联函数，零开销

## 变更文件清单

| 文件 | 补丁 | 变更类型 |
|------|------|---------|
| include/linux/percpu_counter.h | 1/3 | 新增`percpu_counter_init_many_pcpu`声明和`percpu_counter_init_many_cached`宏，新增`percpu_counter_detach_many`声明，!SMP内联stub |
| lib/percpu_counter.c | 1/3 | 实现`percpu_counter_init_many_pcpu()`和`percpu_counter_detach_many()` |
| include/linux/mm_types.h | 2/3 | 新增`struct mm_stat_ext`定义（在`struct mm_struct`之前），在`mm_struct`中嵌入`stat_ext`字段 |
| include/linux/percpu_counter.h | 2/3 | !SMP段新增`percpu_counter_init_many_cached`宏 |
| kernel/fork.c | 3/3 | Per-CPU缓存池实现，mm_init缓存路径，__mmdrop缓存归还路径，mm_stat_ext_cache_init，debugfs统计，mm_stat_ext_cached()辅助函数 |

## 已解决的技术问题

### 问题1：per-CPU缓存与CPU亲和性

**问题**：进程在CPU 0上fork创建mm，在CPU 100上exit释放mm，释放的缓存条目
进入CPU 100的本地缓存而非CPU 0的。

**决策**：put总是放回当前CPU的cache（无锁快速路径）。失衡可接受：
- 创建/销毁路径完全不持锁
- 缓存失衡随时间自我修正
- 回退路径（alloc_percpu）仍正确工作

### 问题2：mm_init中memset安全性

**问题**：`dup_mm()`的`memcpy(mm, oldmm, sizeof(*mm))`会拷贝父进程的
`stat_ext`和`percpu_counter`（含`list_head`）到子进程。

**决策**：不用`memset(&mm->stat_ext, 0, ...)`，改用显式逐字段清零。

### 问题3：percpu数据零化

**决策**：
- `percpu_counter_init_many_pcpu()`: memset全部零化percpu值
- `mm_cid.pcpu`: `for_each_possible_cpu: pcpu->cid = 0` + `mm_init_cid()`重建
- `percpu_counter`的`count`字段设置为0（amount参数）

### 问题4：struct mm_stat_ext定义位置（v2修复）

**问题**：v1中`struct mm_stat_ext`定义在`struct mm_struct`之后，但`mm_struct`
内嵌套使用`struct mm_stat_ext stat_ext`导致"incomplete type"编译错误。

**决策**：将`struct mm_stat_ext`定义前移至`struct mm_struct`之前。

### 问题5：!SMP编译（v2修复）

**问题**：v1在!CONFIG_SMP配置下缺少`percpu_counter_init_many_cached`宏定义，
导致编译错误。

**决策**：在`percpu_counter.h`的!SMP段添加该宏，直接调用
`percpu_counter_init_many_pcpu(fbc, value, counters, nr_counters, NULL)`。

### 问题6：__mmdrop条件判断改进（v2修复）

**问题**：v1使用`if (false || #ifdef...)`模式判断缓存状态，
checkpatch报告"Logical continuations should be on the previous line"。

**决策**：提取为`mm_stat_ext_cached()`辅助函数，清晰且无checkpatch警告。

### 问题7：并发安全性分析

**结论**：并发设计正确，无需修改：
- `mm_stat_ext_cache_get/put`使用`preempt_disable/enable`保护per-CPU数据
- `__mmdrop()`只在进程上下文调用，不会与中断冲突
- `mm_stat_ext_cache_put()`在缓存满时先`preempt_enable()`再调`free_percpu()`
  是安全的，此时mm尚未释放，percpu数据仍有效
- 统计计数器使用`atomic_long_t`，在`preempt_enable()`之后调用，无竞态

## 验证状态

- [x] 全量内核编译通过（CONFIG_SMP=y）
- [x] QEMU引导测试通过（内核正常启动，无panic，mm代码路径无错误）
- [x] checkpatch.pl --strict：0 errors, 0 warnings, 4 alignment checks
  （对齐checks与现有内核代码风格一致，为false positive）
- [ ] QEMU spawn压力测试
- [ ] 性能基准测试（对比有/无缓存的fork吞吐量）
- [ ] 缓存命中率统计验证（通过debugfs接口）

## 下一步

1. 在QEMU中运行spawn压力测试，验证缓存正确性
2. 通过debugfs `/sys/kernel/debug/mm_pcpu_cache/` 读取缓存命中率
3. 性能基准测试（fork/exit microbenchmark）
4. 根据`put_overflows`数据评估`MM_STAT_EXT_CACHE_MAX=8`是否合适
5. 考虑是否需要自适应缓存深度