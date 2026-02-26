# mm percpu cache 优化方案 (v2 - pcpu数据缓存)

## 1. 问题场景

spawn测试用例在4 NUMA、320核机器上并发执行时，fork/exit热路径存在锁竞争：

- `mm_init()` 中 `percpu_counter_init_many(mm->rss_stat, ...)` 调用 `alloc_percpu`
  获取pcpu内存，内部持有 `pcpu_alloc_mutex` + `pcpu_lock`
- `mm_init()` 中 `mm_alloc_cid()` 调用 `alloc_percpu` 分配 `mm_cid.pcpu`
- `__mmdrop()` 中 `percpu_counter_destroy_many()` + `mm_destroy_cid()` 调用
  `free_percpu` 释放pcpu内存，同样持锁

在320核并发fork/exit场景下，这些串行化的pcpu分配器锁成为瓶颈。

## 2. 前置优化

当前仓库最新的24个补丁已将tasklist_lock拆分为NUMA级别链表，去除了tasklist_lock竞争。

## 3. 设计方案

### 3.1 核心思路

缓存 `mm_struct` 中的 **percpu数据指针**（而非整个mm_struct），避免反复
`alloc_percpu`/`free_percpu`。

缓存的对象：
- `rss_stat[NR_MM_COUNTERS]` 的percpu数据区（`s32 __percpu *counters`）
- `mm_cid.pcpu`（`struct mm_cid_pcpu __percpu *`）

### 3.2 数据结构

```c
// include/linux/mm_types.h
// 注意：定义必须在 struct mm_struct 之前，否则会触发 incomplete type 错误
struct mm_stat_ext {
    s32 __percpu *rss_counters;              // SMP only: rss_stat的percpu数据区指针
#ifdef CONFIG_SCHED_MM_CID
    struct mm_cid_pcpu __percpu *mm_cid_pcpu; // mm_cid的percpu数据区指针
#endif
};

// 嵌入mm_struct中（非指针）
struct mm_struct {
    ...
    struct percpu_counter rss_stat[NR_MM_COUNTERS];
    struct mm_stat_ext stat_ext;              // <-- 嵌入，非指针
    ...
};

// Per-CPU缓存池
#define MM_STAT_EXT_CACHE_MAX 8

struct mm_stat_ext_cache {
    unsigned int count;
    struct mm_stat_ext entries[MM_STAT_EXT_CACHE_MAX];
};

static DEFINE_PER_CPU(struct mm_stat_ext_cache, mm_stat_ext_free);
```

### 3.3 缓存路径

#### mm_init() - 分配路径

```
mm_init()
  ├── mm_stat_ext_cache_get(&mm->stat_ext)    // 尝试从本CPU缓存取
  │   ├── 缓存命中:
  │   │   ├── percpu_counter_init_many_pcpu(mm->rss_stat, 0, cached_rss, NR_MM_COUNTERS)
  │   │   │   └── 零化所有percpu值，初始化spinlock/list/count/counter指针
  │   │   ├── mm->mm_cid.pcpu = cached_cid_pcpu
  │   │   ├── for_each_possible_cpu: pcpu->cid = 0
  │   │   └── mm_init_cid(mm, p)    // 重新初始化mm_cid逻辑状态
  │   └── 缓存未命中 (fallback):
  │       ├── mm_alloc_cid(mm, p)    // 原始alloc_percpu路径
  │       ├── percpu_counter_init_many(...)  // 原始alloc_percpu路径
  │       ├── mm->stat_ext.rss_counters = mm->rss_stat[0].counters   // SMP only
  │       └── mm->stat_ext.mm_cid_pcpu = mm->mm_cid.pcpu              // CID only
  └── mm->user_ns = get_user_ns(user_ns)
```

#### __mmdrop() - 释放路径

```
__mmdrop()
  ├── mm_free_pgd, mm_free_id, destroy_context, ...
  ├── check_mm(mm)   // 验证rss_stat值为0
  ├── if (mm_stat_ext_cached(mm)):       // v2: 用辅助函数代替 if(false || #ifdef...)
  │   ├── percpu_counter_detach_many(mm->rss_stat, NR_MM_COUNTERS)  // SMP only
  │   │   └── 从hotplug list摘除、NULL指针，但不free_percpu
  │   ├── mm->mm_cid.pcpu = NULL                                    // CID only
  │   └── mm_stat_ext_cache_put(&mm->stat_ext)   // 归还本CPU缓存
  │       ├── 缓存未满: 存入本CPU缓存
  │       └── 缓存已满: free_percpu回滚
  └── else (无缓存数据):
      ├── mm_destroy_cid(mm)              // 原始free_percpu路径
      └── percpu_counter_destroy_many()  // 原始free_percpu路径
```

### 3.4 percpu_counter基础设施

新增两个辅助函数：

| 函数 | 作用 |
|------|------|
| `percpu_counter_init_many_pcpu()` | 用预分配的percpu区域初始化counters，零化所有percpu值 |
| `percpu_counter_detach_many()` | 从hotplug list摘除、NULL counters指针，但不释放percpu数据 |

这两个函数是对 `percpu_counter_init_many()`/`percpu_counter_destroy_many()` 的
"分离数据所有权"变体：初始化时复用已有数据区，销毁时只切断联系不释放数据。

`percpu_counter_init_many_cached()` 宏封装了lockdep key的自动生成，简化调用方。

### 3.5 关键设计决策

1. **嵌入式stat_ext**（非指针）：
   - `mm_struct`中嵌入`struct mm_stat_ext stat_ext`而非指针
   - `mm_init()`中显式逐字段清零（`rss_counters = NULL; mm_cid_pcpu = NULL`）
   - 避免对含`list_head`的`percpu_counter`结构体做`memset`可能破坏链表

2. **Per-CPU缓存，无锁**：
   - `mm_stat_ext_cache_get/put` 仅使用 `preempt_disable/enable`
   - 不持任何spinlock/mutex
   - CPU亲和性自然保证：创建的CPU分配→销毁的CPU回收

3. **CPU亲和性与失衡**：
   - put总放回当前CPU的cache
   - fork在CPU0、exit在CPU100时，CPU0缓存空、CPU100缓存满
   - 可接受：无锁快路径优势远大于偶尔的alloc_percpu回退

4. **stat_ext字段零化策略**：
   - 使用逐字段赋值而非`memset(&mm->stat_ext, 0, ...)`
   - 避免对将来可能新增的list_head等字段造成隐蔽破坏

5. **CONFIG条件编译**：
   - `rss_counters`仅在`CONFIG_SMP`下存在/使用
   - `mm_cid_pcpu`仅在`CONFIG_SCHED_MM_CID`下存在/使用
   - v2: `mm_stat_ext_cached()`辅助函数封装条件判断，避免`if (false || #ifdef...)`模式

6. **stat_ext定义位置** (v2新增)：
   - `struct mm_stat_ext`必须定义在`struct mm_struct`之前
   - 否则`mm_struct`内嵌套`struct mm_stat_ext stat_ext`会触发"incomplete type"编译错误

7. **!SMP兼容** (v2新增)：
   - `percpu_counter_init_many_cached`宏在`!CONFIG_SMP`段必须提供
   - 否则fork.c中的缓存命中路径在UP编译时报隐式声明错误

8. **缓存统计** (v2新增)：
   - 在`CONFIG_SMP`下通过debugfs暴露缓存命中率统计
   - `/sys/kernel/debug/mm_pcpu_cache/` 提供四项计数器
   - `!SMP`下为空内联函数，零运行时开销

## 4. 补丁拆分

| 补丁 | 文件 | 说明 |
|------|------|------|
| 1/3 | percpu_counter.h, percpu_counter.c | `init_many_pcpu`和`detach_many`基础设施，!SMP stub |
| 2/3 | mm_types.h, percpu_counter.h | `struct mm_stat_ext`定义（在mm_struct之前），嵌入`mm_struct`，!SMP `percpu_counter_init_many_cached`宏 |
| 3/3 | fork.c | Per-CPU缓存池实现，mm_init/__mmdrop路径修改，debugfs统计，mm_stat_ext_cached()辅助函数 |

## 5. 初始化安全性分析

### 5.1 percpu值的零化

| 路径 | rss_stat percpu值 | mm_cid.pcpu cid值 |
|------|-------------------|-------------------|
| 缓存命中 | `percpu_counter_init_many_pcpu()`中`for_each_possible_cpu` memset 0 | `mm_init()`中`for_each_possible_cpu: pcpu->cid = 0` |
| 缓存未命中 | `alloc_percpu`返回已零化的内存 | `alloc_percpu`返回已零化的内存 |

### 5.2 mm_init中rss_stat的memset

原代码`memset(&mm->rss_stat, 0, sizeof(mm->rss_stat))`将`percpu_counter`数组的
`lock`/`list`/`count`/`counters`全部零化。这在两条路径下都安全：

- 缓存未命中：memset在`percpu_counter_init_many()`之前，后续init重新初始化所有字段
- 缓存命中：memset在`percpu_counter_init_many_pcpu()`之前，后续init重新初始化

### 5.3 dup_mm的memcpy

`dup_mm()`中`memcpy(mm, oldmm, sizeof(*mm))`会将父进程的`stat_ext`指针
拷贝到子进程。`mm_init()`中的显式零化将其清零，确保子进程不会误用
父进程的percpu数据。

## 6. 与v1方案的对比

| | v1 (mm_struct缓存) | v2 (percpu数据缓存) |
|---|---|---|
| 缓存对象 | 整个mm_struct | 仅percpu数据指针 |
| 缓存深度 | 2 | 8 (per CPU) |
| 锁 | local_lock_t | 无锁(preempt_disable) |
| 复杂度 | 需处理整个mm的重新初始化 | 只需处理percpu数据 |
| 内存占用 | ~10KB/mm_struct + percpu | ~2个percpu指针 |
| memset安全 | 需避免对已init的list_head memset | 只零化两个指针字段 |

## 7. 缓存统计接口 (v2新增)

在`CONFIG_SMP`下，通过debugfs暴露缓存运行时统计：

```
/sys/kernel/debug/mm_pcpu_cache/
├── get_hits          # 缓存命中（mm_stat_ext_cache_get返回true）
├── get_misses        # 缓存未命中（fallback到alloc_percpu）
├── put_stores        # 成功将percpu数据存入本CPU缓存
└── put_overflows     # 缓存已满，需要free_percpu释放
```

实现方式：
- `CONFIG_SMP`: 四个`atomic_long_t`计数器 + `DEFINE_SHOW_ATTRIBUTE`导出debugfs
- `!CONFIG_SMP`: 四个空内联函数，编译器优化为无操作，零开销

统计可用于：
- 验证缓存命中率是否足够高（>50%说明有效）
- 判断`MM_STAT_EXT_CACHE_MAX=8`是否合适
- 评估CPU亲和性对缓存效率的影响

## 8. 补丁文件位置

```
mm_pcpu_dev/
├── v2-0001-lib-percpu_counter-add-init_many_pcpu-and-detach_.patch
├── v2-0002-mm-add-mm_stat_ext-struct-for-caching-percpu-data.patch
└── v2-0003-fork-implement-per-cpu-cache-for-mm-percpu-data.patch
```

## 9. 验证状态

- [x] 全量内核编译通过（CONFIG_SMP=y + CONFIG_SCHED_MM_CID=y）
- [x] QEMU引导测试通过（内核正常启动，无panic，mm代码路径无错误）
- [x] checkpatch.pl --strict：0 errors, 0 warnings, 4 alignment checks
  （对齐checks与现有内核代码风格一致，为false positive）
- [ ] QEMU spawn压力测试
- [ ] 性能基准测试（对比有/无缓存的fork吞吐量）
- [ ] 缓存命中率统计验证（通过debugfs接口）

## 10. 待完成项

1. spawn压力测试验证缓存正确性
2. 性能基准测试（fork/exit microbenchmark）
3. 缓存命中率统计验证（debugfs接口）
4. `MM_STAT_EXT_CACHE_MAX`参数调优（基于命中率数据）
5. 评估是否需要自适应缓存深度（根据CPU数量或负载特征）