#!/bin/bash
# QEMU启动脚本 - 支持网络、共享目录、串口日志

set -e

# 配置
KERNEL="bzImage"
DISK="rootfs.img"
SERIAL_LOG="serial.log"
QEMU_LOG="qemu.log"
SHARED_DIR="$(pwd)/shared"
VM_IP="10.0.2.15"
HOST_IP="10.0.2.2"

# 检查必要文件
if [ ! -f "$KERNEL" ]; then
    echo "Error: Kernel image '$KERNEL' not found!"
    echo "Please provide a kernel image or download one."
    exit 1
fi

if [ ! -f "$DISK" ]; then
    echo "Error: Rootfs image '$DISK' not found!"
    echo "Please run: sudo ./mkext.sh"
    exit 1
fi

# 创建共享目录
mkdir -p "$SHARED_DIR"

# 清理之前的日志
> "$SERIAL_LOG" 2>/dev/null || true

# 检查是否有之前的QEMU进程
if [ -f /tmp/qemu.pid ]; then
    OLD_PID=$(cat /tmp/qemu.pid)
    if kill -0 "$OLD_PID" 2>/dev/null; then
        echo "Warning: QEMU already running with PID $OLD_PID"
        echo "Stop it first with: kill $OLD_PID"
        exit 1
    fi
fi

# 启动QEMU，串口输出重定向到文件
echo "Starting QEMU VM..."
echo "Kernel: $KERNEL"
echo "Disk: $DISK"
echo "SSH: localhost:2222 -> VM:22"
echo "Shared: $SHARED_DIR -> /mnt/shared"
echo ""

qemu-system-x86_64 \
    -kernel "$KERNEL" \
    -hda "$DISK" \
    -append "root=/dev/sda rw console=ttyS0 nokaslr" \
    -nographic \
    -net nic -net user,hostfwd=tcp::2222-:22 \
    -virtfs local,path="$SHARED_DIR",mount_tag=shared,security_model=none,id=shared \
    -serial file:"$SERIAL_LOG" &

QEMU_PID=$!
echo "QEMU started with PID: $QEMU_PID"
echo $QEMU_PID > /tmp/qemu.pid

# 等待VM启动
echo "Waiting for VM to boot (about 30-40 seconds)..."
sleep 10

# 检查QEMU是否还在运行
if ! kill -0 $QEMU_PID 2>/dev/null; then
    echo "Error: QEMU failed to start!"
    echo "Check serial.log for details"
    exit 1
fi

echo "VM is starting..."
echo "To view boot log: tail -f serial.log"
echo "To stop: kill $QEMU_PID or Ctrl+A then X"
echo ""
echo "Wait for VM to fully boot before connecting via SSH"
