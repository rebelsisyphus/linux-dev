# tasklist_lock 审计分析报告

**分析日期**: 2026-04-08
**分析范围**: `tasklist_lock` 在 fork/exit 热路径上的保护范围与替代可行性

---

## 1. 核心结论

**`tasklist_lock` 不能被 per-NUMA 分片锁替代。**

即使进程绑定到某个 NUMA 节点，fork/exit 路径仍必须持有全局 `tasklist_lock`，原因如下：

1. 进程树（parent/children/sibling）是**跨节点全局结构**
2. PID 哈希表是**全局数据结构**，无 NUMA 划分
3. ptrace/signal 关系是**跨进程组**的全局关系
4. `tasks` 链表头在 `init_task` 上，RCU 写端必须互斥

per-NUMA 分片锁（`tasks_node`/`tasks_shard`）的价值在于将**辅助索引维护**从全局锁中拆出，缩小临界区，而非替代全局锁。

---

## 2. tasklist_lock 保护范围完整审计

### 2.1 fork 路径（`copy_process` L2391-2482）

```
write_lock_irq(&tasklist_lock);
  ┌─ p->real_parent = current / current->real_parent     ← 全局进程树
  │  p->parent_exec_id = ...
  ├─ klp_copy_process(p)                                 ← 内核livepatch
  ├─ sched_core_fork(p)                                  ← 调度器核心
  ├─ spin_lock(&current->sighand->siglock)               ← 嵌套锁
  │  ├─ signal 继承
  │  ├─ ptrace_init_task(p, ...)                          ← 全局ptrace关系
  │  ├─ init_task_pid(p, PIDTYPE_PID/TGID/PGID/SID)      ← PID哈希表
  │  ├─ list_add_tail(&p->sibling, &p->real_parent->children) ← 跨节点
  │  ├─ list_add_tail_rcu(&p->tasks, &init_task.tasks)   ← 全局链表
  │  ├─ attach_pid(p, PIDTYPE_TGID)                      ← PID哈希表
  │  ├─ attach_pid(p, PIDTYPE_PGID)                      ← PID哈希表
  │  ├─ attach_pid(p, PIDTYPE_SID)                       ← PID哈希表
  │  ├─ __this_cpu_inc(process_counts)                   ← per-cpu统计
  │  ├─ nr_threads++                                     ← 全局计数
  │  └─ total_forks++                                    ← 全局计数
  └─ spin_unlock(&current->sighand->siglock)
write_unlock_irq(&tasklist_lock);
```

### 2.2 exit 路径（`release_task` + `__unhash_process`）

```
numa_tasklist_del(p);              ← 已在锁外（per-node lock）
sharded_tasklist_del(p);           ← 已在锁外（per-shard lock）

write_lock_irq(&tasklist_lock);
  ├─ ptrace_release_task(p)       ← 全局ptrace关系
  ├─ __exit_signal(&post, p)      ← 信号处理，跨进程组
  │   ├─ spin_lock(&sighand->siglock)
  │   ├─ sig->nr_threads--        ← 信号组统计
  │   ├─ __unhash_process()
  │   │   ├─ nr_threads--         ← 全局计数
  │   │   ├─ detach_pid(PIDTYPE_PID)   ← PID哈希表
  │   │   ├─ detach_pid(PIDTYPE_TGID)  ← PID哈希表
  │   │   ├─ detach_pid(PIDTYPE_PGID)  ← PID哈希表
  │   │   ├─ detach_pid(PIDTYPE_SID)  ← PID哈希表
  │   │   ├─ list_del_rcu(&p->tasks)   ← 全局链表摘除
  │   │   ├─ list_del_init(&p->sibling) ← parent->children摘除
  │   │   └─ list_del_rcu(&p->thread_node)
  │   └─ spin_unlock(&sighand->siglock)
  ├─ do_notify_parent()            ← 可能唤醒等待进程
  └─ zap_leader 处理
write_unlock_irq(&tasklist_lock);
```

### 2.3 其他 tasklist_lock 写端使用点

| 文件 | 函数 | 操作 | 是否NUMA本地 |
|------|------|------|:---:|
| `kernel/ptrace.c` | `ptrace_attach` | 建立tracer/tracee关系 | 否 |
| `kernel/ptrace.c` | `ptrace_traceme` | 建立trace关系 | 否 |
| `kernel/ptrace.c` | `ptrace_detach` | 断开trace关系 | 否 |
| `kernel/sys.c` | `ksys_setsid` | 修改session关系 | 否 |
| `kernel/exit.c` | `exit_notify` | 通知父进程/修改进程树 | 否 |
| `kernel/exit.c` | `wait_task_zombie` | 回收子进程 | 否 |
| `kernel/exit.c` | `find_child_reaper` | 找子进程reaper | 否 |

### 2.4 其他 tasklist_lock 读端使用点

| 文件 | 函数 | 操作 |
|------|------|------|
| `kernel/signal.c` | `kill_pgrp_info` | 发送进程组信号 |
| `kernel/signal.c` | `kill_something_info` | 发送信号 |
| `kernel/signal.c` | `ptrace_stop` | ptrace停止 |
| `kernel/signal.c` | `do_signal_stop` | 信号停止 |
| `kernel/signal.c` | `get_signal` | 获取信号 |
| `kernel/signal.c` | `exit_signals` | 退出信号处理 |
| `kernel/sched/core.c` | `normalize_rt_tasks` | 归一化RT任务 |
| `kernel/power/process.c` | `try_to_freeze_tasks` | 冻结任务 |
| `kernel/livepatch/transition.c` | 多个函数 | 内核热补丁 |
| `kernel/fork.c` | `walk_process_tree` | 遍历进程树 |

---

## 3. 操作分类：可否移到 per-NUMA 锁

### 3.1 A类：必须在 tasklist_lock 内（不可移出）

| 操作 | 原因 |
|------|------|
| `list_add_tail(&p->sibling, &p->real_parent->children)` | parent 可在任何节点，sibling 链表头在 parent 上 |
| `list_add_tail_rcu(&p->tasks, &init_task.tasks)` | 全局链表，RCU 写端互斥 |
| `list_del_rcu(&p->tasks)` | 全局链表摘除，RCU 写端互斥 |
| `list_del_init(&p->sibling)` | 从 parent->children 摘除 |
| `attach_pid / detach_pid (PIDTYPE_*)` | PID 哈希表是全局数据结构 |
| `ptrace_init_task / ptrace_release_task` | tracer/tracee 关系跨节点 |
| `__exit_signal` | 信号处理跨进程组 |
| `signal 继承 (shared_pending, tty, has_child_subreaper)` | 需要与 sighand->siglock 同步 |

### 3.2 B类：可移到 per-node/shard 锁（已实现）

| 操作 | 当前位置 | 状态 |
|------|---------|:---:|
| `numa_tasklist_add(p, node)` | 已移到锁内，可外提 | 🔄 待评估 |
| `sharded_tasklist_add(p)` | 已在锁外 | ✅ |
| `numa_tasklist_del(p)` | 已在锁外 | ✅ |
| `sharded_tasklist_del(p)` | 已在锁外 | ✅ |

### 3.3 C类：可改为 atomic/per-cpu（潜在优化）

| 操作 | 当前实现 | 优化方式 | 可行性 |
|------|---------|---------|:---:|
| `nr_threads++` | 全局变量，锁保护 | `atomic_inc` + 移出锁外 | ⚠️ 需审计读端 |
| `total_forks++` | 全局变量，锁保护 | `atomic_inc` + 移出锁外 | ✅ 读端不要求强一致 |
| `__this_cpu_inc(process_counts)` | per-cpu | 已无争用 | ✅ |
| `nr_threads--` (exit) | 全局变量，锁保护 | `atomic_dec` + 锁外 | ⚠️ 需审计读端 |

---

## 4. 为何 per-NUMA 分片锁不能替代 tasklist_lock

### 4.1 "逐个持有 4 个 NUMA 锁"方案的问题

假设 4 NUMA 节点，试图用 `node_lock[N]` 替代 `tasklist_lock`：

**问题1：进程树是跨节点的**

```
init_task (Node 0) ──┬── sshd (Node 1)
                      └── bash (Node 0)
                           └── make -j4 (Node 2)
                                ├── cc1 (Node 2)
                                └── cc1 (Node 3)
```

当 Node 3 上的 cc1 fork 时：
- `p->sibling` 必须插入到 Node 2 上 `make->children` 链表
- 需要持有 Node 2 的锁，而非 Node 3
- 更一般地，fork 时不知道需要锁哪几个节点

**问题2：逐个获取多把锁等价于全局屏障**

如果要求"所有相关节点的锁"，则：
- 需要按节点 ID 升序获取（防死锁）
- 等价于所有参与节点全局静止
- 比 `tasklist_lock` 开销更高

**问题3：PID 哈希表是全局的**

```
pid_hash[0] ──→ task_a (Node 0)
pid_hash[1] ──→ task_b (Node 2)
pid_hash[2] ──→ task_c (Node 1)
```

`attach_pid()` 插入全局哈希表，与 NUMA 无关。若将 PID 哈希也分片，则进程组/会话查询需遍历所有分片。

**问题4：读者需要一致的全局视图**

`for_each_process()` 遍历 `init_task.tasks` 时，RCU 保证读端无锁。但写端的 `list_add_tail_rcu()` 必须互斥——即使只持有一个节点的锁，其他 CPU 上并发的写端可能在修改同一个 `init_task.tasks` 链表头。

### 4.2 为何"绑定 NUMA 的进程"也不能跳过 tasklist_lock

"进程绑定到 NUMA 节点"只影响**运行时 CPU/内存亲和性**，不影响**进程树关系的全局性**：

1. 进程的 parent 可以在任何节点（由 fork 语义决定）
2. 进程的 session/group leader 可以在任何节点
3. ptrace 的 tracer 可以在任何节点
4. `init_task.tasks` 是全局唯一链表头，没有任何"NUMA 本地"版本

即使 parent 和 child 恰好在同一节点，其他 CPU 上的并发读者（`for_each_process`、`kill`、`/proc`）也在访问同一链表头，写端必须全局互斥。

---

## 5. 当前架构定位（正确）

```
tasklist_lock          → 全局进程树一致性（不可替代）
  ├─ 进程树关系        → sibling/children/parent
  ├─ PID命名空间       → attach_pid/detach_pid
  ├─ ptrace关系        → tracer/tracee
  ├─ 信号继承          → sighand->siglock嵌套
  └─ 全局链表          → init_task.tasks (for_each_process)

per-node lock          → tasks_node NUMA局部索引（已在锁外 ✓）
per-shard lock         → tasks_shard 分片索引（已在锁外 ✓）
```

这三把锁保护的是**不同层次的数据**：

| 层次 | 数据 | 锁 | 可否分片 |
|------|------|-----|:---:|
| 全局 | 进程树、PID、signal、ptrace | `tasklist_lock` | 否 |
| NUMA | `tasks_node` 链表 | per-node lock | 是（已实现） |
| 分片 | `tasks_shard` 链表 | per-shard lock | 是（已实现） |

---

## 6. 可行的优化路径

### 6.1 缩小 tasklist_lock 临界区（最实用）

将非必要操作从 `tasklist_lock` 内移出：

**当前临界区（fork）：**
```c
write_lock_irq(&tasklist_lock);
  p->real_parent = ...;              // A类：必须全局锁
  ptrace_init_task(...);              // A类：必须全局锁
  list_add_tail(&p->sibling, ...);    // A类：必须全局锁
  list_add_tail_rcu(&p->tasks, ...);  // A类：必须全局锁
  attach_pid(...);                    // A类：必须全局锁
  nr_threads++;                       // C类：可改 atomic
  total_forks++;                      // C类：可改 atomic + 移出
write_unlock_irq(&tasklist_lock);
```

**优化后临界区：**
```c
numa_tasklist_add_local(p, node);  // 已在锁外 ✓
sharded_tasklist_add(p);            // 已在锁外 ✓

write_lock_irq(&tasklist_lock);
  p->real_parent = ...;              // A类
  spin_lock(&current->sighand->siglock);
  ptrace_init_task(...);              // A类
  list_add_tail(&p->sibling, ...);   // A类
  list_add_tail_rcu(&p->tasks, ...);  // A类
  attach_pid(...);                    // A类
  __this_cpu_inc(process_counts);     // A类（per-cpu，无争用）
  spin_unlock(&current->sighand->siglock);
write_unlock_irq(&tasklist_lock);

// 移出锁外（atomic 或 per-cpu）
atomic_long_inc(&total_forks);       // C类
```

**预期收益**：临界区缩短 10-20%，spawn 吞吐提升约 5-15%。

### 6.2 `total_forks` 外提可行性分析

`total_forks` 仅用于统计（`/proc` 读端），不参与任何同步判据。可以改为 `atomic_long_t` 并移到 `tasklist_lock` 外。

**当前读端**：
```c
// kernel/fork.c
total_forks++;  // 在 tasklist_lock 内

// fs/proc/stat.c
for_each_possible_cpu(i)
    total += per_cpu(p, i);
// 不要求强一致性
```

**优化后**：
```c
// 写端（锁外）
atomic_long_inc(&total_forks);

// 读端
atomic_long_read(&total_forks);  // 或保留 per-cpu 统计
```

### 6.3 `nr_threads` 的特殊性

`nr_threads` 有**强一致性读端**：

```c
// kernel/fork.c L2476
nr_threads++;  // 在 tasklist_lock 写端

// kernel/fork.c L1965（copy_process 入口前）
if (data_race(nr_threads >= max_threads))
    goto bad_fork_cleanup_count;
```

虽然检查用了 `data_race()`（容忍非一致读），但 fork 路径需要合理的边界保护。可以改为 `atomic_t`，但仍需在 `tasklist_lock` 内递增以保证与 `attach_pid` 的顺序。

更彻底的优化是将检查改为：
```c
if (atomic_read(&nr_threads) >= max_threads)
    return -EAGAIN;
```
允许少量超限（无锁），然后在 `tasklist_lock` 内修正。

### 6.4 `numa_tasklist_add` 外提可行性

当前状态：
```c
write_lock_irq(&tasklist_lock);
  // ...
  list_add_tail_rcu(&p->tasks, &init_task.tasks);  // 全局链表
  numa_tasklist_add(p, current_numa_node());          // NUMA链表（在锁内）
  // ...
write_unlock_irq(&tasklist_lock);
```

**分析**：`numa_tasklist_add` 操作的是 per-node 链表（`tasks_node`），但存在以下约束：

1. **可见性顺序**：`tasks` 全局链表和 `tasks_node` NUMA链表必须同时可见，否则遍历者可能看到不一致状态
2. **RCU 语义**：`tasks_node` 的 `list_add_tail_rcu` 配合全局 `tasks` 的 RCU 发布，需要读者在 RCU read-side 内看到一致的视图

**结论**：如果 `for_each_process` 和 `for_each_process_numa` 需要一致视图，`numa_tasklist_add` 应保持在 `tasklist_lock` 内。如果 NUMA 遍历是独立的（不要求与全局遍历原子一致），可以外提。

---

## 7. 性能预期与路径规划

### 7.1 各方案性能预期（4 NUMA / 128 CPU）

| 方案 | fork锁竞争 | exit锁竞争 | 预期spawn提升 | 实施难度 |
|------|-----------|-----------|:---:|:---:|
| 原始实现 | 128-way | 128-way | 基准 | - |
| 当前（三链表接线） | 128-way（全局锁未拆） | 128-way | -5%~+5% | ✅ 完成 |
| 缩小全局锁临界区 | 128-way（时间缩短30-50%） | 同左 | +10%~+25% | 中 |
| total_forks/nr_threads atomic | 略微缩短 | 略微缩短 | +3%~+5% | 低 |
| per-NUMA 分片锁替代tasklist_lock | 不适用 | 不适用 | 不适用 | ❌ 不可行 |

### 7.2 推荐优化路径

```
当前状态 ──→ 缩小临界区（A/C类分类）──→ atomic计数外提 ──→ 测量瓶颈
   │                                      │                    │
   │                                      │                    ▼
   │                                      │          若全局锁仍占 >50%
   │                                      │          考虑进程树结构重构
   │                                      │          （超出当前patch范围）
   │                                      │
   └── 已完成：tasks_node/tasks_shard 外提
```

### 7.3 不可行路径（避免投入）

1. ❌ 用 per-NUMA 锁替代 `tasklist_lock` — 进程树跨节点，无法按 NUMA 分片
2. ❌ 用 per-PID 分片锁替代 `tasklist_lock` — sibling/children 链表头在 parent 上
3. ❌ 绑定 NUMA 的进程跳过 `tasklist_lock` — 其他 CPU 的读者仍需一致性
4. ❌ 逐个获取所有 NUMA 锁 — 等价于全局屏障，比 `tasklist_lock` 更差

---

## 8. 关键代码位置参考

| 文件 | 行号 | 说明 |
|------|------|------|
| `kernel/fork.c` | L2391-2482 | fork路径 tasklist_lock 临界区 |
| `kernel/exit.c` | L270-296 | release_task tasklist_lock 临界区 |
| `kernel/exit.c` | L135-155 | __unhash_process 全局摘链 |
| `kernel/exit.c` | L160-222 | __exit_signal 信号处理 |
| `kernel/fork.c` | L2376-2385 | NUMA/shard 索引（已外提） |

---

## 9. per-NUMA 进程链表替代 `init_task.tasks` 全局链表

### 9.1 核心思想

将 `tasks` 字段从全局 `init_task.tasks` 链表改为 per-NUMA-node 链表，消除 fork 路径上最热的缓存行争抢。

```
当前:  init_task.tasks → task1 → task2 → ... → taskN → (回 init_task)
       所有 fork 修改同一个链表头 → 128-way 竞争

提议:  node[0].tasks → init_task → task_a → task_b
       node[1].tasks → task_c → task_d
       node[2].tasks → task_e → task_f
       node[3].tasks → task_g → task_h
       fork 只持本节点锁 → 竞争从 128-way 降到 32-way (4节点)
       叠加 shard → 8-way
```

**这一改动利用现有 `tasks_node` 链表基础设施，核心变化是让 `tasks` 字段不再挂入 `init_task.tasks`，而是挂入 `numa_tasklist.per_node[node].tasks`。**

### 9.2 可行性分析

| 要素 | 结论 | 说明 |
|------|------|------|
| 消除 `init_task.tasks` 热点 | **可行且价值最大** | 这是最热的缓存行，128 CPU 全争抢同一个 `prev` 指针 |
| per-NUMA 锁保护 `tasks` 插入 | **可行** | `list_add_tail_rcu` 只需本节点锁序列化写端 |
| `for_each_process()` 兼容 | **可行** | 重定义宏，逐节点遍历，语义等价 |
| `tasklist_lock` 完全去除 | **不可行** | PID 哈希、sibling、ptrace 仍需全局序列化 |
| 缩短 `tasklist_lock` 临界区 | **可行，收益显著** | `tasks` 插入移出后临界区缩短约 30-40% |

### 9.3 `init_task.tasks` 缓存行争抢分析

`init_task.tasks` 是内核最热的缓存行之一：

```
每次 fork/exit:
  write_lock_irq(&tasklist_lock);      // 写端全局互斥
  list_add_tail_rcu(&p->tasks, &init_task.tasks);
                                        ^^^^^^^^^^^^
                                        每次 fork 修改 init_task 的 prev 指针
                                        128 个 CPU 争抢同一缓存行
每次 for_each_process():
  rcu_read_lock();
  for (p = &init_task; ...p->tasks...)  // 读端也在第一条上
```

移除这个操作后：
- **写端**：fork 只持 `node_lock[N]`（4 节点中 1 个，竞争 32-way→如用 shard 更低至 8-way）
- **读端**：`for_each_process()` 按 NUMA 节点遍历，每个节点的链表头在本地内存
- **缓存行争抢**：从 1 个全局热点 → 4 个 NUMA 本地链表头（或 4×4=16 个 shard 链表头）

### 9.4 fork 路径改造

**当前 fork 临界区：**
```c
write_lock_irq(&tasklist_lock);             // 全局锁
  list_add_tail(&p->sibling, &parent->children);  // 本地缓存行
  list_add_tail_rcu(&p->tasks, &init_task.tasks);  // ← 全局最热点！
  numa_tasklist_add(p, node);                     // 已在锁内
  attach_pid(p, PIDTYPE_TGID);                    // pid_hash
  attach_pid(p, PIDTYPE_PGID);                    // pid_hash
  attach_pid(p, PIDTYPE_SID);                     // pid_hash
  attach_pid(p, PIDTYPE_PID);                     // pid_hash
  nr_threads++;
  total_forks++;
write_unlock_irq(&tasklist_lock);
```

**改造后：**
```c
/* ① tasks 插入 — 移到 per-NUMA 锁（消除最热缓存行） */
int node = task_numa_node(p);
write_lock_irq(&numa_tasklist.per_node[node].node_lock);
  list_add_tail_rcu(&p->tasks, &numa_tasklist.per_node[node].tasks);
write_unlock_irq(&numa_tasklist.per_node[node].node_lock);

/* ② 进程树 + PID — 仍在 tasklist_lock 内（但已不含 tasks 插入） */
write_lock_irq(&tasklist_lock);
  spin_lock(&current->sighand->siglock);
    p->real_parent = current;
    ptrace_init_task(p, ...);
    list_add_tail(&p->sibling, &parent->children);  // 本地缓存行
    attach_pid(p, PIDTYPE_TGID);
    attach_pid(p, PIDTYPE_PGID);
    attach_pid(p, PIDTYPE_SID);
    attach_pid(p, PIDTYPE_PID);
    __this_cpu_inc(process_counts);
  spin_unlock(&current->sighand->siglock);
write_unlock_irq(&tasklist_lock);

/* ③ 统计 — atomic（锁外） */
atomic_long_inc(&total_forks);
atomic_inc(&nr_threads);
```

### 9.5 exit 路径改造

**当前 exit 临界区：**
```c
numa_tasklist_del(p);                  // 已在锁外
sharded_tasklist_del(p);              // 已在锁外

write_lock_irq(&tasklist_lock);
  ptrace_release_task(p);
  __exit_signal(&post, p);
    spin_lock(&sighand->siglock);
      __unhash_process();
        detach_pid(PIDTYPE_PID);         // pid_hash
        detach_pid(PIDTYPE_TGID);         // pid_hash
        detach_pid(PIDTYPE_PGID);         // pid_hash
        detach_pid(PIDTYPE_SID);          // pid_hash
        list_del_rcu(&p->tasks);           // ← 全局最热点！
        list_del_init(&p->sibling);        // 本地缓存行
    spin_unlock(&sighand->siglock);
write_unlock_irq(&tasklist_lock);
```

**改造后：**
```c
/* ① tasks 摘除 — 移到 per-NUMA 锁 */
int node = task_numa_node(p);
write_lock_irq(&numa_tasklist.per_node[node].node_lock);
  list_del_rcu(&p->tasks);
write_unlock_irq(&numa_tasklist.per_node[node].node_lock);

/* ② 进程树 + PID — 仍在 tasklist_lock 内 */
write_lock_irq(&tasklist_lock);
  ptrace_release_task(p);
  __exit_signal(&post, p);
    spin_lock(&sighand->siglock);
      detach_pid(PIDTYPE_PID);
      detach_pid(PIDTYPE_TGID);
      detach_pid(PIDTYPE_PGID);
      detach_pid(PIDTYPE_SID);
      list_del_init(&p->sibling);
    spin_unlock(&sighand->siglock);
write_unlock_irq(&tasklist_lock);

/* ③ 统计 — atomic（锁外） */
atomic_dec(&nr_threads);
```

### 9.6 `for_each_process()` 兼容性

**当前定义（`include/linux/sched/signal.h`）：**
```c
#define tasklist_empty() \
    list_empty(&init_task.tasks)

#define next_task(p) \
    list_entry_rcu((p)->tasks.next, struct task_struct, tasks)

#define for_each_process(p) \
    for (p = &init_task ; (p = next_task(p)) != &init_task ; )
```

**改造后（`CONFIG_NUMA_TASKLIST=y`）：**
```c
#define tasklist_empty() \
    (numa_tasklist_nr_tasks() <= 1)  /* 仅剩 init_task */

/* 单节点内遍历 */
#define __next_task_in_node(p, node) ({                       \
    struct task_struct *__next;                               \
    __next = list_next_or_null_rcu(                           \
        &numa_tasklist.per_node[(node)].tasks,                \
        &(p)->tasks, struct task_struct, tasks);              \
    __next;                                                    \
})

/* 跨节点遍历：当前节点末尾时跳到下一节点 */
#define next_task(p)                                           \
    __numa_next_task(p)

/* 完整遍历所有进程 */
#define for_each_process(p)                                    \
    for_each_numa_node(__node_iter)                            \
        list_for_each_entry_rcu((p),                           \
            &numa_tasklist.per_node[__node_iter].tasks, tasks)
```

### 9.7 init_task 处理

- `init_task`（PID=0）始终在 `node[0].tasks` 链表头部
- 其他节点的链表初始为空
- `init_task.tasks` 仍作为其 `tasks_node`/`tasks` 成员的连接器，但不再作为全局链表头

### 9.8 `tasklist_lock` 仍需保留的操作

即使移除 `tasks` 全局链表操作，以下操作仍需 `tasklist_lock`：

| 操作 | 原因 | 能否进一步优化 |
|------|------|:---:|
| `list_add_tail(&p->sibling, &parent->children)` | parent 可在任何节点 | 可 RCU 化 + per-parent 锁 |
| `attach_pid(PIDTYPE_*)` | PID 哈希表是全局数据结构 | 可 per-bucket spinlock |
| `ptrace_init_task` | tracer 可在任何节点 | 低频，不值得优化 |
| `signal 继承` | 跨进程组 | 不可优化 |
| `detach_pid(PIDTYPE_*)` | 同 fork 侧 | 可 per-bucket spinlock |
| `list_del_init(&p->sibling)` | 同 fork 侧 | 可 RCU 化 |

### 9.9 同 NUMA 场景的额外收益

```
场景：make -j128 在 Node 2 上大量 fork，所有子进程也在 Node 2

当前：所有 fork 持同一把 tasklist_lock
     Node 2 的 32 个 CPU 排队等一把全局锁 → 128-way 竞争

改造后（同 NUMA 场景）：
  ① tasks 插入：只持 node_lock[2] → 32-way（如用 shard → 8-way）
  ② tasklist_lock：仍全局，但临界区更短（不含 tasks 插入）
  ③ sibling 插入：修改 parent（Node 2 本地缓存行）→ 不跨节点
  ④ sighand->siglock：可能在本地内存 → 也本地
  ⑤ pid_hash：分散到不同桶 → 部分本地化

理论收益：
  - tasks 插入从 128-way → 8-way (per-shard)
  - sibling 完全本地（同 NUMA 时 parent 的 children 链表在本地缓存行）
  - tasklist_lock 持有时间缩短 30-40%
  - 如果后续 PID hash per-bucket 化，tasklist_lock 可降至 <100ns 级别
```

### 9.10 进一步优化路线图

```
Step 1 (立即):  tasks 字段从 init_task.tasks 全局链表移到 per-node 链表
                收益: 消除最热缓存行，fork 临界区缩短 30-40%
                风险: 需重写 for_each_process() 宏，影响面广

Step 2 (短期):  PID hash per-bucket spinlock 替代 tasklist_lock 的 PID 操作
                收益: attach_pid/detach_pid 不再全局互斥
                风险: 中等，需审计 pid_hash 数据结构

Step 3 (中期):  sibling 链表 RCU 化 + per-parent rwlock
                收益: 同 NUMA fork 的 sibling 操作完全本地化
                风险: 较高，children/sibling 遍历者众多

Step 4 (远期):  tasklist_lock 仅保护 ptrace/rare 操作 → 近似无锁 fork
                收益: spawn 接近线性扩展
                风险: 高，需大量测试验证
```

### 9.11 Step 1 实施要点

#### 数据结构变更

不需要新增 `task_struct` 字段。`tasks` 字段从全局链表连接器变为 per-node 链表连接器。

**当前：**
```c
struct task_struct {
    struct list_head tasks;    // → init_task.tasks (全局链表)
    ...
};
```

**改造后：**
```c
struct task_struct {
    struct list_head tasks;    // → numa_tasklist.per_node[node].tasks (per-node 链表)
    ...
};
```

#### fork 路径关键变更

```c
// 删除：list_add_tail_rcu(&p->tasks, &init_task.tasks);
// 替换为：list_add_tail_rcu(&p->tasks, &numa_tasklist.per_node[node].tasks);
//         在 node_lock 保护下（而非 tasklist_lock）
```

#### exit 路径关键变更

```c
// 删除：list_del_rcu(&p->tasks);  (原在 tasklist_lock 内)
// 替换为：list_del_rcu(&p->tasks); (在 node_lock 保护下，移到 tasklist_lock 外)
```

#### `for_each_process()` 重写

从单一全局链表遍历改为逐节点遍历。遍历语义完全等价（遍历所有进程），但不再依赖 `init_task.tasks` 作为链表头。

#### 需要审计的调用点

所有使用 `init_task.tasks`、`for_each_process()`、`next_task()`、`tasklist_empty()` 的代码：
- `include/linux/sched/signal.h` — 宏定义
- `kernel/fork.c` — fork 路径
- `kernel/exit.c` — exit 路径
- `kernel/pid.c` — PID 查找
- `fs/proc/` — /proc 遍历
- 其他使用 `for_each_process` 的子系统

#### `CONFIG_NUMA_TASKLIST=n` 兼容

当配置关闭时，`tasks` 仍挂入 `init_task.tasks`，所有宏定义不变。

---

**文档版本**: 1.1
**更新日期**: 2026-04-08