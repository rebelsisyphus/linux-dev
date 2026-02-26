#!/bin/bash
# 一键测试脚本

set -e

SHARED_DIR="$(pwd)/shared"
TEST_SCRIPT="$SHARED_DIR/test.sh"
RESULT_FILE="$SHARED_DIR/test_result.txt"

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo "QEMU Test Automation Script"
echo "==========================="
echo ""

# 1. 检查QEMU是否运行
if [ -f /tmp/qemu.pid ]; then
    QEMU_PID=$(cat /tmp/qemu.pid)
    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
        echo -e "${YELLOW}QEMU not running. Starting...${NC}"
        ./qemu.sh
        echo ""
    fi
else
    echo -e "${YELLOW}QEMU not running. Starting...${NC}"
    ./qemu.sh
    echo ""
fi

# 2. 启动panic监控（如果未运行）
if [ ! -f /tmp/panic_monitor.pid ] || ! kill -0 $(cat /tmp/panic_monitor.pid) 2>/dev/null; then
    echo "Starting panic monitor..."
    ./monitor.sh &
    sleep 1
fi

# 3. 等待SSH可用
echo "Waiting for SSH to be available..."
MAX_RETRIES=30
RETRY_COUNT=0

while [ $RETRY_COUNT -lt $MAX_RETRIES ]; do
    if nc -zv localhost 2222 2>/dev/null; then
        # 测试SSH连接
        if ssh -o ConnectTimeout=5 -o StrictHostKeyChecking=accept-new -o BatchMode=yes root@localhost -p 2222 "echo ok" 2>/dev/null | grep -q "ok"; then
            echo -e "${GREEN}SSH is ready!${NC}"
            break
        fi
    fi
    
    RETRY_COUNT=$((RETRY_COUNT + 1))
    echo "  Attempt $RETRY_COUNT/$MAX_RETRIES..."
    sleep 2
done

if [ $RETRY_COUNT -eq $MAX_RETRIES ]; then
    echo -e "${RED}Error: SSH not available after $MAX_RETRIES attempts${NC}"
    exit 1
fi

# 4. 确保共享目录已挂载
echo "Mounting shared directory..."
ssh root@localhost -p 2222 "mountpoint -q /mnt/shared || mount -t 9p -o trans=virtio shared /mnt/shared" 2>/dev/null || true

# 5. 创建默认测试脚本（如果不存在）
if [ ! -f "$TEST_SCRIPT" ]; then
    echo "Creating default test script..."
    cat > "$TEST_SCRIPT" << 'EOF'
#!/bin/bash
# Default test script

echo "=== QEMU VM Test ==="
echo "Date: $(date)"
echo "Hostname: $(hostname)"
echo "Kernel: $(uname -r)"
echo ""
echo "=== System Info ==="
lsb_release -a 2>/dev/null || cat /etc/os-release
echo ""
echo "=== Memory Usage ==="
free -h
echo ""
echo "=== Disk Usage ==="
df -h
echo ""
echo "=== Shared Directory ==="
ls -la /mnt/shared/
echo ""
echo "=== Test Result ==="
echo "Status: PASSED"
echo "All tests completed successfully!"
EOF
    chmod +x "$TEST_SCRIPT"
fi

# 6. 执行测试
echo ""
echo "Running test script in VM..."
echo "==========================="
ssh root@localhost -p 2222 "cd /mnt/shared && ./test.sh" | tee "$RESULT_FILE"

# 7. 显示结果
echo ""
echo "==========================="
echo "Test execution completed!"
echo "Results saved to: $RESULT_FILE"

# 8. 检查panic
if [ -f qemu.log ]; then
    echo -e "${RED}WARNING: Panic detected! Check qemu.log${NC}"
    exit 1
else
    echo -e "${GREEN}No panic detected.${NC}"
fi

echo ""
echo "To stop QEMU: kill $(cat /tmp/qemu.pid)"
