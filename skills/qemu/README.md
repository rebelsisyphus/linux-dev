# QEMU测试环境使用说明

## 已实现的功能

根据`qemu.md`的要求，已实现以下5个目标：

1. **Panic监控** - 自动监控串口日志，发生panic时保存上下文到`qemu.log`
2. **SSH访问** - 物理机可通过`localhost:2222` SSH到虚拟机
3. **常用工具** - VM中已安装gcc、ip、ls等调测工具
4. **共享目录** - `/home/sisyphus/code/test` 与VM中的 `/mnt/shared` 共享
5. **一键测试** - 物理机一键执行VM中的`test.sh`并获取结果

## 核心脚本

### 1. qemu.sh - 启动QEMU VM
```bash
./qemu.sh
```
- 启动虚拟机，后台运行
- 配置网络（SSH端口2222转发）
- 挂载共享目录
- 串口输出重定向到`serial.log`
- PID保存到`/tmp/qemu.pid`

### 2. monitor.sh - Panic监控
```bash
./monitor.sh [serial.log] [qemu.log]
```
- 监控串口日志中的panic关键字
- 发现panic时保存上下文（panic行±200行）到`qemu.log`
- PID保存到`/tmp/panic_monitor.pid`

### 3. test.sh - 一键测试执行
```bash
./test.sh
```
- 检查QEMU运行状态（如未运行则自动启动）
- 启动panic监控
- 等待SSH可用
- 自动挂载共享目录到VM
- 在VM中执行`/home/sisyphus/code/test/test.sh`
- 获取并显示测试结果
- 检查是否有panic发生

## 快速开始

### 方法一：分步执行

```bash
# 1. 启动QEMU
./qemu.sh

# 2. 在另一个终端启动panic监控
./monitor.sh

# 3. 等待VM启动（约20-30秒）

# 4. 手动SSH到VM（可选）
ssh -p 2222 root@localhost
# 密码: root

# 5. 在VM中执行测试
mkdir -p /mnt/shared
mount -t 9p -o trans=virtio shared /mnt/shared
cd /mnt/shared && ./test.sh
```

### 方法二：一键测试（推荐）

```bash
# 直接执行测试脚本，自动完成所有步骤
./test.sh
```

## 测试流程

1. **创建测试脚本**（已预置示例）
   ```bash
   # 在共享目录中创建或编辑测试脚本
   vim /home/sisyphus/code/test/test.sh
   ```

2. **执行测试**
   ```bash
   ./test.sh
   ```

3. **查看结果**
   - 测试结果：`/home/sisyphus/code/test/test_result.txt`
   - 串口日志：`serial.log`
   - Panic日志：`qemu.log`（如有panic）

4. **停止测试**
   ```bash
   # 停止QEMU
   kill $(cat /tmp/qemu.pid)
   
   # 停止panic监控
   kill $(cat /tmp/panic_monitor.pid)
   ```

## 目录结构

```
/home/sisyphus/code/qemu/
├── qemu.sh          # 启动QEMU
├── monitor.sh       # panic监控
├── test.sh          # 一键测试
├── qemu.md          # 需求文档
├── bzImage          # 内核镜像
├── rootfs.img       # 根文件系统
├── serial.log       # 串口日志（运行时生成）
└── qemu.log         # panic日志（如有panic时生成）

/home/sisyphus/code/test/
├── test.sh          # 测试脚本
└── test_result.txt  # 测试结果（运行时生成）
```

## VM配置信息

- **OS**: Ubuntu 24.04 LTS (Noble)
- **用户名**: root
- **密码**: root
- **SSH**: localhost:2222 -> VM:22
- **共享目录**: /home/sisyphus/code/test <-> /mnt/shared
- **已安装工具**: gcc, ip, ls, free, df, sshd, busybox等

## 故障排查

### 1. QEMU无法启动
```bash
# 检查是否有之前的进程残留
pkill -9 qemu
rm -f /tmp/qemu.pid /tmp/panic_monitor.pid
```

### 2. SSH无法连接
```bash
# 检查端口转发是否正常
nc -zv localhost 2222

# 检查VM网络状态
tail -f serial.log
```

### 3. 共享目录无法挂载
```bash
# 在VM中手动挂载
mkdir -p /mnt/shared
mount -t 9p -o trans=virtio shared /mnt/shared
```

### 4. 测试脚本找不到
```bash
# 确保测试脚本在共享目录中
ls -la /home/sisyphus/code/test/
```

## 测试示例

预置的`test.sh`会输出：
- 系统信息
- 内存使用情况
- 磁盘使用情况
- 共享目录内容
- 可用工具版本
- 测试结果状态

## 注意事项

1. VM启动需要20-30秒时间
2. SSH连接在VM启动后可能还需要10-15秒初始化
3. 共享目录使用9p virtio协议，确保QEMU启动参数中包含`-virtfs`
4. panic监控使用`tail -f`实时监控，会占用少量资源
5. 所有脚本都使用PID文件来管理进程，请勿手动kill -9

## 技术细节

- **QEMU版本**: 8.2.2
- **网络模式**: user模式 + 端口转发
- **文件系统共享**: 9p virtio
- **panic检测**: 基于串口日志的关键字监控
- **测试自动化**: 使用SSH + 9p挂载实现物理机到VM的测试执行
