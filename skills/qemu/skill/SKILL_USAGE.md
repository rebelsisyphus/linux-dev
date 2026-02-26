# QEMU测试环境 Skill 使用说明

## 概述

本skill包含了一套完整的QEMU测试环境配置文件，可以在新目录下快速部署具有SSH免密登录和共享目录功能的QEMU虚拟机。

## 安装到opencode

### 方法1：复制到opencode skills目录

```bash
# 1. 找到opencode skills目录
# 通常在 ~/.config/opencode/skills/ 或 /usr/share/opencode/skills/

# 2. 创建skill目录
mkdir -p ~/.config/opencode/skills/qemu-test-env

# 3. 复制skill文件
cp /home/sisyphus/code/qemu/skill/* ~/.config/opencode/skills/qemu-test-env/

# 4. 现在可以在新目录使用
mkdir ~/my-qemu-project
cd ~/my-qemu-project
opencode /apply-skill qemu-test-env
```

### 方法2：直接在新目录复制文件

```bash
# 1. 创建新目录
mkdir ~/my-qemu-project
cd ~/my-qemu-project

# 2. 复制skill文件
cp /home/sisyphus/code/qemu/skill/*.sh .
cp /home/sisyphus/code/qemu/skill/README.md .

# 3. 执行安装
sudo ./install.sh
```

## 使用流程

### 第一步：一键部署

```bash
sudo ./install.sh
```

这会：
- 创建20GB的Ubuntu 24.04 rootfs镜像
- 配置SSH免密码登录
- 配置共享目录自动挂载
- 安装常用工具

### 第二步：准备内核

```bash
# 方法1: 从当前系统复制
cp /boot/vmlinuz-$(uname -r) ./bzImage

# 方法2: 使用自定义内核
cp /path/to/your/kernel ./bzImage

# 方法3: 编译内核后复制
cp arch/x86/boot/bzImage ./bzImage
```

### 第三步：启动VM

```bash
./qemu.sh
```

等待30-40秒让VM启动完成。

### 第四步：连接VM

```bash
# SSH免密码登录
ssh root@localhost -p 2222

# 查看共享目录
ls /mnt/shared
```

## 文件说明

| 文件 | 用途 |
|------|------|
| `install.sh` | 一键部署脚本 |
| `qemu.sh` | 启动QEMU VM |
| `monitor.sh` | Panic监控 |
| `test.sh` | 一键测试执行 |
| `mkext.sh` | 创建rootfs镜像 |
| `setup-rootfs.sh` | 配置rootfs |
| `bzImage` | 内核镜像（用户需提供） |
| `rootfs.img` | rootfs镜像（自动生成） |
| `shared/` | 共享目录 |

## 高级用法

### 自定义rootfs大小

编辑 `mkext.sh`，修改 `DISK_SIZE` 变量：

```bash
DISK_SIZE="50G"  # 改为50GB
```

### 添加自定义包

编辑 `setup-rootfs.sh`，在 `apt-get install` 中添加包名：

```bash
chroot "$MOUNT_POINT" apt-get install -y \
    openssh-server \
    netplan.io \
    your-package-name  # 添加这里
```

### 修改SSH配置

编辑 `setup-rootfs.sh`，修改 SSH 配置部分：

```bash
cat > "$MOUNT_POINT/etc/ssh/sshd_config" << 'EOF'
# 你的自定义配置
EOF
```

### 自定义测试脚本

在 `shared/` 目录创建测试脚本：

```bash
cat > shared/my-test.sh << 'EOF'
#!/bin/bash
echo "My custom test"
# 你的测试代码
EOF
chmod +x shared/my-test.sh
```

然后执行：

```bash
./test.sh
```

## 故障排查

### 1. 安装失败

```bash
# 检查依赖
which qemu-system-x86_64 debootstrap

# 安装依赖（Ubuntu/Debian）
apt-get update
apt-get install -y qemu-system-x86 debootstrap
```

### 2. rootfs创建失败

```bash
# 清理并重试
sudo rm -f rootfs.img
sudo ./mkext.sh
```

### 3. SSH连接失败

```bash
# 检查QEMU是否运行
ps aux | grep qemu

# 检查端口
ss -tlnp | grep 2222

# 查看日志
tail -f serial.log
```

### 4. 共享目录不工作

```bash
# 在VM中手动挂载
ssh root@localhost -p 2222
mount -t 9p -o trans=virtio shared /mnt/shared
```

## 架构说明

```
宿主机                          QEMU VM
+--------+                     +------------------+
|        |  SSH:2222          |                  |
|  SSH   +------------------->|  SSHD:22         |
| Client |                     |                  |
+--------+                     +------------------+
|        |  9p virtio          |                  |
| shared |<------------------->|  /mnt/shared     |
| 目录   |  共享目录            |                  |
+--------+                     +------------------+
|        |  串口               |                  |
|serial.log<-------------------|  console         |
|        |                     |                  |
+--------+                     +------------------+
```

## 网络配置

- **模式**: QEMU user模式（SLIRP）
- **VM IP**: 10.0.2.15
- **网关**: 10.0.2.2
- **DNS**: 10.0.2.3
- **端口转发**: host:2222 -> VM:22

## 限制

1. **User模式网络** - VM可以访问外部网络，但外部不能直接访问VM（除了端口转发）
2. **性能** - 9p共享目录性能一般，适合测试不适合高性能IO
3. **rootfs大小** - 默认20GB，创建时需要足够磁盘空间

## 示例项目结构

```
my-qemu-project/
├── bzImage              # 内核镜像
├── rootfs.img           # 20GB rootfs
├── qemu.sh              # 启动脚本
├── monitor.sh           # 监控脚本
├── test.sh              # 测试脚本
├── mkext.sh             # 创建rootfs
├── setup-rootfs.sh      # 配置rootfs
├── install.sh           # 一键安装
├── shared/              # 共享目录
│   ├── test.sh          # 测试脚本
│   └── my-code/         # 你的代码
├── serial.log           # 串口日志
└── qemu.log             # Panic日志
```

## 更新日志

### v1.0.0
- 初始版本
- 支持Ubuntu 24.04
- SSH免密码登录
- 9p共享目录
- Panic监控
- 一键测试执行
