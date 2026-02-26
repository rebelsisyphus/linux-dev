# 过时方案与历史记录

此文件保存已废弃的方案、过时的分析和历史记录。

---

## 1. 早期 for_each_process 替换尝试（已回退）

### 2026-04-08 第一次尝试

尝试将 `for_each_process()` 宏重写为逐节点遍历以支持 per-NUMA tasks 链表，但遇到终止条件不兼容问题：

**问题**：
- 原始 `for_each_process` 使用 `!= &init_task` 终止（循环链表）
- NUMA 版本使用 `!= NULL` 或链表头终止
- BPF task_iter 直接调用 `next_task()` 并检查 `== &init_task`
- 63+ 调用点需要全部兼容

**失败原因**：
- `next_task()` 宏仅遍历当前节点的下一个任务，无法跨节点
- 需要完整重写遍历逻辑才能支持 per-NUMA 链表
- BPF task_iter 的手动遍历逻辑与宏终止条件冲突

**回退**：所有 Step 1 更改回退到稳定状态（Kernel #25）

### 解决方案

在 2026-04-09 的第二次尝试中成功解决：
- `for_each_process` 重写为完整的两层循环（外层遍历节点，内层遍历任务）
- BPF task_iter 单独处理：NUMA 模式下使用 `numa_next_task()` 而非 `next_task()`
- `tasklist_empty` 改为计数器判断 `numa_tasklist_nr_tasks() <= 1`

---

## 2. 三链表架构演进历史

### 原始设计（2026-03）

```
tasks → init_task.tasks (全局链表)
tasks_node → NUMA 本地索引
tasks_shard → per-node shard 索引
```

### 演进（2026-04）

Step 1 完成后，`tasks` 字段从 `init_task.tasks` 迁移到 per-NUMA-node 链表，`tasks_node` 功能与 `tasks` 重叠。

**当前状态**：
- `tasks` → per-NUMA-node 链表（用于 `for_each_process` 遍历）
- `tasks_node` → 保留供 sharded lock 使用
- `tasks_shard` → per-node shard 链表

---

## 3. 旧版优化方案（已过时）

### 方案 A：保守方案

保留 `init_task.tasks` 作为全局链表头，仅将 `tasks_node` 作为 NUMA 索引。

**评估**：收益有限，`init_task.tasks` 缓存行仍是热点。

### 方案 B：激进方案

完全替换 `for_each_process`，解决所有 63+ 调用点的兼容性问题。

**评估**：工程量巨大，需要大量测试验证。

### 方案 C：折中方案（最终采用）

先完成安全优化（atomic 变量、外提统计），然后单独处理 `for_each_process` 重写。

**结果**：成功实施。

---

## 4. 历史问题列表（已解决）

### 1. `p->tasks` list_head 冲突
- **问题**：分片锁操作复用 `p->tasks`，与全局链表冲突
- **解决**：新增独立 `tasks_shard` 字段

### 2. 分片锁调用点未启用
- **问题**：`sharded_tasklist_add/del` 被注释
- **解决**：在 fork.c/exit.c 启用调用点

### 3. `sharded_next_task` 遍历字段错误
- **问题**：使用 `p->tasks` 而非 `p->tasks_shard`
- **解决**：修正为 `tasks_shard`

### 4. init_task 分片链表问题
- **问题**：PID=0 在 early boot 阶段不应加入分片链表
- **解决**：`sharded_tasklist_add` 跳过 PID=0

---

## 5. 早期审计分析（已整合到其他文档）

以下分析已整合到 `analyse.md` 和 `optize.md`：

- tasklist_lock 操作分类（A/B/C 类）
- PID hash 桶锁优化分析
- sibling RCU 化分析
- ptrace 稀有路径分析

---

## 6. 旧版性能预期（已更新）

以下预期来自早期分析，已在最新版文档中更新：

| 方案 | fork 锁竞争 | spawn 提升预估 |
|------|-----------|:---:|
| 当前实现 | 128-way (全局) | 基准 |
| S1 完成 | tasks: 8-way, 其余: 128-way | +15-25% |

最新预期（2026-04-09）：
- S1 完成：tasks 8-way + atomic 外提，临界区缩短 40%
- S1+S2：tasks 8-way + pid per-bucket，+30-50%
- S1+S3：几乎所有操作本地化，+80-150%

---

## 7. 已废弃的代码片段

### 旧版 numa_tasklist_add（已不使用）

```c
// 旧版：操作 tasks_node 字段
write_lock_irq(&ntl->lock);
list_add_tail_rcu(&p->tasks_node, &ntl->tasks);
```

### 新版（当前使用）

```c
// 新版：操作 tasks 字段
write_lock_irq(&ntl->lock);
list_add_tail_rcu(&p->tasks, &ntl->tasks);
```

---

**最后更新**：2026-04-09
**原因**：Step 1 完成，清理过时方案