#!/bin/bash
# 一键测试执行脚本 - 在虚拟机中执行test.sh并获取结果

set -e

# 配置
SSH_PORT=2222
VM_USER="root"
SHARED_DIR="/home/sisyphus/code/test"
BASE_TEST_SCRIPT="$SHARED_DIR/test.sh"
BASE_RESULT_FILE="$SHARED_DIR/test_result.txt"
SPAWN_TEST_SCRIPT="$SHARED_DIR/spawn-test/test.sh"
SPAWN_RESULT_FILE="$SHARED_DIR/spawn-test/test_result.txt"
SERIAL_LOG="serial.log"
QEMU_LOG="qemu.log"

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

# 检查测试脚本是否存在
if [ ! -f "$BASE_TEST_SCRIPT" ]; then
    echo -e "${RED}Error: $BASE_TEST_SCRIPT not found!${NC}"
    exit 1
fi

if [ ! -f "$SPAWN_TEST_SCRIPT" ]; then
    echo -e "${RED}Error: $SPAWN_TEST_SCRIPT not found!${NC}"
    exit 1
fi

run_remote_test() {
    local name="$1"
    local result_file="$2"
    local remote_cmd="$3"
    local test_exit_code

    rm -f "$result_file"

    echo ""
    echo "========================================"
    echo "Executing $name in VM..."
    echo "========================================"
    echo ""

    ssh -p $SSH_PORT -o ConnectTimeout=10 -o StrictHostKeyChecking=no \
        $VM_USER@localhost "$remote_cmd" 2>&1 | tee "$result_file"

    test_exit_code=${PIPESTATUS[0]}

    echo ""
    if [ $test_exit_code -eq 0 ]; then
        echo -e "${GREEN}$name completed successfully (exit code: $test_exit_code)${NC}"
    else
        echo -e "${RED}$name failed (exit code: $test_exit_code)${NC}"
    fi

    return $test_exit_code
}

# 检查QEMU是否正在运行
if [ ! -f /tmp/qemu.pid ]; then
    echo -e "${YELLOW}QEMU not running. Starting VM...${NC}"
    ./qemu.sh
    sleep 15
fi

QEMU_PID=$(cat /tmp/qemu.pid 2>/dev/null || echo "")
if [ -z "$QEMU_PID" ] || ! kill -0 "$QEMU_PID" 2>/dev/null; then
    echo -e "${RED}Error: QEMU is not running!${NC}"
    exit 1
fi

echo -e "${GREEN}QEMU is running (PID: $QEMU_PID)${NC}"

# 启动panic监控
if [ ! -f /tmp/panic_monitor.pid ] || ! kill -0 $(cat /tmp/panic_monitor.pid) 2>/dev/null; then
    echo "Starting panic monitor..."
    ./monitor.sh "$SERIAL_LOG" "$QEMU_LOG"
    sleep 2
fi

echo "Panic monitor is active"

# 等待SSH可用
echo "Waiting for SSH..."
for i in {1..30}; do
    if nc -z localhost $SSH_PORT 2>/dev/null; then
        echo -e "${GREEN}SSH ready!${NC}"
        break
    fi
    echo -n "."
    sleep 1
done

if ! nc -z localhost $SSH_PORT 2>/dev/null; then
    echo -e "${RED}SSH not available${NC}"
    exit 1
fi

# 挂载共享目录
ssh -p $SSH_PORT -o ConnectTimeout=5 -o StrictHostKeyChecking=no \
    $VM_USER@localhost "mkdir -p /mnt/shared && mount -t 9p -o trans=virtio shared /mnt/shared" 2>/dev/null || true

TEST_EXIT_CODE=0

run_remote_test "base regression" "$BASE_RESULT_FILE" \
    "cd /mnt/shared && chmod +x test.sh && ./test.sh" || TEST_EXIT_CODE=1

run_remote_test "spawn stress" "$SPAWN_RESULT_FILE" \
    "cd /mnt/shared/spawn-test && chmod +x test.sh && ./test.sh" || TEST_EXIT_CODE=1

echo ""
echo "========================================"
echo "Test artifacts:"
echo "========================================"
echo "Base regression: $BASE_RESULT_FILE"
echo "Spawn stress:    $SPAWN_RESULT_FILE"

# 检查panic
if [ -f /tmp/qemu_panic_detected ]; then
    echo ""
    echo -e "${RED}WARNING: Panic detected!${NC}"
    echo "See $QEMU_LOG for details."
fi

echo ""
echo "Test execution completed."
exit $TEST_EXIT_CODE
