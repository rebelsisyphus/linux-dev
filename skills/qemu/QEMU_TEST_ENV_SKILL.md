# QEMU测试环境部署指南

## 技能概述

本技能用于在新目录下快速创建一套完整的QEMU测试环境，包含：
- 自动化QEMU VM启动
- SSH免密码登录
- 共享目录（宿主机与VM之间）
- Panic监控
- 一键测试执行

## 依赖条件

- Linux系统（Ubuntu/Debian推荐）
- root权限
- 已安装QEMU (qemu-system-x86_64)
- 已安装debootstrap（创建rootfs用）

## 使用流程

### 1. 初始化工作目录

```bash
# 创建新的工作目录
mkdir -p /path/to/new/qemu-project
cd /path/to/new/qemu-project

# 运行skill创建所有必要文件
opencode /apply-skill qemu-test-env
```

### 2. 创建rootfs镜像（首次使用）

```bash
# 创建20GB的Ubuntu 24.04 rootfs（需要root权限）
sudo ./mkext.sh

# 这会创建一个20GB的rootfs.img文件
```

### 3. 配置SSH和共享目录

```bash
# 挂载rootfs并配置SSH免密登录和共享目录
sudo ./setup-rootfs.sh
```

### 4. 启动QEMU并测试

```bash
# 启动QEMU VM
./qemu.sh

# 等待约30-40秒VM启动完成

# 测试SSH连接
ssh root@localhost -p 2222

# 测试共享目录
ls /mnt/shared
```

## 文件结构

```
.
├── qemu.sh              # 启动QEMU VM
├── monitor.sh           # Panic监控
├── test.sh              # 一键测试执行
├── mkext.sh             # 创建rootfs镜像
├── setup-rootfs.sh      # 配置rootfs（SSH、共享目录等）
├── bzImage              # 内核镜像（需要用户提供或下载）
├── rootfs.img           # 根文件系统（由mkext.sh创建）
├── serial.log           # 串口日志（运行时生成）
└── qemu.log             # Panic日志（如有panic时生成）
```

## 网络配置

- **SSH端口**: localhost:2222 → VM:22
- **共享目录**: 宿主机`./shared` <-> VM `/mnt/shared`
- **VM网络**: user模式，支持NAT

## VM配置

- **OS**: Ubuntu 24.04 LTS (Noble)
- **用户名**: root
- **密码**: 空（免密码登录）
- **磁盘**: 20GB ext4
- **内存**: 默认QEMU配置

## 故障排查

### QEMU无法启动
```bash
pkill -9 qemu
rm -f /tmp/qemu.pid
```

### SSH连接失败
```bash
# 检查QEMU是否运行
ps aux | grep qemu

# 检查端口
ss -tlnp | grep 2222

# 查看日志
tail -f serial.log
```

### 共享目录未挂载
```bash
# 在VM中手动挂载
mount -t 9p -o trans=virtio shared /mnt/shared
```

## 注意事项

1. **rootfs.img 20GB** - 确保磁盘空间充足
2. **首次启动需要30-40秒** - 请耐心等待VM启动完成
3. **共享目录使用9p virtio** - 确保QEMU支持virtfs
4. **Ctrl+A X** - QEMU nographic模式退出热键
