---
name: kernel-dev-loop
description: Linux内核迭代开发循环，自动执行编译验证、内核测试、Panic分析、报告生成四阶段流程，支持连续模式自动重试
license: MIT
compatibility: opencode
metadata:
  category: development
  tags: [kernel, linux, build, testing, automation, loop, panic]
---

# Skill: kernel-dev-loop

## 名称

Kernel Development Iteration Loop - Linux 内核迭代开发循环

## 描述

本 skill 提供标准化的 Linux 内核迭代开发工作流程，通过 4 阶段循环（编译→测试→分析→报告）确保代码质量，自动处理编译错误和内核 panic。

## 触发条件

当用户提到以下关键词时自动触发：
- **迭代开发**, kernel dev loop, 开发循环
- 编译并测试内核修改
- 处理内核 panic 并修复
- 开发内核新功能（如 NUMA tasklist）
- 需要自动化开发流程

## 使用方法

### 基本命令

```bash
# 运行完整开发循环
kernel-dev-loop

# 连续模式（失败后自动重试）
kernel-dev-loop -c

# 显示帮助
kernel-dev-loop -h
```

### 工作流程

本 skill **专注于开发流程管理**，实际测试完全复用 `test-kernel` skill：

```
kernel-dev-loop (本 skill)          test-kernel (复用)
        │                                  │
        ├── 编译验证 (make)                ├── 环境清理
        │                                  ├── 镜像准备
        ├── 调用 test-kernel ─────────────▶├── QEMU启动
        │                                  ├── SSH检测
        ├── Panic 分析                     ├── 测试执行
        │                                  
        └── 修复建议                       
```

### 4阶段流程

**Phase 1: 编译验证**
```bash
make -j$(nproc)
```
- 检查 `bzImage` 是否生成
- 分析编译错误

**Phase 2: 测试内核** (复用 test-kernel)
```bash
test-kernel arch/x86/boot/bzImage
```
- QEMU 启动
- SSH 检测
- 测试执行

**Phase 3: Panic 分析**
- 分析 `serial.log` 和 `qemu.log`
- 分类 panic 类型
- 提供修复建议

**Phase 4: 报告生成**
- 迭代状态
- 修复建议
- 下一步行动

## 常见 Panic 类型及修复

### Type A: No working init found
**症状**: 
```
Kernel panic - not syncing: No working init found
```

**根因**: `for_each_process` 遍历失败（tasks链表被禁用）

**修复**:
```c
// fork.c - 双链表维护
list_add_tail_rcu(&p->tasks, &init_task.tasks);     // 保持兼容
numa_tasklist_add(p, current_numa_node());           // 新增优化

// exit.c - 恢复删除
list_del_rcu(&p->tasks);
```

### Type B: Kernel Oops
**症状**: `Unable to handle kernel NULL pointer dereference`
**修复**: 检查空指针和数据结构初始化

### Type C: Lockdep Warning
**症状**: `possible recursive locking detected`
**修复**: 检查锁层次结构和获取顺序

## 配置

### 环境变量
```bash
MAX_ITERATIONS=10     # 最大迭代次数
```

### 依赖
- `test-kernel` skill（必须）
- `make`, `gcc`

## 开发规则

1. **CONFIG 隔离**: 新代码使用 `#ifdef CONFIG_NUMA_TASKLIST`
2. **保持兼容性**: 不破坏 `for_each_process` 等现有宏
3. **完全委托**: 不复做 `test-kernel` 的任何操作

## 示例

### 标准开发流程
```bash
$ kernel-dev-loop
[INFO] === Phase 1: Compile Verification ===
[SUCCESS] Kernel compiled: arch/x86/boot/bzImage (15M)
[INFO] === Phase 2: Kernel Testing (via test-kernel skill) ===
[ERROR] test-kernel failed
[INFO] === Phase 3: Panic Analysis ===
[ERROR] Kernel panic detected!
[WARNING] [PANIC TYPE A] 'No working init found'
[INFO] Fix: Restore tasks list operations

# 修复代码后再次运行
$ kernel-dev-loop
[SUCCESS] ALL PHASES COMPLETED!
```

### 连续模式
```bash
# 自动重试直到成功
$ kernel-dev-loop -c
[INFO] Iteration 1/10 ...（失败，等待修复）
[INFO] Iteration 2/10 ...（成功）
[SUCCESS] ALL PHASES COMPLETED!
```

## 故障排除

**Q: test-kernel 未找到？**  
A: 确保 test-kernel skill 已安装

**Q: 编译通过但启动失败？**  
A: 检查 Phase 3 的 panic 分析，常见是链表兼容性问题

**Q: 迭代多少次正常？**  
A: 复杂功能通常 3-5 次迭代

## 文件位置

- **Skill**: `/root/.config/opencode/skills/kernel-dev-loop/SKILL.md`
- **脚本**: `/usr/local/bin/kernel-dev-loop`
- **源码**: `/home/sisyphus/code/linux/scripts/kernel-dev-loop.sh`

## 相关 Skill

- `test-kernel`: 内核测试（本 skill 依赖）
