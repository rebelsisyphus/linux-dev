#!/bin/bash
# Generic VM runner for qdisc_uaf_fix reproducers.
# Usage: run_in_vm.sh <test_script_on_host> <output_dir> [test_name]

set -e

TEST_SCRIPT="$1"
OUTPUT_DIR="$2"
TEST_NAME="${3:-$(basename "$TEST_SCRIPT")}"

QEMU_DIR="/home/sisyphus/code/qemu"
SERIAL_LOG="$QEMU_DIR/serial.log"
TEST_RESULT="/home/sisyphus/code/test/test_result.txt"
SPAWN_RESULT="/home/sisyphus/code/test/spawn-test/test_result.txt"

if [ ! -f "$TEST_SCRIPT" ]; then
    echo "Error: test script not found: $TEST_SCRIPT"
    exit 1
fi

mkdir -p "$OUTPUT_DIR"

# Backup current test.sh in shared directory
SHARED_TEST="/home/sisyphus/code/test/test.sh"
BACKUP="/home/sisyphus/code/test/test.sh.qdisc_uaf_fix.bak"

if [ ! -f "$BACKUP" ]; then
    cp "$SHARED_TEST" "$BACKUP"
fi

# Use the requested test script as the VM test.sh
cp "$TEST_SCRIPT" "$SHARED_TEST"

cleanup() {
    echo "Restoring original test.sh..."
    cp "$BACKUP" "$SHARED_TEST"
    rm -f "$BACKUP"
}
trap cleanup EXIT

# Stop any running QEMU
if [ -f /tmp/qemu.pid ]; then
    QEMU_PID=$(cat /tmp/qemu.pid 2>/dev/null || echo "")
    if [ -n "$QEMU_PID" ] && kill -0 "$QEMU_PID" 2>/dev/null; then
        echo "Stopping existing QEMU (PID $QEMU_PID)..."
        kill "$QEMU_PID" 2>/dev/null || true
        sleep 3
    fi
fi

cd "$QEMU_DIR"

# Clean serial log and previous results
> "$SERIAL_LOG"
rm -f "$TEST_RESULT" "$SPAWN_RESULT"

# Start VM
./qemu.sh ./bzImage

# Wait for SSH port to be open, then give sshd more time to accept banners
for i in $(seq 1 120); do
    if nc -z localhost 2222 2>/dev/null; then
        echo "SSH port open, waiting for banner..."
        sleep 5
        break
    fi
    sleep 1
done

if ! nc -z localhost 2222 2>/dev/null; then
    echo "SSH not available, collecting serial log and aborting"
    cp "$SERIAL_LOG" "$OUTPUT_DIR/${TEST_NAME}_serial.log"
    exit 1
fi

# Run the test through the standard qemu/test.sh wrapper
# (which mounts shared dir and executes test.sh)
./test.sh 2>&1 | tee "$OUTPUT_DIR/${TEST_NAME}_test_result.txt" || true

cp "$TEST_RESULT" "$OUTPUT_DIR/${TEST_NAME}_test_result.txt" 2>/dev/null || true
cp "$SPAWN_RESULT" "$OUTPUT_DIR/${TEST_NAME}_spawn_result.txt" 2>/dev/null || true
cp "$SERIAL_LOG" "$OUTPUT_DIR/${TEST_NAME}_serial.log"

# Stop VM
if [ -f /tmp/qemu.pid ]; then
    QEMU_PID=$(cat /tmp/qemu.pid 2>/dev/null || echo "")
    if [ -n "$QEMU_PID" ] && kill -0 "$QEMU_PID" 2>/dev/null; then
        kill "$QEMU_PID" 2>/dev/null || true
        sleep 3
    fi
fi

echo "Test $TEST_NAME completed"
exit 0
