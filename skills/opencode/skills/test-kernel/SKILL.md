---
name: test-kernel
description: 自动化测试Linux内核镜像，启动QEMU虚拟机，验证SSH连接，执行测试脚本，输出测试结果
license: MIT
compatibility: opencode
metadata:
  category: development
  tags: [kernel, linux, qemu, testing, vm, automation]
---

## 功能

自动化内核测试工具，一键完成以下流程：
1. 清理历史测试记录和日志
2. 启动QEMU虚拟机加载指定内核
3. 验证内核成功启动
4. 建立SSH连接（免密码）
5. 挂载共享目录
6. 执行基础回归和 spawn 压测脚本
7. 输出测试结果

## 触发条件

当用户需要测试Linux内核时使用，包括：
- "测试内核" 或 "test kernel" 关键词
- 编译了新内核需要验证
- 需要检查内核启动和基本功能
- 自动化内核回归测试

## 使用方法

```bash
# 测试默认内核 (bzImage)
test-kernel

# 测试指定路径的内核
test-kernel ./arch/x86/boot/bzImage

# 测试自定义内核
test-kernel /path/to/custom/bzImage
```

## 测试流程

1. **环境清理**: 删除历史日志 (serial.log, qemu.log, panic.log等)
2. **停止旧实例**: 终止已运行的QEMU进程
3. **准备内核**: 复制内核镜像到QEMU目录
4. **启动VM**: 使用QEMU启动虚拟机
5. **监控panic**: 后台监控Kernel panic
6. **等待SSH**: 等待SSH服务就绪（通常30-60秒）
7. **执行测试**: 在VM中运行测试脚本集合
8. **输出结果**: 显示测试结果并保存到文件

## 默认测试集

`test-kernel` 默认执行以下用例：

1. `/home/sisyphus/code/test/test.sh`
   - 基础启动后回归
   - 检查 `fork/exec`、并发 `fork/exit`、`dmesg` 中的常见告警
2. `/home/sisyphus/code/test/spawn-test/test.sh`
   - spawn 压测
   - 用于覆盖更高频的进程创建/退出场景

如果其中任一用例失败，都应保留 `serial.log` 并检查 guest `dmesg` 以区分内核 panic、运行时告警和测试环境问题。

## VM配置

虚拟机默认配置：
- **架构**: x86_64 (q35芯片组)
- **NUMA节点**: 2个
- **CPU**: 8核 (每NUMA节点4核: cpus 0-3 在 node 0, cpus 4-7 在 node 1)
- **内存**: 2GB (每NUMA节点1GB)
- **网络**: 用户模式网络，SSH端口转发到主机的2222端口
- **存储**: rootfs.img作为系统盘 (IDE/SATA)
- **共享目录**: 9p virtio共享 `/home/sisyphus/code/test`

QEMU启动参数：
```bash
-machine q35 \
-object memory-backend-ram,id=mem0,size=1G \
-object memory-backend-ram,id=mem1,size=1G \
-smp 8,sockets=2,cores=4 \
-m 2G \
-numa node,memdev=mem0,cpus=0-3 \
-numa node,memdev=mem1,cpus=4-7
```

### NUMA拓扑

```
Node 0: CPUs 0-3, Memory 1G
Node 1: CPUs 4-7, Memory 1G
Total:   CPUs 0-7, Memory 2GB
```

## 依赖要求

- qemu-system-x86_64
- SSH客户端
- nc (netcat)
- 已配置的rootfs.img
- 已配置SSH免密登录

## 输出文件

- `/home/sisyphus/code/qemu/serial.log` - 串口日志
- `/home/sisyphus/code/qemu/qemu.log` - panic日志（如有）
- `/home/sisyphus/code/test/test_result.txt` - 基础测试结果
- `spawn-test` 标准输出 - spawn 压测结果

## 故障排查

如果测试失败：
1. 检查 `serial.log` 查看启动日志
2. 确认内核镜像存在且可启动
3. 检查rootfs.img是否完整
4. 验证SSH免密配置
5. 检查端口2222是否被占用

## 常用命令

```bash
# 查看实时日志
tail -f /home/sisyphus/code/qemu/serial.log

# 连接SSH
ssh root@localhost -p 2222

# 停止QEMU
kill $(cat /tmp/qemu.pid)
```
