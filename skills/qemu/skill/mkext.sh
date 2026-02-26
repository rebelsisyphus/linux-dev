#!/bin/bash
# 创建Ubuntu rootfs镜像

set -e

DISK_IMG="rootfs.img"
MOUNT_POINT="/mnt/rootfs"
DISK_SIZE="20G"
UBUNTU_VERSION="noble"  # Ubuntu 24.04

echo "Creating Ubuntu rootfs image..."
echo "This will take several minutes..."
echo ""

# 检查root权限
if [ "$(id -u)" -ne 0 ]; then
    echo "Error: This script must be run as root"
    echo "Usage: sudo $0"
    exit 1
fi

# 检查依赖
if ! command -v debootstrap &> /dev/null; then
    echo "Error: debootstrap not found"
    echo "Install with: apt-get install debootstrap"
    exit 1
fi

# 清理旧的镜像
if [ -f "$DISK_IMG" ]; then
    echo "Removing old $DISK_IMG..."
    rm -f "$DISK_IMG"
fi

# 创建磁盘镜像
echo "Creating $DISK_SIZE disk image..."
dd if=/dev/zero of="$DISK_IMG" bs=1 count=0 seek="$DISK_SIZE" status=progress

# 创建分区
echo "Creating partition..."
parted -s "$DISK_IMG" mklabel msdos
parted -s "$DISK_IMG" mkpart primary ext4 1MiB 100%
parted -s "$DISK_IMG" set 1 boot on

# 设置loop设备
echo "Setting up loop device..."
LOOP_DEV=$(losetup -f --show -P "$DISK_IMG")
echo "Loop device: $LOOP_DEV"

# 创建文件系统
echo "Creating ext4 filesystem..."
mkfs.ext4 "${LOOP_DEV}p1"

# 挂载
echo "Mounting..."
mkdir -p "$MOUNT_POINT"
mount "${LOOP_DEV}p1" "$MOUNT_POINT"

# 使用debootstrap创建基本系统
echo "Running debootstrap (this may take 10-20 minutes)..."
debootstrap --arch=amd64 "$UBUNTU_VERSION" "$MOUNT_POINT" http://archive.ubuntu.com/ubuntu/

# 配置系统
echo "Configuring system..."

# 设置主机名
echo "sisyphus" > "$MOUNT_POINT/etc/hostname"

# 配置hosts
cat > "$MOUNT_POINT/etc/hosts" << 'EOF'
127.0.0.1   localhost
127.0.1.1   sisyphus

# The following lines are desirable for IPv6 capable hosts
::1     ip6-localhost ip6-loopback
fe00::0 ip6-localnet
ff00::0 ip6-mcastprefix
ff02::1 ip6-allnodes
ff02::2 ip6-allrouters
EOF

# 安装必要的包
echo "Installing essential packages..."
chroot "$MOUNT_POINT" apt-get update
chroot "$MOUNT_POINT" apt-get install -y \
    openssh-server \
    netplan.io \
    iproute2 \
    iputils-ping \
    net-tools \
    systemd \
    systemd-sysv \
    linux-image-generic \
    grub-pc

# 启用systemd网络管理
chroot "$MOUNT_POINT" systemctl enable systemd-networkd
chroot "$MOUNT_POINT" systemctl enable systemd-resolved

# 生成SSH主机密钥
echo "Generating SSH host keys..."
chroot "$MOUNT_POINT" ssh-keygen -A

# 清理
echo "Cleaning up..."
chroot "$MOUNT_POINT" apt-get clean

# 卸载
umount "$MOUNT_POINT"
losetup -d "$LOOP_DEV"
rmdir "$MOUNT_POINT" 2>/dev/null || true

echo ""
echo "==================================="
echo "Rootfs image created successfully!"
echo "File: $DISK_IMG"
echo "Size: $DISK_SIZE"
echo ""
echo "Next steps:"
echo "1. Run: sudo ./setup-rootfs.sh"
echo "2. Run: ./qemu.sh"
echo "==================================="
