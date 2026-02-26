# NUMA 感知 tasklist_lock 优化项目

> 详细的开发进展见 `process.md`，待办事项见 `todo_list.md`。

## 文档结构

| 文件 | 说明 |
|------|------|
| `README.md` | 项目概览（本文） |
| `optize.md` | 优化分析与方案 |
| `todo_list.md` | 待办事项清单 |
| `process.md` | 开发进展日志 |
| `numa-tasklist-design.md` | 架构设计文档 |
| `analyse.md` | 锁审计分析 |
| `review.md` | 代码审查报告 |
| `timeout.md` | 过时方案与历史记录 |
| `0001-tasklist-lock.patch` | 当前补丁文件 |

## 当前状态（2026-04-09）

### ✅ 已完成

- **Step 1**：per-NUMA tasks 链表迁移
  - `tasks` 字段从 `init_task.tasks` 迁移到 per-NUMA-node 链表
  - `for_each_process` 重写为逐节点遍历
  - `total_forks`/`nr_threads` 转为 atomic 并移出 `tasklist_lock`
- **Step 2**：PID hash per-PID spinlock
  - `attach_pid_numa()`/`detach_pid_numa()` 使用 `pid->lock` 替代 `tasklist_lock`
  - fork/exit 热路径 PID 操作移出 `tasklist_lock` 临界区
  - 修复 `init_struct_pid.lock` 初始化（`CONFIG_DEBUG_SPINLOCK`）
- Kernel #35 编译和 QEMU 启动测试通过

### 🔄 进行中

- Step 3：sibling 链表 per-parent spinlock + RCU 化
- Step 4：ptrace 稀有路径分离
- 性能基准测试（UnixBench spawn）

### 📊 优化收益

| 阶段 | fork 锁竞争 | 临界区缩短 | spawn 预估 |
|------|-----------|-----------|:---:|
| 原始 | 128-way | - | 基准 |
| S1 完成 | tasks: 8-way | ~40% | +15-25% |
| S1+S2 | tasks + pid (per-PID lock) | ~60-70% | +30-50% |
| S1+S3 | 几乎本地化 | 极短 | +80-150% |

## 快速验证

```bash
# 编译
make -j$(nproc)

# 测试
test-kernel

# 检查启动日志
grep "NUMA-aware tasklist\|Sharded tasklist" /home/sisyphus/code/qemu/serial.log
```

## 关键修改文件

- `kernel/fork.c`、`kernel/exit.c` — fork/exit 路径优化
- `kernel/fork_numa.c` — NUMA tasklist 核心实现
- `kernel/pid.c` — per-PID spinlock（`attach_pid_numa`/`detach_pid_numa`）
- `include/linux/pid.h` — Step 2 函数声明和 lockdep 条件
- `include/linux/sched/signal.h` — `for_each_process` 重写
- `include/linux/sched/task_numa.h` — NUMA 遍历宏