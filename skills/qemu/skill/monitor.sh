#!/bin/bash
# Panic监控脚本

set -e

SERIAL_LOG="${1:-serial.log}"
QEMU_LOG="${2:-qemu.log}"
PID_FILE="/tmp/panic_monitor.pid"

# 检查日志文件是否存在
if [ ! -f "$SERIAL_LOG" ]; then
    echo "Error: Serial log file '$SERIAL_LOG' not found!"
    echo "Start QEMU first: ./qemu.sh"
    exit 1
fi

echo "Starting panic monitor..."
echo "Monitoring: $SERIAL_LOG"
echo "Panic log: $QEMU_LOG"
echo ""

# 后台监控
tail -f "$SERIAL_LOG" | while read line; do
    if echo "$line" | grep -qi "panic\|oops\|segfault"; then
        echo ""
        echo "================================"
        echo "PANIC DETECTED!"
        echo "================================"
        echo "$(date): Panic detected in serial log" >> "$QEMU_LOG"
        echo "" >> "$QEMU_LOG"
        
        # 保存panic上下文（前后200行）
        tail -n 200 "$SERIAL_LOG" >> "$QEMU_LOG"
        
        echo "Panic context saved to: $QEMU_LOG"
        echo ""
    fi
done &

MONITOR_PID=$!
echo $MONITOR_PID > "$PID_FILE"
echo "Panic monitor started with PID: $MONITOR_PID"
echo "To stop: kill $MONITOR_PID"
