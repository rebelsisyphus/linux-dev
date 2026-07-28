#!/bin/bash
# test-kernel wrapper for qdisc_uaf_fix reproducers.
# Usage: run_test_kernel.sh <test_script> <output_dir> <test_name>

set -e

TEST_SCRIPT="$1"
OUTPUT_DIR="$2"
TEST_NAME="$3"

TEST_DIR="/home/sisyphus/code/test"
SHARED_TEST="$TEST_DIR/test.sh"
BACKUP="$TEST_DIR/test.sh.qdisc_uaf_fix.bak"
QEMU_DIR="/home/sisyphus/code/qemu"
SERIAL_LOG="$QEMU_DIR/serial.log"
QEMU_LOG="$QEMU_DIR/qemu.log"
RESULT_FILE="$TEST_DIR/test_result.txt"

KERNEL="${4:-/home/sisyphus/code/linux/arch/x86/boot/bzImage}"

if [ ! -f "$TEST_SCRIPT" ]; then
    echo "Error: test script not found: $TEST_SCRIPT"
    exit 1
fi

mkdir -p "$OUTPUT_DIR"

# Backup current test.sh
if [ ! -f "$BACKUP" ]; then
    cp "$SHARED_TEST" "$BACKUP"
fi

cp "$TEST_SCRIPT" "$SHARED_TEST"

cleanup() {
    echo "Restoring test.sh..."
    cp "$BACKUP" "$SHARED_TEST"
    rm -f "$BACKUP"
    if [ -f /tmp/qemu.pid ]; then
        local pid
        pid=$(cat /tmp/qemu.pid 2>/dev/null || echo "")
        if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
            echo "Stopping VM..."
            kill "$pid" 2>/dev/null || true
            sleep 3
        fi
    fi
    pkill -f qemu-system-x86_64 2>/dev/null || true
    sleep 1
}
trap cleanup EXIT

# Run test-kernel
test-kernel "$KERNEL" 2>&1 | tee "$OUTPUT_DIR/${TEST_NAME}_test_kernel.log" || true

# Collect artifacts
sleep 2
sync

ssh -p 2222 -o ConnectTimeout=10 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
    root@localhost "dmesg" > "$OUTPUT_DIR/${TEST_NAME}_dmesg.log" 2>&1 || true

cp "$SERIAL_LOG" "$OUTPUT_DIR/${TEST_NAME}_serial.log" 2>/dev/null || true
cp "$QEMU_LOG" "$OUTPUT_DIR/${TEST_NAME}_qemu.log" 2>/dev/null || true
cp "$RESULT_FILE" "$OUTPUT_DIR/${TEST_NAME}_test_result.txt" 2>/dev/null || true

echo "Test $TEST_NAME completed. Artifacts saved to $OUTPUT_DIR"
exit 0
