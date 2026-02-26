#!/bin/bash
# QEMU启动脚本 - 支持网络、共享目录、串口日志

set -e

# 配置
KERNEL="${1:-./bzImage}"
DISK="rootfs.img"
SERIAL_LOG="serial.log"
QEMU_LOG="qemu.log"
SHARED_DIR="/home/sisyphus/code/test"
PID_FILE="/tmp/qemu.pid"
VM_IP="10.0.2.15"
HOST_IP="10.0.2.2"

# 清理之前的日志
echo > "$SERIAL_LOG"

# 清理旧PID文件
rm -f "$PID_FILE"

# 启动QEMU，串口输出重定向到文件
# NUMA配置: 2节点, 8CPU(每节点4核), 2GB内存(每节点1GB)
qemu-system-x86_64 \
    -kernel "$KERNEL" \
    -hda "$DISK" \
    -append "root=/dev/sda rw console=ttyS0 nokaslr" \
    -display none \
    -monitor none \
    -machine q35 \
    -object memory-backend-ram,id=mem0,size=1G \
    -object memory-backend-ram,id=mem1,size=1G \
    -smp 8,sockets=2,cores=4 \
    -m 2G \
    -numa node,memdev=mem0,cpus=0-3 \
    -numa node,memdev=mem1,cpus=4-7 \
    -netdev user,id=net0,hostfwd=tcp::2222-:22 -device virtio-net-pci,netdev=net0 \
    -virtfs local,path="$SHARED_DIR",mount_tag=shared,security_model=none,id=shared \
    -serial file:"$SERIAL_LOG" \
    -daemonize \
    -pidfile "$PID_FILE"

if [[ ! -f "$PID_FILE" ]]; then
    echo "QEMU failed to create pid file!"
    exit 1
fi

QEMU_PID=$(<"$PID_FILE")
echo "QEMU started with PID: $QEMU_PID"
echo "Serial log: $SERIAL_LOG"
echo "SSH port forwarded: localhost:2222 -> VM:22"
echo "Shared directory: $SHARED_DIR -> /mnt/shared (in VM)"

# 等待VM启动
echo "Waiting for VM to boot..."
sleep 10

# 检查QEMU是否还在运行
if ! kill -0 $QEMU_PID 2>/dev/null; then
    echo "QEMU failed to start!"
    exit 1
fi

echo "VM is running."
echo "To stop: kill $QEMU_PID or Ctrl+A then X in terminal"

# 返回PID供其他脚本使用
echo $QEMU_PID > "$PID_FILE"
