#!/bin/bash
# Robust VM runner for a single reproducer script.
# Usage: run_repro_in_vm.sh <test_script> <output_dir> <test_name>

set -e

TEST_SCRIPT="$1"
OUTPUT_DIR="$2"
TEST_NAME="$3"

QEMU_DIR="/home/sisyphus/code/qemu"
SERIAL_LOG="$QEMU_DIR/serial.log"

cleanup_vm() {
    if [ -f /tmp/qemu.pid ]; then
        local pid
        pid=$(cat /tmp/qemu.pid 2>/dev/null || echo "")
        if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
            echo "Stopping VM (PID $pid)..."
            kill "$pid" 2>/dev/null || true
            sleep 3
        fi
    fi
}

cleanup_vm

cd "$QEMU_DIR"
> "$SERIAL_LOG"
./qemu.sh ./bzImage

# Wait for SSH with retries
for i in $(seq 1 180); do
    if nc -z localhost 2222 2>/dev/null; then
        echo "SSH port open"
        break
    fi
    sleep 1
done

if ! nc -z localhost 2222 2>/dev/null; then
    echo "SSH port never opened"
    cp "$SERIAL_LOG" "$OUTPUT_DIR/${TEST_NAME}_serial.log"
    cleanup_vm
    exit 1
fi

# Give sshd time to fully start and retry banner exchange
for attempt in 1 2 3; do
    echo "SSH attempt $attempt..."
    if ssh -p 2222 -o ConnectTimeout=30 -o ServerAliveInterval=10 -o ServerAliveCountMax=3 -o StrictHostKeyChecking=no \
            root@localhost "echo banner-ok"; then
        echo "SSH banner OK"
        break
    fi
    sleep 5
done

# Copy test script into shared dir and run it inside VM
REPRO_NAME="repro_${TEST_NAME}.sh"
cp "$TEST_SCRIPT" "/home/sisyphus/code/test/$REPRO_NAME"

mkdir -p "$OUTPUT_DIR"

ssh -p 2222 -o ConnectTimeout=30 -o ServerAliveInterval=10 -o ServerAliveCountMax=3 -o StrictHostKeyChecking=no \
    root@localhost "mkdir -p /mnt/shared; mountpoint -q /mnt/shared || mount -t 9p -o trans=virtio shared /mnt/shared; cd /mnt/shared && chmod +x $REPRO_NAME && ./$REPRO_NAME" 2>&1 | tee "$OUTPUT_DIR/${TEST_NAME}_test_result.txt" || true

sleep 5
ssh -p 2222 -o ConnectTimeout=30 -o ServerAliveInterval=10 -o ServerAliveCountMax=3 -o StrictHostKeyChecking=no \
    root@localhost "dmesg" > "$OUTPUT_DIR/${TEST_NAME}_dmesg.log" 2>&1 || true

cp "$SERIAL_LOG" "$OUTPUT_DIR/${TEST_NAME}_serial.log"
cleanup_vm
rm -f "/home/sisyphus/code/test/$REPRO_NAME"

echo "Test $TEST_NAME completed"
