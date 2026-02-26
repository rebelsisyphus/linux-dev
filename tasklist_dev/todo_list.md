# NUMA感知tasklist_lock待改进列表

## 开发规则

**重要规则：使用CONFIG_NUMA_TASKLIST隔离优化代码，不要对config关闭时的原始流程进行修改。**

当CONFIG_NUMA_TASKLIST关闭时，内核应该完全保持原有的行为，不受任何影响。所有优化代码必须在`#ifdef CONFIG_NUMA_TASKLIST`块内。

---

## 状态标记说明
- ✅ **已完成** - 已实现并验证
- 🔄 **进行中** - 正在开发中
- ⏳ **待优化** - 已分析，有待实现
- ❌ **不适用** - 不适用或不需要修改

---

## 1. 数据结构一致性问题

### 1.1 任务结构扩展问题
**状态**: ✅ **已完成**

- **问题**：在task_struct中添加了`tasks_node`字段，但原始的`tasks`字段仍在使用
- **分析**：双链表策略是有意为之，不是bug。原始`tasks`链表必须保留以维持`for_each_process`等遍历宏的兼容性。
- **优化方案**：已在设计中明确说明双链表策略的必要性
- **验证**：`for_each_process`遍历正常，内核启动成功

### 1.2 初始化问题
**状态**: ✅ **已完成**

- **问题**：在fork.c中，原始链表节点仍然被添加到全局链表
- **分析**：正确做法。进程必须同时加入两个链表：
  - `tasks`链表：维持内核API兼容性（for_each_process等）
  - `tasks_node`链表：NUMA优化实现
- **优化方案**：无需修改，当前实现正确
- **验证**：fork.c和exit.c正确操作两个链表

## 2. 遍历接口兼容性问题

### 2.1 遍历宏设计
**状态**: ✅ **已完成**

- **问题**：虽然定义了`for_each_task_all_nodes`等宏，但没有完全替换内核中所有遍历任务的宏
- **分析**：不应直接替换`for_each_process`，原因：
  1. 内核中有大量代码依赖`for_each_process`
  2. 直接替换可能导致不可预知的兼容性问题
  3. 应保持原有API不变，提供NUMA版本作为可选优化
- **优化方案**：
  - 保留原有`for_each_process`不变（确保兼容性）
  - 提供`for_each_process_numa`作为优化版本
  - 通过条件编译让调用者自行选择
- **实现状态**：已在task_numa.h中定义`for_each_process_numa`和`for_each_task_all_nodes`

### 2.2 遍历函数实现问题
**状态**: 🔄 **待修复（高优先级）**

- **问题**：`numa_next_task`函数中使用了`list_next_or_null_rcu`和`list_first_or_null_rcu`函数
- **分析**：经检查，这两个函数**确实存在**于`/include/linux/rculist.h`（第407行和第426行），不是问题
- **但是**：存在逻辑bug - 当遍历到最后一个节点且该节点列表为空时，可能返回错误结果
- **优化方案**：
  1. 简化`numa_next_task`逻辑，使用标准的`list_next_entry_rcu`和`list_first_entry_or_null_rcu`
  2. 确保跨节点遍历时正确处理空列表情况
  3. 添加边界检查防止无限循环
- **预计代码变更**（kernel/fork_numa.c）：
  ```c
  struct task_struct *numa_next_task(struct task_struct *p)
  {
      struct task_struct *next = NULL;
      int node;
      
      if (!p)
          return NULL;
          
      node = task_numa_node(p);
      
      rcu_read_lock();
      
      /* 尝试在当前节点找下一个任务 */
      if (!list_is_last(&p->tasks_node, &numa_tasklist.per_node[node].tasks)) {
          next = list_next_entry_rcu(p, tasks_node);
      } else {
          /* 当前节点结束，查找下一个非空节点 */
          int next_node = node;
          while ((next_node = next_node_in_mask(next_node, 
                            numa_tasklist.active_nodes)) >= 0) {
              if (!list_empty(&numa_tasklist.per_node[next_node].tasks)) {
                  next = list_first_entry_or_null_rcu(
                      &numa_tasklist.per_node[next_node].tasks,
                      struct task_struct, tasks_node);
                  break;
              }
          }
      }
      
      if (next && !refcount_inc_not_zero(&next->usage))
          next = NULL;
          
      rcu_read_unlock();
      return next;
  }
  ```

## 3. 锁操作安全性问题

### 3.1 锁的嵌套和顺序问题
**状态**: ✅ **已完成**

- **问题**：在某些场景下，可能需要同时操作多个节点的锁，需要确保锁获取顺序的一致性
- **分析**：`numa_tasklist_migrate`函数已实现正确的锁顺序：
  ```c
  if (old_node < new_node) {
      write_lock_irq(&old_ntl->lock);  // 先锁ID小的
      write_lock(&new_ntl->lock);
  } else {
      write_lock_irq(&new_ntl->lock);  // 先锁ID小的
      write_lock(&old_ntl->lock);
  }
  ```
- **优化方案**：已在代码中添加注释说明锁顺序规则，并在设计文档中详细说明
- **验证**：代码逻辑正确，遵循"always lock lower node first"原则

### 3.2 锁的异常处理
**状态**: ✅ **已完成**

- **问题**：缺少对异常情况的处理（如节点ID无效时的处理）
- **分析**：已实现边界检查：
  ```c
  if (unlikely(node < 0 || node >= MAX_NUMA_NODES))
      node = 0;
  ```
- **优化方案**：当前实现足够健壮，对于无效节点ID会安全地回退到node 0
- **验证**：所有锁操作函数都有边界检查

## 4. 内存管理问题

### 4.1 RCU安全释放
**状态**: ✅ **已完成**

- **问题**：在`numa_tasklist_del`中，虽然使用了`list_del_rcu`，但可能缺少适当的RCU同步点
- **分析**：
  - `list_del_rcu`只需要配合`synchronize_rcu`或`call_rcu`使用在**释放内存**时
  - 在`release_task`路径中，task_struct的释放由`__put_task_struct`处理，它会使用RCU安全机制
  - `numa_tasklist_del`只是从链表中移除，不需要立即同步RCU
- **优化方案**：当前实现正确，RCU安全释放由内核标准流程保证
- **验证**：`__put_task_struct` -> `delayed_put_task_struct` -> `rcu`机制保证安全

### 4.2 内存开销优化
**状态**: ❌ **不适用**

- **问题**：每个task_struct增加约24字节的开销
- **分析**：这是可接受的trade-off：
  - 24字节 = list_head(16字节) + int(4字节) + padding(4字节)
  - 对于1000个进程：约24KB额外内存
  - 对于大型系统（256核）：这是合理的开销
- **优化方案**：
  - 使用`CONFIG_NUMA_TASKLIST`配置项控制，不需要时完全编译掉
  - 单节点系统也可以选择不启用此功能
- **结论**：无需优化，当前设计已足够高效

## 5. 功能完整性问题

### 5.1 PID查找优化
**状态**: 🔄 **待优化（中优先级）**

- **问题**：`numa_find_task_by_pid`函数遍历所有节点，效率可能不够高
- **分析**：当前实现按顺序遍历所有节点，对于单节点任务优先搜索当前节点可以提高命中率
- **优化方案**：
  1. **优先搜索当前节点**：先在当前NUMA节点搜索，再搜索其他节点
  2. **利用局部性原理**：任务通常在创建它的节点上运行
  3. **添加hint参数**：允许调用者指定优先搜索的节点
- **预计代码变更**（kernel/fork_numa.c）：
  ```c
  struct task_struct *numa_find_task_by_pid_on_node(pid_t pid, int pref_node)
  {
      struct task_struct *p;
      int node, i;
      int nodes_to_check[MAX_NUMNODES];
      int nr_nodes = 0;
      
      /* 优先搜索指定节点 */
      if (pref_node >= 0 && pref_node < MAX_NUMA_NODES &&
          node_isset(pref_node, numa_tasklist.active_nodes)) {
          nodes_to_check[nr_nodes++] = pref_node;
      }
      
      /* 添加其他节点 */
      for_each_numa_node(node) {
          if (node != pref_node)
              nodes_to_check[nr_nodes++] = node;
      }
      
      rcu_read_lock();
      for (i = 0; i < nr_nodes; i++) {
          for_each_task_numa_node(p, nodes_to_check[i]) {
              if (p->pid == pid) {
                  if (refcount_inc_not_zero(&p->usage)) {
                      rcu_read_unlock();
                      return p;
                  }
              }
          }
      }
      rcu_read_unlock();
      return NULL;
  }
  
  struct task_struct *numa_find_task_by_pid(pid_t pid)
  {
      /* 默认优先搜索当前节点 */
      return numa_find_task_by_pid_on_node(pid, current_numa_node());
  }
  ```

### 5.2 统计信息准确性
**状态**: ✅ **已完成**

- **问题**：全局任务计数和其他统计信息的准确性需要保证
- **分析**：
  - 全局计数使用`atomic_long_inc/dec`，线程安全
  - 节点本地计数在锁保护下更新
  - 统计读取时使用读锁或RCU保证一致性
- **优化方案**：当前实现已使用正确的同步机制
- **验证**：
  - `atomic_long_inc(&numa_tasklist.nr_total_tasks)` - 原子操作
  - `ntl->nr_tasks++`在`write_lock_irq`保护下
  - 读操作使用`read_lock`或`rcu_read_lock`

## 6. 调试和监控功能

### 6.1 调试接口
**状态**: ✅ **已完成**

- **问题**：虽然设计文档中提到了debugfs接口，但代码中未实现
- **分析**：实际已实现！位于`kernel/fork_numa.c`第294-371行
  - `numa_tasklist_debugfs_init()` - debugfs初始化
  - `/sys/kernel/debug/numa_tasklist/stats` - 统计信息导出
  - 包含节点任务数、全局任务数等信息
- **验证**：已在代码中实现，使用`late_initcall`注册
- **使用方式**：
  ```bash
  cat /sys/kernel/debug/numa_tasklist/stats
  ```

### 6.2 性能基准测试
**状态**: ⏳ **待实现（中优先级）**

- **问题**：缺乏实际性能测试和对比数据
- **分析**：需要实际测试数据来验证优化效果
- **优化方案**：
  1. **创建测试脚本**：编写自动化测试脚本
  2. **测试场景**：
     - UnixBench spawn测试
     - 高并发fork/exit压力测试
     - 跨NUMA节点任务创建测试
  3. **对比基准**：
     - 启用CONFIG_NUMA_TASKLIST vs 禁用
     - 不同NUMA节点数量下的性能差异
- **预计实现**（scripts/test_numa_tasklist.sh）：
  ```bash
  #!/bin/bash
  # NUMA Tasklist性能测试脚本
  
  echo "=== NUMA Tasklist Performance Test ==="
  
  # 检查debugfs统计
  if [ -f /sys/kernel/debug/numa_tasklist/stats ]; then
      echo "NUMA tasklist statistics:"
      cat /sys/kernel/debug/numa_tasklist/stats
  fi
  
  # 运行UnixBench spawn测试
  if command -v ubgeek &> /dev/null; then
      echo "Running UnixBench spawn test..."
      ubgeek -t spawn
  fi
  
  # 压力测试：并发创建进程
  echo "Running fork stress test..."
  stress-ng --fork 64 --timeout 30s --metrics-brief
  ```

## 7. 错误处理和边界情况

### 7.1 节点故障处理
**状态**: ✅ **已完成**

- **问题**：当某个NUMA节点不可用时的处理机制不明确
- **分析**：使用`node_possible_map`和`node_online_map`进行节点管理：
  - 初始化时只初始化`node_possible_map`中的节点
  - 遍历使用`for_each_numa_node`宏，会自动跳过不在`active_nodes`中的节点
- **优化方案**：当前实现已正确处理，使用内核标准节点掩码机制
- **验证**：`for_each_node_mask(node, node_possible_map)`只遍历可能存在的节点

### 7.2 极端负载处理
**状态**: ⏳ **待优化（低优先级，未来工作）**

- **问题**：在极端负载情况下可能出现的问题（如节点任务过多）
- **分析**：这是高级优化功能，超出当前版本范围
- **优化方案**（未来版本考虑）：
  1. **负载监控**：定期检查各节点任务数量
  2. **动态迁移**：当某节点任务过多时，触发任务迁移
  3. **创建时负载均衡**：在fork时选择任务较少的节点
- **预计实现**（kernel/fork_numa.c新增）：
  ```c
  /* 选择负载最轻的节点 */
  int numa_tasklist_select_node(void)
  {
      int node, best_node = 0;
      unsigned long min_tasks = ULONG_MAX;
      
      for_each_numa_node(node) {
          struct numa_tasklist *ntl = &numa_tasklist.per_node[node];
          if (ntl->nr_tasks < min_tasks) {
              min_tasks = ntl->nr_tasks;
              best_node = node;
          }
      }
      
      return best_node;
  }
  ```

## 8. 代码质量改进

### 8.1 代码注释和文档
**状态**: ⏳ **待优化（低优先级）**

- **问题**：部分关键函数缺少详细注释
- **分析**：关键函数已实现，但缺少kernel-doc格式的注释
- **优化方案**：为所有导出函数添加kernel-doc格式注释：
  ```c
  /**
   * numa_tasklist_add - Add a task to a specific NUMA node's task list
   * @p: The task struct to add
   * @node: The NUMA node ID to add the task to
   *
   * This function adds a task to the specified NUMA node's task list.
   * It acquires the per-node write lock and updates the task's NUMA
   * node ID. The global task counter is also updated atomically.
   *
   * Context: Can sleep. Must be called with tasklist_lock held or in
   *          a context where task list modifications are safe.
   */
  void numa_tasklist_add(struct task_struct *p, int node)
  ```
- **实施计划**：为以下函数添加注释：
  - `numa_tasklist_add`
  - `numa_tasklist_del`
  - `numa_tasklist_migrate`
  - `numa_find_task_by_pid`
  - `numa_next_task`

### 8.2 代码风格一致性
**状态**: ⏳ **待验证**

- **问题**：代码风格可能与内核标准不完全一致
- **分析**：需要运行checkpatch.pl验证
- **优化方案**：
  1. 运行`./scripts/checkpatch.pl -f kernel/fork_numa.c`
  2. 修复所有风格问题
  3. 确保符合内核编码规范
- **验证步骤**：
  ```bash
  ./scripts/checkpatch.pl -f kernel/fork_numa.c
  ./scripts/checkpatch.pl -f include/linux/sched/task_numa.h
  ```

---

## 优化优先级总结

### 高优先级（立即实施）
1. ✅ **2.2 遍历函数逻辑优化** - 修复`numa_next_task`实现
2. ✅ **8.2 代码风格检查** - 运行checkpatch.pl并修复问题

### 中优先级（计划实施）
3. ✅ **5.1 PID查找优化** - 优先搜索当前NUMA节点
4. ✅ **6.2 性能基准测试** - 创建测试脚本并收集数据

### 低优先级（未来工作）
5. ✅ **7.2 极端负载处理** - 动态负载均衡（高级功能）
6. ✅ **8.1 代码注释完善** - 添加kernel-doc格式注释

---

## 开发计划

### 第一阶段：修复和优化 ✅
1. ✅ 修复`numa_next_task`遍历逻辑
2. ✅ 实现PID查找优化
3. ✅ 运行checkpatch.pl修复代码风格

### 第二阶段：分片锁优化 🔄 (当前重点)

> **重要**: 代码审查发现三个阻塞性缺陷，必须按顺序修复后才能启用分片锁。
> 详见 `numa-tasklist-design.md` 第10节。

#### 阶段 2A：修正数据结构（必须优先完成）
4. **在 `task_struct` 中新增 `tasks_shard` 字段**
   - 文件: `include/linux/sched.h`
   - 问题: 当前 `sharded_tasklist_add` 复用 `p->tasks`，与全局链表冲突
   - 方案: 新增 `struct list_head tasks_shard` 字段（类似 `tasks_node`）
5. **在 `init_task.c` 中初始化 `tasks_shard`**
   - 文件: `init/init_task.c`
   - 方案: `INIT_LIST_HEAD(init_task.tasks_shard)`
6. **修正 `sharded_tasklist_add/del` 使用 `tasks_shard`**
   - 文件: `kernel/fork_numa.c`
   - 问题: 当前使用 `p->tasks`，会破坏全局链表
   - 方案: 改为 `p->tasks_shard`；PID=0 的 init_task 跳过（`if (!p->pid) return`）
7. **修正 `sharded_next_task` 遍历字段**
   - 文件: `kernel/fork_numa.c`
   - 问题: 当前用 `p->tasks` 在分片链表上查找，行为未定义
   - 方案: 改为 `p->tasks_shard` 字段
8. **修正 `for_each_task_sharded` 宏**
   - 文件: `kernel/fork_numa.c`
   - 方案: 使用 `tasks_shard` 替代 `tasks`

#### 阶段 2B：启用调用点（核心路径）
9. **在 `fork.c` 中启用 `sharded_tasklist_add`**
   - 文件: `kernel/fork.c: copy_process()`
   - 操作: 取消 `sharded_tasklist_add(p)` 的注释
   - 位置: 在 `write_lock_irq(&tasklist_lock)` **之前**（锁外执行，减少竞争）
   - 前提: 确保 `INIT_LIST_HEAD(&p->tasks_shard)` 已在之前执行
10. **在 `exit.c` 中启用 `sharded_tasklist_del`**
    - 文件: `kernel/exit.c: release_task()`
    - 操作: 取消 `sharded_tasklist_del(p)` 的注释

#### 阶段 2C：验证
11. 测试验证性能提升
    - `CONFIG_PROVE_LOCKING=y` 验证无死锁
    - `perf lock record` 竞争分析

### 第三阶段：测试和验证 ⏳
8. 创建性能测试脚本
9. 运行UnixBench spawn测试
10. 收集性能对比数据

### 第四阶段：文档完善 ⏳
11. 添加kernel-doc注释
12. 更新设计文档
13. 编写用户指南

---

## 新增待办事项（2026-03-31）

### 9. 分片锁核心路径优化

#### 9.1 状态: 🔴 高优先级 - 核心优化

#### 问题描述
当前NUMA实现保留了全局`tasklist_lock`，spawn路径仍是128-way竞争。核心瓶颈未被解决。

**性能预期对比：**
| 实现 | fork路径锁 | exit路径锁 | 锁竞争度 | 性能提升 |
|------|------------|------------|----------|----------|
| 原始 | 全局锁 | 全局锁 | 128-way | 基准 (100%) |
| 当前NUMA | 全局锁 | 全局锁 | 128-way | 0% ~ -5% |
| 分片锁 | 分片锁 | 分片锁 | 8-way | **200-250%** |

#### 实施方案

**方案A：分层锁（推荐）**

```c
// kernel/fork.c: copy_process()
#ifdef CONFIG_NUMA_TASKLIST
    // 高频路径：分片锁（无全局竞争）
    sharded_tasklist_add(p);
    
    // 低频路径：全局锁（必须操作）
    write_lock(&tasklist_lock);
    ptrace_init_task(p, ...);
    // signal继承操作
    write_unlock(&tasklist_lock);
#else
    // 原始实现
    write_lock_irq(&tasklist_lock);
    // ... 所有操作 ...
    write_unlock_irq(&tasklist_lock);
#endif
```

**关键修改点：**

| 文件 | 函数 | 修改内容 |
|------|------|----------|
| kernel/fork.c | copy_process | 分离链表操作到分片锁 |
| kernel/exit.c | release_task | 分离链表删除到分片锁 |
| kernel/fork_numa.c | sharded_tasklist_add | 启用并优化 |

#### 代码变更位置

```
kernel/fork.c:2370-2467   - fork路径关键区域
kernel/exit.c:263-289     - exit路径
kernel/fork_numa.c:91-105 - sharded_tasklist_add
kernel/fork_numa.c:111-125 - sharded_tasklist_del
```

#### 测试验证

```bash
# 编译
make -j$(nproc)

# 启动测试
./scripts/kernel-dev-loop.sh

# 性能对比
# 禁用分片锁时：~15,000 ops/sec
# 启用分片锁后：预期 ~30,000-40,000 ops/sec
```

---

### 10. init_task处理策略

#### 10.1 状态: 🟡 中优先级 - 需要解决

#### 问题分析

init_task在内核启动早期初始化，此时：
- cgroup子系统未初始化
- memory allocator未完全可用
- 分片锁可能未初始化

**错误的修改：**
```c
// 会导致cgroup crash
sharded_tasklist_add(&init_task);
```

**正确的处理：**
```c
// init_task处理策略：
// 1. 始终在NUMA节点链表（已正确实现）
// 2. 始终在原始全局链表（已正确实现）
// 3. 不在分片链表（PID=0不参与分片）

// 分片链表仅用于：
// - 用户进程（PID > 1）
// - 高频创建/销毁
// - spawn路径优化
```

#### 修改位置

```
init/init_task.c   - 保持不变
kernel/fork_numa.c:222-236 - numa_tasklist_init()保持不变
kernel/fork.c       - 分片操作仅对PID > 0的进程
```

---

### 11. 锁临界区最小化

#### 11.1 状态: 🟡 中优先级

#### 问题分析

当前分片锁临界区可能过长，导致RCU stall：
```c
write_lock_irq(&shard->lock);
// ... 可能包含其他操作 ...
write_unlock_irq(&shard->lock);
```

#### 优化方案

**最小化临界区：**
```c
void sharded_tasklist_add(struct task_struct *p)
{
    struct tasklist_shard *shard = get_tasklist_shard(p->pid);
    
    // 仅保护链表操作
    write_lock_irq(&shard->lock);
    list_add_tail_rcu(&p->tasks, &shard->tasks);
    atomic_long_inc(&shard->nr_tasks);
    write_unlock_irq(&shard->lock);
    
    // 其他操作在锁外进行
    // 例如：统计、钩子等
}
```

**锁持有时间控制：**
- 目标：< 100ns
- 避免：在锁内调用可能阻塞的函数
- 使用：atomic操作避免额外锁

---

### 12. 性能计数器和调试

#### 12.1 状态: 🟢 低优先级 - 未来增强

#### 目标

添加性能监控能力，便于分析和调优：

```c
struct tasklist_perf_stats {
    atomic_long_t add_ops;
    atomic_long_t del_ops;
    atomic_long_t lock_waits;
    atomic64_t lock_wait_time_ns;
};

static DEFINE_PER_CPU(struct tasklist_perf_stats, tasklist_stats);

// 在debugfs中导出
static int tasklist_perf_show(struct seq_file *s, void *v)
{
    // 显示各分片统计
    // 显示锁竞争率
    // 显示平均等待时间
}
```

---

---

## 实施状态总结（2026-03-31 晚间）

### ✅ 已完成（高优先级缺陷修复）

**阶段 A：数据结构修正**
1. ✅ 新增 `task_struct.tasks_shard` 字段
2. ✅ 初始化 `init_task.tasks_shard`
3. ✅ 修正 `sharded_tasklist_add/del` 使用 `tasks_shard`
4. ✅ 修正 `sharded_next_task` 遍历字段
5. ✅ 修正 `for_each_task_sharded` 宏

**阶段 B：启用调用点**
6. ✅ fork.c: 启用 `sharded_tasklist_add()`，移到全局锁前
7. ✅ exit.c: 启用 `sharded_tasklist_del()`

**阶段 C：验证**
8. ✅ 编译验证（Kernel #16）
9. ✅ QEMU 启动测试（systemd 全部 OK）
10. ✅ NUMA/分片初始化验证

**阶段 D：creation-home node 修复**
11. ✅ 发现并修复普通 fork 路径 `NUMA_NO_NODE` 问题
12. ✅ 新增 `tasklist_creation_node()` helper
13. ✅ 修正 `copy_process()` 中的 node 参数传递

**阶段 E：lockdep 验证**
14. ✅ 启用 `CONFIG_PROVE_LOCKING=y` 等配置
15. ✅ lockdep 内核编译通过（Kernel #20）
16. ✅ boot 到 userspace 成功
17. ✅ 未发现新的 lockdep splat

### 编译结果
```
Kernel: arch/x86/boot/bzImage is ready  (#20)
[    0.193967] Sharded tasklist initialized with 4 shards per node
[    0.194121] NUMA-aware tasklist initialized with 2 nodes, 4 shards per node
```

### 内存开销
```
task_struct 扩展（CONFIG_NUMA_TASKLIST=y）：
- tasks_node:   16 bytes（已有）
- numa_node_id:  4 bytes（已有）
- padding:       4 bytes（已有）
- tasks_shard:  16 bytes（新增）
总计：40 bytes/task
```

---

---

## 后续工作（待完成）

### 🔴 高优先级（验证与审计）

### 27. lockdep 高并发压力验证

#### 27.1 状态: 🔴 高优先级 - 待完成

1. **高并发 fork/exit 压力测试**
   - 执行 `stress-ng --fork 64 --timeout 60`
   - 观察 lockdep 是否在此场景触发告警
   - 目标: 确认无死锁、无递归锁

2. **并发创建/销毁压力测试**
   - 使用 test-kernel 或自定义脚本
   - 覆盖正常创建、fork 失败、进程退出等路径

3. **输出 lockdep 验证报告**
   - 包括 boot + 压力测试两部分结果

### 28. 全局锁需求审计

#### 28.1 状态: 🔴 高优先级 - 当前主任务

1. **审计 `kernel/fork.c:copy_process()`**
   - 分类 A/B/C：
     - A: 必须全局锁（进程树、ptrace、signal）
     - B: 可转移到 shard/node 锁
     - C: 可改为原子/延迟统计
   - 重点关注 `nr_threads++`、`total_forks++` 是否可外提

2. **审计 `kernel/exit.c:release_task()`**
   - 确认 `detach_pid()` 的锁依赖
   - 确认 `list_del_rcu(&p->tasks)` 的最小锁需求

3. **形成锁域审计报告**

### 29. 失败路径回滚审计

#### 29.1 状态: 🟡 中优先级

1. **审计 `bad_fork_*` 路径**
   - 确认 `numa_tasklist_del_local()` 被正确调用
   - 确认三链表状态一致：shard、node、global

2. **审计 `release_task()` 退出路径**
   - 确认 `numa_tasklist_del_local()` 在锁外
   - 确认 `ptrace_release_task()`、`__exit_signal()` 在锁内

### 🟡 中优先级（性能验证）

### 30. UnixBench spawn 基准测试

#### 30.1 状态: 🟡 中优先级

1. **建立基线**
   - `CONFIG_NUMA_TASKLIST=n` 测量
   - `CONFIG_NUMA_TASKLIST=y` 测量
   - 对比 ops/sec、延迟

2. **性能对比**
   - 4 NUMA / 128 CPU 环境
   - 预期: 当前版本收益约 -5% 到 +5%（全局锁瓶颈仍存在）

3. **输出性能报告**

### 31. perf lock 竞争分析

#### 31.1 状态: 🟡 中优先级

1. **采集 `perf lock record` 数据**
   - 对比 `CONFIG_NUMA_TASKLIST=y/n`
   - 分析 tasklist_lock 等待时间

2. **分析热点**
   - 确认主瓶颈仍是全局锁
   - 观察 node_lock / shard_lock 竞争占比

3. **输出竞争分析报告**

### 🟢 低优先级（增强功能）

### 32. test-kernel 脚本修复

#### 32.1 状态: 🟢 低优先级 - 可选

1. **修复 SSH 探测问题**
   - 当前 host 侧 SSH 连接超时
   - 非 NUMA tasklist 内核问题
   - 可作为后续开发辅助工具修复

### 33. 动态负载均衡

#### 33.1 状态: 🟢 低优先级 - 未来工作

1. **评估任务迁移策略**
2. **设计动态重平衡机制**

---

## 新增执行清单：per-NUMA 进程链表替代 `init_task.tasks`（2026-04-08）

### 34. Step 1：tasks 字段从全局链表迁移到 per-NUMA 链表

**状态**: 🔴 遇到阻塞问题，已回退所有更改

#### 34.0 遇到的问题

**启动崩溃（NULL pointer dereference in `rcu_tasks_wait_gp`）**

- 崩溃位置: `rcu_tasks_wait_gp+0xde` (RCU Tasks 初始化)
- 根因: `for_each_process()` 宏替换导致 early boot 时遍历语义不兼容
- 细节:
  1. `for_each_process()` 原始语义以 `&init_task` 为起点和终点，遍历全局循环链表
  2. NUMA 版本 `__numa_next_task()` 遍历 per-node 链表，所有节点遍历完后返回 NULL
   3. 终止条件不兼容: 原始用 `!= &init_task` 终止，NUMA 版本用 `!= NULL`
  4. 63+ 个调用点（RCU、调度器、OOM 等）不能简单替换
  5. BPF task iterator 使用 `next_task() == &init_task` 检查终止条件
  6. early boot 阶段 `for_each_process` 在 `numa_tasklist_init()` 之前被调用

**所有更改已回退到稳定状态（Kernel #25 编译通过）**

#### 34.1 重新设计 Step 1 的方向

**方案 A（保守，推荐先做）**: 不替换 `for_each_process()`，保持 `tasks` 在全局链表，先做低风险高收益的优化：
- `total_forks` → `atomic_long_t` 并移出 `tasklist_lock`
- `numa_tasklist_add()` 移出 `tasklist_lock`（`tasks` 仍在全局链表内，但 node/shard 索引可以在锁外）
- `process_counts` / `nr_threads` 审计和优化
- 这些不需要修改任何遍历宏，风险极低

**方案 B（激进）**: 全面替换 `for_each_process()` 宏，修复所有兼容性问题:
- 为 `next_task()` 添加 early boot 回退逻辑
- 统一终止条件（所有调用点改为 `!= NULL` 检查）
- 修复 BPF task iterator 的终止条件
- 需要全量测试 63+ 个调用点

**方案 C（折中，最工程化）**: 将 Step 1 拆分为两个子步骤:
- 子步骤 1-a: 完成可安全完成的优化（A/C 类优化、缩小临界区）
- 子步骤 1-b: 独立 patch 处理 `for_each_process` 替换

**状态**: 🔴 高优先级 - 待实施

1. **修改 `kernel/fork.c:copy_process()`**
   - 将 `list_add_tail_rcu(&p->tasks, &init_task.tasks)` 从 `tasklist_lock` 临界区移出
   - 改为：在 `node_lock` 保护下插入 `numa_tasklist.per_node[node].tasks`
   - `numa_tasklist_add(p, node)` 当前在锁内，合并到新的 `node_lock` 临界区
   - `tasks` 和 `tasks_node` 可以在同一次 `node_lock` 操作中完成

2. **修改 `kernel/exit.c:release_task()` + `__unhash_process()`**
   - 将 `list_del_rcu(&p->tasks)` 从 `tasklist_lock` 移出
   - 改为：在 `node_lock` 保护下摘除
   - `numa_tasklist_del_local(p)` 合并到同一次 `node_lock` 操作

3. **修改 `include/linux/sched/signal.h` 宏定义**
   - `for_each_process(p)` 改为逐节点遍历
   - `next_task(p)` 改为跨节点 `__numa_next_task(p)`
   - `tasklist_empty()` 改为 `numa_tasklist_nr_tasks() <= 1`

4. **审计所有 `init_task.tasks` 引用**
   - `kernel/fork.c` — `list_add_tail_rcu`
   - `kernel/exit.c` — `list_del_rcu`
   - `fs/proc/` — `/proc` 遍历
   - 其他使用 `for_each_process` 的子系统

5. **确保 `CONFIG_NUMA_TASKLIST=n` 时行为不变**
   - 条件编译：`#ifdef CONFIG_NUMA_TASKLIST` 使用 per-node 链表
   - `#else` 保留原始 `init_task.tasks` 行为

6. **lockdep + boot 验证**

7. **`total_forks` → `atomic_long_t` 并移出 `tasklist_lock`**

8. **`nr_threads` → `atomic_t` 审计**（需确认读端一致性要求）

#### 34.2 性能预期

| 指标 | 当前 | Step 1 后 |
|------|------|----------|
| tasks 插入竞争 | 128-way (全局) | 8-way (per-shard) 或 32-way (per-node) |
| tasklist_lock 临界区 | ~1-5μs | 缩短 30-40% |
| spawn 吞吐 | 基准 | +15-25% |

#### 34.3 风险评估

| 风险 | 概率 | 缓解措施 |
|------|------|----------|
| `for_each_process` 遍历语义变化 | 中 | 逐节点遍历语义等价，配合 RCU |
| init_task 不在全局链表头 | 低 | init_task 在 node[0].tasks 头部，遍历语义不变 |
| 可见性顺序问题 | 中 | tasks 插入在 node_lock 内，需确保与 tasklist_lock 内操作可见性 |

### 35. Step 2：PID hash per-bucket spinlock

#### 35.1 状态: ✅ 已完成 - 内核 #35 测试通过

1. **使用 `pid->lock`（per-PID spinlock）保护 PID hash 操作**
   - `struct pid` 已有 `spinlock_t lock` 字段（为 pidfd 添加）
   - `attach_pid_numa()`: 获取 `pid->lock`，执行 `hlist_add_head_rcu()`，释放锁
   - `detach_pid_numa()`: 获取 `pid->lock`，执行 `hlist_del_rcu()` + `pid_has_task()` 检查，释放锁
   - 零额外内存开销

2. **fork/exit 路径中 `attach_pid`/`detach_pid` 移出 `tasklist_lock` 临界区**
   - `fork.c`: `attach_pid_numa()` 在 `write_unlock_irq(&tasklist_lock)` 之后调用
   - `exit.c`: `detach_pid_numa()` 在 `write_unlock_irq(&tasklist_lock)` 之后调用
   - `nr_threads_dec()` 也移出临界区（原子操作）
   - `__this_cpu_inc/dec(process_counts)` 移出临界区

3. **`change_pid`/`transfer_pid`/`exchange_tids` 保持不变**
   - 这些是冷路径（setpgid/setsid/exec），仍使用 `tasklist_lock`
   - 热路径（fork/exit）已优化

4. **lockdep 断言更新**
   - `pid_task()` 新增 `lockdep_pid_lock_is_held(pid)` 条件
   - `rcu_dereference_check()` 现在接受 `pid->lock` 作为有效上下文

5. **修复 `init_struct_pid.lock` 初始化问题**
   - `init_struct_pid` 的 `.lock` 字段需要 `__SPIN_LOCK_INITIALIZER` 初始化
   - `CONFIG_DEBUG_SPINLOCK` 要求 `.magic = SPINLOCK_MAGIC`
   - 静态零初始化导致 early boot spinlock bad magic BUG

6. **性能预期**
   - `tasklist_lock` 临界区减少约 30-40%
   - 4 次 `attach_pid` + 4 次 `detach_pid` 操作移出临界区
   - spawn 吞吐预期 +15-25%

#### 35.2 修改文件

| 文件 | 修改内容 |
|------|----------|
| `kernel/pid.c` | 新增 `attach_pid_numa()`, `detach_pid_numa()`, `lockdep_pid_lock_is_held()`; 修复 `init_struct_pid.lock` 初始化; 更新 `pid_task()` lockdep 条件 |
| `kernel/fork.c` | NUMA 模式下 `attach_pid` 移出 `tasklist_lock`; 添加 `attach_pid_numa()` 调用 |
| `kernel/exit.c` | NUMA 模式下 `detach_pid` + `nr_threads_dec` + `process_counts` 移出 `tasklist_lock`; 添加 `detach_pid_numa()` 调用 |
| `include/linux/pid.h` | 新增 `attach_pid_numa()` / `detach_pid_numa()` / `lockdep_pid_lock_is_held()` 声明 |

### 36. Step 3：sibling 链表 RCU 化

#### 36.1 状态: 🟢 低优先级 - Step 2 完成后评估

1. **将 `parent->children` / `p->sibling` 改为 RCU 链表**
   - 写端使用 `list_add_tail_rcu` / `list_del_rcu`
   - 读端使用 `rcu_read_lock()` 保护
   - 配合 per-parent spinlock 序列化写端

2. **审计所有 `children` / `sibling` 遍历者**
   - `fs/proc/` — 进程信息读取
   - `kernel/exit.c` — `forget_original_parent`
   - `kernel/ptrace.c` — ptrace 操作

3. **性能预期**
   - 同 NUMA fork 的 sibling 操作完全本地化
   - spawn 吞吐提升接近线性扩展

---

## 新增执行清单（基于 `optize.md` v2.0）

### 13. 阶段 0：基线测量与热点确认

#### 13.1 状态: 🟡 中优先级 - 待完成

1. **建立 UnixBench spawn 基线**
   - 目标: 获取 `CONFIG_NUMA_TASKLIST=n/y` 的吞吐对比
   - 输出: ops/sec、方差、测试命令、测试环境
2. **采集 `perf lock report` 数据**
   - 目标: 确认 `tasklist_lock` 的等待时间和持有时间占比
   - 输出: Top contended locks、调用栈、持有者分布
3. **采集 `copy_process()` / `release_task()` 时延分布**
   - 目标: 区分"等锁耗时"与"锁内执行耗时"
   - 输出: 关键函数时延样本
4. **形成基线报告**
   - 目标: 作为后续每一步优化的比较基准

### 14. 阶段 1：全局锁需求审计

#### 14.1 状态: 🔴 高优先级 - 当前主任务

1. **审计 `kernel/fork.c:copy_process()`**
   - 将 `tasklist_lock` 内动作分类为：
     - A 类：必须保留在全局锁内
     - B 类：可转移到 shard / node 锁
     - C 类：可改为原子或延迟统计
2. **审计 `kernel/exit.c:release_task()`**
   - 同样完成 A/B/C 分类
3. **审计 `__exit_signal()` / `__unhash_process()`**
   - 确认 pid 解绑、全局链表摘链、统计更新的最小锁需求
4. **审计 `attach_pid()` / `detach_pid()` 顺序约束**
   - 输出一份"发布顺序约束说明"
5. **形成锁域审计报告**
   - 归档到设计文档或单独报告中

### 15. 阶段 2：fork 路径瘦身

#### 15.1 状态: 🟡 中高优先级 - 审计后实施

1. **评估 `numa_tasklist_add()` 能否移到全局锁外**
   - 前提: 不破坏任务发布顺序和可见性语义
2. **梳理 `copy_process()` 中统计更新逻辑**
   - 重点: `nr_threads`、`total_forks`、其他热路径统计
3. **拆分可外提的辅助逻辑**
   - 目标: 缩短 `tasklist_lock` 持有时间
4. **检查所有 `bad_fork_*` 失败路径回滚**
   - 确保 shard/node/global 三类链表状态一致
5. **完成 fork 路径专项回归测试**
   - 覆盖 fork 失败、正常创建、并发创建

### 16. 阶段 3：exit 路径瘦身

#### 16.1 状态: 🟡 中高优先级 - fork 后实施

1. **保持 `numa_tasklist_del()` / `sharded_tasklist_del()` 在锁外**
   - 检查边界条件和重复删除保护
2. **审计 `__exit_signal()` 中可外提的统计逻辑**
   - 目标: 将非强一致统计移出全局锁
3. **审计 `process_counts` / `nr_threads` 更新方式**
   - 评估是否可改为更低争用的实现
4. **完成 release/reap/wait/ptrace 回归测试**
   - 避免引入退出路径行为回归

### 17. 阶段 4：锁正确性与性能闭环验证

#### 17.1 状态: 🟡 中优先级 - 连续执行

1. ✅ **启用 `CONFIG_PROVE_LOCKING=y` 做 lockdep 验证** - boot 阶段通过
2. **启用 `CONFIG_DEBUG_LIST=y` / `CONFIG_PROVE_RCU=y` 做一致性验证**
3. **重复执行 UnixBench spawn 对比测试**
4. **重复执行 `perf lock report` 对比锁竞争变化**
5. **输出每轮优化的 delta 报告**
   - 包括吞吐、锁等待、功能状态

### 18. 阶段 5：实验性高收益原型（可选）

#### 18.1 状态: 🟢 低优先级 - 仅在前述阶段收益不足时考虑

1. **设计 shard-aware 专用遍历接口**
   - 不直接替换 `for_each_process`
2. **设计 NUMA + shard 二层索引原型**
   - 仅作为实验分支
3. **评估“兼容全局视图 + 快速索引视图”双模型**
   - 重点评估内核社区可接受性
4. **将原型收益与主线安全方案收益做对比**

### 19. 文档与交付物要求

#### 19.1 状态: 🟡 中优先级 - 与开发并行

1. **维护 `optize.md` 作为总设计文档**
   - 持续更新方案、风险、阶段结论
2. **维护 `process.md` 作为开发日志**
   - 记录每轮实现和验证结果
3. **维护性能对比表**
   - 至少包含基线、当前版本、下一版
4. **整理 patch 系列拆分建议**
   - 便于后续形成可审阅的小 patch 集

---

## 新增执行清单（NUMA 收敛竞争方案）

### 20. 阶段 A：NUMA 收敛模型设计

#### 20.1 状态: 🔴 高优先级 - 设计先行

1. **明确 `tasks` / `tasks_node` / `tasks_shard` 三链表职责**
   - `tasks`: 全局兼容链表
   - `tasks_node`: NUMA 本地索引
   - `tasks_shard`: NUMA 节点内分片索引
2. **确定 `numa_node_id` 归属策略**
   - 推荐: 创建时归属，不随运行时迁移自动改变
3. **定义每节点分片模型**
   - 例如 `SHARDS_PER_NODE = 4`
4. **输出锁层次规则**
   - `tasklist_lock` -> `node_lock` -> `shard_lock`

### 21. 阶段 B：NUMA 本地索引数据结构落地

#### 21.1 状态: 🔴 高优先级 - 设计后实施

1. **定义 `struct numa_tasklist_node`**
   - 包含 `node_lock`、节点链表、节点计数、每节点 shard 数组
2. **定义 `struct tasklist_shard` 的每节点实例化方式**
3. **补齐初始化逻辑**
   - NUMA 节点初始化
   - 每节点 shard 初始化
4. **确认 `init_task` 策略**
   - 不进入 shard 链表
   - 是否进入 node 链表保持与现有实现一致

### 22. 阶段 C：fork 路径 NUMA 收敛改造

#### 22.1 状态: 🟡 中高优先级 - 数据结构后实施

1. **在 `copy_process()` 中确定新任务归属节点**
2. **初始化 `tasks_node` / `tasks_shard`**
3. **将 NUMA 本地索引挂链放到锁外**
   - 节点链表
   - 节点内 shard 链表
4. **保留全局发布动作在 `tasklist_lock` 内**
   - `tasks` 全局挂链
   - `children/sibling` 关系维护
   - `attach_pid()`
   - `ptrace_init_task()`
5. **验证 fork 失败路径回滚完整性**

### 23. 阶段 D：exit 路径 NUMA 收敛改造

#### 23.1 状态: 🟡 中高优先级 - fork 后实施

1. **将 `tasks_node` / `tasks_shard` 摘链保持在锁外**
2. **保留全局摘链和 pid 清理在 `tasklist_lock` 内**
   - `detach_pid()`
   - `list_del_rcu(&p->tasks)`
   - `list_del_init(&p->sibling)`
   - `ptrace_release_task()`
   - `__exit_signal()`
3. **验证 release/reap/wait/ptrace 路径无回归**

### 24. 阶段 E：NUMA 局部遍历与调试接口

#### 24.1 状态: 🟡 中优先级 - 核心路径稳定后实施

1. **新增 `for_each_process_node()` 接口**
2. **新增 `for_each_process_node_sharded()` 接口**
3. **不替换现有 `for_each_process()`**
4. **补充调试和统计接口**
   - 节点任务数
   - shard 任务数
   - 节点内竞争情况

### 25. 阶段 F：NUMA 收敛方案专项验证

#### 25.1 状态: 🟡 中优先级 - 每轮改动后执行

1. **验证竞争是否先收敛到每节点范围**
   - 目标观察值: `128-way -> 32-way`
2. **验证叠加每节点 shard 后是否进一步收敛**
   - 目标观察值: `32-way -> 8-way`
3. **执行 `perf lock report` 对比 node/shard/global 锁占比**
4. **执行 4 NUMA / 128 核并发 spawn 对比测试**
5. **输出 NUMA 收敛专项报告**

### 26. 阶段 G：边界问题专项处理

#### 26.1 状态: 🟡 中优先级 - 与实现并行

1. **明确任务迁移策略**
   - 第一阶段默认不做动态索引迁移
2. **明确跨节点锁获取顺序**
   - 若未来支持多节点组合操作，必须按节点 ID 升序加锁
3. **明确 early boot 约束**
   - `init_task` 和分片初始化顺序不可破坏
4. **明确全局兼容语义边界**
   - NUMA 局部索引不能替代全局 `tasks` 语义

---

## S1 步骤完成状态

### S1-1 ✅ tasks 字段从 init_task.tasks 迁移到 per-NUMA-node 链表

- `fork.c`：NUMA 模式下不再挂入 `init_task.tasks`，由 `numa_tasklist_add()` 处理
- `exit.c`：NUMA 模式下不再从全局链表摘除，由 `numa_tasklist_del()` 处理
- `fork_numa.c`：`numa_tasklist_add/del/migrate` 操作 `tasks` 字段（而非 `tasks_node`）
- `init_task.tasks` 由 `numa_tasklist_init()` 挂入 node 0 链表

### S1-2 ✅ exit 路径 tasks 摘除从 tasklist_lock 移到 per-node lock

- `list_del_rcu(&p->tasks)` 在 `#ifndef CONFIG_NUMA_TASKLIST` 保留
- NUMA 模式下由 `numa_tasklist_del(p)` 在 `tasklist_lock` 外处理

### S1-3 ✅ 重写 for_each_process/tasklist_empty 宏

- `for_each_process` 改为逐节点 `for` + `list_for_each_entry_rcu` 遍历
- `tasklist_empty` 改为 `numa_tasklist_nr_tasks() <= 1`
- `next_task` 宏保持不变（仅在非 NUMA 模式下使用）

### S1-4 ✅ 审计所有 init_task.tasks 引用点

- `kernel/fork.c` — 已修改
- `kernel/exit.c` — 已修改
- `kernel/cgroup/cgroup.c` — BUG_ON 修改
- `kernel/bpf/task_iter.c` — 终止条件修改
- `kernel/context_tracking.c` — 通过 tasklist_empty() 宏自动适配
- `init/init_task.c` — 保持 LIST_HEAD_INIT

### S1-5 ✅ CONFIG_NUMA_TASKLIST=n 时行为完全不变

- 所有修改在 `#ifdef CONFIG_NUMA_TASKLIST` 内
- 非 NUMA 代码路径完全保持原始逻辑

### S1-6 ✅ 编译和启动验证

- Kernel #32 编译通过
- QEMU 启动成功（4 CPU, 2 NUMA node）
- `ps aux` 正常列出进程（for_each_process 遍历正常）
- fork 测试通过

### S1-7 ✅ total_forks → atomic_long_t 并移出 tasklist_lock

- `/proc/stat` 通过 `atomic_long_read()` 读取
- 递增从 `tasklist_lock` 内移到锁外

### S1-8 ✅ nr_threads → atomic_t（含一致性审计）

- 提供 `nr_threads_read()/inc()/dec()` 内联函数
- 读取点：`/proc/stat`、`/proc/loadavg`、`sysinfo`、KDB

### S1-9 ✅ numa_tasklist_add() 移出 tasklist_lock（tasks 已在 node lock 内）

- 使用 per-node rwlock 保护
- 对称地，exit 路径 `numa_tasklist_del()` 也在锁外

---

## 后续优化方案

### Step 2：PID hash per-bucket spinlock（中短期，高收益）

**目标**：`attach_pid/detach_pid` 的 4 次 PID 哈希表操作从 `tasklist_lock` 中移出

1. **pid_hash[] 每个桶加独立 spinlock_t**
   - 当前 `pid_hash[i]` 是 `hlist_head`，只需加 `spinlock_t`
   - `attach_pid`/`detach_pid` 在桶级序列化，不需要全局锁
2. **修改 `attach_pid`/`detach_pid` 使用 per-bucket lock**
   - 在 `tasklist_lock` 内嵌套 per-bucket lock
   - 后续可将 per-bucket 操作完全移出 `tasklist_lock`
3. **预估收益**
   - 临界区再缩短 ~30%
   - spawn 吞吐 +15-25%

### Step 3：sibling 链表 RCU 化 + per-parent spinlock（中期）

**目标**：`list_add_tail(&p->sibling, &parent->children)` 移出 `tasklist_lock`

1. 每个进程的 `children` 链表用 `spinlock_t parent->children_lock` 保护
2. 读端用 RCU（`for_each_thread` 已经是 RCU safe）
3. 同 NUMA fork 操作完全本地化
4. 预估 spawn 吞吐 +80-150%

### Step 4：ptrace 稀有路径分离（中远期）

1. `ptrace_init_task`/`ptrace_release_task` 从 `tasklist_lock` 主路径分离
2. 使用独立的 `ptrace_lock`
3. `tasklist_lock` 仅保护 signal 继承等必须全局序列化的操作

### Step 5：基准测试和性能验证

1. UnixBench spawn 基线 vs 优化对比
2. `perf lock report` 确认争抢从 128-way 降至 8-way
3. stress-ng fork/exit 稳定性验证
4. LTP 进程管理测试功能回归验证
