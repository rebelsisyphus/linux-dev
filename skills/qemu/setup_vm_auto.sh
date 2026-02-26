#!/bin/bash
# 配置VM环境：安装SSH、常用工具

set -e

ROOTFS="/home/sisyphus/code/qemu/rootfs.img"
MNT="/mnt/rootfs"

echo "Mounting rootfs..."
mkdir -p "$MNT"
sudo mount -o loop "$ROOTFS" "$MNT"

echo "Installing packages in VM..."

# 更新软件源并安装核心包
sudo chroot "$MNT" bash -c '
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq
    apt-get install -y --no-install-recommends openssh-server gcc build-essential iproute2 iputils-ping coreutils util-linux procps net-tools
' || true

# 配置SSH
sudo chroot "$MNT" bash -c '
    echo "root:root" | chpasswd
    sed -i "s/#PermitRootLogin.*/PermitRootLogin yes/" /etc/ssh/sshd_config 2>/dev/null || true
    sed -i "s/PasswordAuthentication no/PasswordAuthentication yes/" /etc/ssh/sshd_config 2>/dev/null || true
    mkdir -p /mnt/shared /run/sshd /var/run/sshd
'

echo "Unmounting rootfs..."
sudo umount "$MNT" 2>/dev/null || sudo umount -f "$MNT" 2>/dev/null || true

echo "VM setup complete!"
