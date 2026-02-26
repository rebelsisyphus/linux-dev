#!/bin/bash
# 配置rootfs镜像（SSH免密登录、共享目录、网络等）

set -e

DISK_IMG="rootfs.img"
MOUNT_POINT="/mnt/rootfs"

echo "Configuring rootfs image..."
echo ""

# 检查root权限
if [ "$(id -u)" -ne 0 ]; then
    echo "Error: This script must be run as root"
    echo "Usage: sudo $0"
    exit 1
fi

# 检查镜像文件
if [ ! -f "$DISK_IMG" ]; then
    echo "Error: $DISK_IMG not found!"
    echo "Run: sudo ./mkext.sh"
    exit 1
fi

# 设置loop设备
echo "Mounting rootfs..."
LOOP_DEV=$(losetup -f --show -P "$DISK_IMG")
echo "Loop device: $LOOP_DEV"

mkdir -p "$MOUNT_POINT"
mount "${LOOP_DEV}p1" "$MOUNT_POINT"

# 1. 配置SSH允许root登录和空密码
echo "Configuring SSH..."
cat > "$MOUNT_POINT/etc/ssh/sshd_config" << 'EOF'
Include /etc/ssh/sshd_config.d/*.conf

Port 22
ListenAddress 0.0.0.0

PermitRootLogin yes
PasswordAuthentication yes
PermitEmptyPasswords yes
AuthenticationMethods none

KbdInteractiveAuthentication no
UsePAM yes
X11Forwarding yes
PrintMotd no
AcceptEnv LANG LC_*
Subsystem	sftp	/usr/lib/openssh/sftp-server
EOF

# 2. 清空root密码
echo "Clearing root password..."
chroot "$MOUNT_POINT" passwd -d root

# 3. 配置网络（netplan）
echo "Configuring network..."
mkdir -p "$MOUNT_POINT/etc/netplan"
cat > "$MOUNT_POINT/etc/netplan/00-installer-config.yaml" << 'EOF'
network:
  version: 2
  ethernets:
    enp0s3:
      dhcp4: true
      dhcp6: true
EOF

# 4. 配置fstab（自动挂载共享目录）
echo "Configuring fstab..."
cat > "$MOUNT_POINT/etc/fstab" << 'EOF'
# /etc/fstab: static file system information.
#
# <file system> <mount point>   <type>  <options>       <dump>  <pass>
/dev/sda / ext4 errors=remount-ro 0 1
shared /mnt/shared 9p trans=virtio,version=9p2000.L 0 0
EOF

# 5. 启用SSH服务
echo "Enabling SSH service..."
mkdir -p "$MOUNT_POINT/etc/systemd/system/multi-user.target.wants"
ln -sf /usr/lib/systemd/system/ssh.service "$MOUNT_POINT/etc/systemd/system/multi-user.target.wants/"

mkdir -p "$MOUNT_POINT/etc/systemd/system/sockets.target.wants"
ln -sf /usr/lib/systemd/system/ssh.socket "$MOUNT_POINT/etc/systemd/system/sockets.target.wants/"

# 6. 创建共享目录挂载点
mkdir -p "$MOUNT_POINT/mnt/shared"

# 7. 安装常用工具
echo "Installing additional tools..."
chroot "$MOUNT_POINT" apt-get update
chroot "$MOUNT_POINT" apt-get install -y \
    vim \
    nano \
    curl \
    wget \
    htop \
    tree \
    git \
    build-essential \
    gcc \
    make \
    2>/dev/null || echo "Some packages may not be available"

chroot "$MOUNT_POINT" apt-get clean

# 8. 创建测试脚本目录
mkdir -p "$MOUNT_POINT/mnt/shared"

# 9. 设置时区
chroot "$MOUNT_POINT" ln -sf /usr/share/zoneinfo/UTC /etc/localtime

# 10. 配置locale
chroot "$MOUNT_POINT" locale-gen en_US.UTF-8 2>/dev/null || true

# 卸载
echo "Unmounting..."
umount "$MOUNT_POINT"
losetup -d "$LOOP_DEV"
rmdir "$MOUNT_POINT" 2>/dev/null || true

echo ""
echo "==================================="
echo "Rootfs configuration complete!"
echo ""
echo "Features configured:"
echo "  - SSH root login with empty password"
echo "  - DHCP network (interface: enp0s3)"
echo "  - Shared directory auto-mount (/mnt/shared)"
echo "  - Essential tools installed"
echo ""
echo "Next step: ./qemu.sh"
echo "==================================="
