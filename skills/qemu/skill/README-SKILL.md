# QEMU测试环境 Skill

## 简介

本skill提供了一套完整的QEMU虚拟化测试环境，可以在任意新目录下快速部署具有以下特性的VM：

- ✅ **SSH免密码登录** - 直接使用 `ssh root@localhost -p 2222`
- ✅ **共享目录** - 宿主机与VM之间文件实时同步
- ✅ **Panic监控** - 自动检测并记录panic信息
- ✅ **一键测试** - 自动化测试执行和结果收集

## 安装方式

### 方式1：在新目录直接使用（推荐）

```bash
# 1. 创建新目录
mkdir ~/my-qemu-project
cd ~/my-qemu-project

# 2. 解压skill文件
tar -xzf /home/sisyphus/code/qemu/skill/qemu-test-env-skill-v1.0.0.tar.gz

# 3. 一键部署（需要root权限，耗时10-20分钟）
sudo ./install.sh

# 4. 准备内核镜像
cp /boot/vmlinuz-$(uname -r) ./bzImage

# 5. 启动VM
./qemu.sh

# 6. 等待30-40秒后连接
ssh root@localhost -p 2222
```

### 方式2：安装到opencode skills目录

```bash
# 1. 创建skill目录
mkdir -p ~/.config/opencode/skills/qemu-test-env

# 2. 解压skill文件
tar -xzf /home/sisyphus/code/qemu/skill/qemu-test-env-skill-v1.0.0.tar.gz \
  -C ~/.config/opencode/skills/qemu-test-env/

# 3. 在新目录使用skill
mkdir ~/my-qemu-project
cd ~/my-qemu-project
opencode /apply-skill qemu-test-env
```

## 文件清单

| 文件 | 大小 | 说明 |
|------|------|------|
| `install.sh` | 1.7KB | 一键部署脚本 |
| `qemu.sh` | 1.9KB | 启动QEMU VM |
| `monitor.sh` | 1.1KB | Panic监控 |
| `test.sh` | 3.0KB | 一键测试执行 |
| `mkext.sh` | 2.8KB | 创建20GB rootfs |
| `setup-rootfs.sh` | 3.3KB | 配置SSH和共享目录 |
| `README.md` | 2.4KB | 使用说明 |
| `SKILL_USAGE.md` | 5.8KB | 详细使用文档 |
| `skill.json` | 755B | skill元数据 |

**总计**: 约8.4KB（压缩后）

## VM配置

| 配置项 | 值 |
|--------|-----|
| OS | Ubuntu 24.04 LTS (Noble) |
| 磁盘 | 20GB ext4 |
| 用户名 | root |
| 密码 | 空（免密码） |
| SSH端口 | localhost:2222 → VM:22 |
| 共享目录 | ./shared ↔ /mnt/shared |
| 网络 | User模式 + NAT |
| 已安装工具 | sshd, vim, nano, curl, wget, htop, gcc, make, git |

## 使用示例

### 基本使用

```bash
# 1. 启动VM
./qemu.sh

# 2. 在另一个终端连接
ssh root@localhost -p 2222

# 在VM中执行命令
root@sisyphus:~# ls /mnt/shared
test.sh  test_result.txt

# 3. 停止VM
kill $(cat /tmp/qemu.pid)
```

### 运行测试

```bash
# 一键执行测试
./test.sh

# 输出：
# QEMU Test Automation Script
# ===========================
# 
# QEMU already running. ✅
# Waiting for SSH to be available...
# SSH is ready! ✅
# Mounting shared directory... ✅
# Running test script in VM...
# ===========================
# === QEMU VM Test ===
# Date: Fri Mar 28 10:00:00 UTC 2026
# Hostname: sisyphus
# Kernel: 7.0.0-rc5dcc-00079-g0138af2472df
# ...
# Status: PASSED ✅
```

### 自定义测试

```bash
# 在宿主机创建测试脚本
cat > shared/my-test.sh << 'EOF'
#!/bin/bash
echo "Running custom test..."
echo "CPU info:"
lscpu | grep "Model name"
echo "Memory:"
free -h | grep Mem
echo "Test passed!"
EOF
chmod +x shared/my-test.sh

# 在VM中执行
ssh root@localhost -p 2222 /mnt/shared/my-test.sh
```

## 工作原理

### 网络架构

```
宿主机                                    QEMU VM
┌─────────────┐                          ┌─────────────┐
│             │  SSH Port Forward        │             │
│  localhost  │  localhost:2222 ───────► │  VM:22      │
│  :2222      │                          │  (sshd)     │
│             │                          │             │
│  ./shared   │  9p virtio               │  /mnt/      │
│  (host dir) │  ───────────────────────►│  shared     │
│             │  Shared Directory        │  (VM dir)   │
│  serial.log │  Serial Port             │             │
│  (log file) │  ◄───────────────────────│  console    │
└─────────────┘                          └─────────────┘
```

### 启动流程

1. **创建rootfs** (`mkext.sh`)
   - 使用debootstrap创建Ubuntu基础系统
   - 安装必要包（sshd, netplan等）
   - 生成SSH主机密钥

2. **配置rootfs** (`setup-rootfs.sh`)
   - 配置SSH允许空密码登录
   - 清空root密码
   - 配置netplan网络（enp0s3接口）
   - 配置fstab自动挂载共享目录
   - 启用SSH服务

3. **启动VM** (`qemu.sh`)
   - 加载内核(bzImage)和rootfs(rootfs.img)
   - 配置端口转发(2222→22)
   - 配置9p共享目录
   - 启动VM

4. **连接使用**
   - SSH免密码登录
   - 共享目录自动同步

## 故障排查

### 安装阶段

```bash
# 检查依赖
which qemu-system-x86_64 debootstrap ss

# 安装依赖
sudo apt-get update
sudo apt-get install -y qemu-system-x86 debootstrap
```

### 启动阶段

```bash
# QEMU无法启动
pkill -9 qemu
rm -f /tmp/qemu.pid
./qemu.sh

# 检查日志
tail -f serial.log
```

### 连接阶段

```bash
# SSH连接超时
# 1. 等待更长时间（首次启动需要30-40秒）
sleep 40

# 2. 检查端口
ss -tlnp | grep 2222

# 3. 检查QEMU进程
ps aux | grep qemu

# 4. 查看串口日志
grep -i "ssh" serial.log
```

### 共享目录

```bash
# 共享目录未挂载
ssh root@localhost -p 2222 "mount -t 9p -o trans=virtio shared /mnt/shared"

# 检查挂载
ssh root@localhost -p 2222 "df -h | grep shared"
```

## 限制和注意事项

1. **磁盘空间** - rootfs.img为20GB，确保有足够空间
2. **首次启动** - 需要30-40秒初始化时间
3. **内核镜像** - 需要自行提供bzImage
4. **网络模式** - User模式，性能受限但配置简单
5. **共享目录** - 9p协议，适合测试不适合高性能IO

## 自定义配置

### 修改rootfs大小

编辑 `mkext.sh`：
```bash
DISK_SIZE="50G"  # 改为50GB
```

### 添加自定义包

编辑 `setup-rootfs.sh`：
```bash
chroot "$MOUNT_POINT" apt-get install -y \
    openssh-server \
    netplan.io \
    your-package  # 添加包名
```

### 修改SSH配置

编辑 `setup-rootfs.sh`，修改sshd_config部分。

### 自定义网络

编辑 `setup-rootfs.sh`，修改netplan配置。

## 版本历史

### v1.0.0 (2026-03-28)
- 初始版本
- Ubuntu 24.04 LTS支持
- SSH免密码登录
- 9p共享目录
- Panic监控
- 一键测试执行

## 许可

MIT License

## 作者

opencode

---

**Location**: `/home/sisyphus/code/qemu/skill/qemu-test-env-skill-v1.0.0.tar.gz`

**Size**: 8.4KB
