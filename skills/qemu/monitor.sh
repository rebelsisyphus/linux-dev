#!/bin/bash
# panic监控脚本 - 监控串口日志，保存panic信息

set -e

SERIAL_LOG="${1:-serial.log}"
QEMU_LOG="${2:-qemu.log}"
CONTEXT_LINES=200

# 清理qemu.log
> "$QEMU_LOG"

echo "Monitoring $SERIAL_LOG for panic..."

# 使用tail -f监控日志文件
tail -f "$SERIAL_LOG" | while IFS= read -r line; do
    # 将当前行追加到qemu.log
    echo "$line" >> "$QEMU_LOG"
    
    # 检查是否包含panic
    if echo "$line" | grep -qi "panic"; then
        echo ""
        echo "========================================"
        echo "PANIC DETECTED!"
        echo "========================================"
        echo ""
        
        # 保存panic上下文到qemu.log
        {
            echo ""
            echo "========================================"
            echo "PANIC CONTEXT - $(date)"
            echo "========================================"
            echo ""
            
            # 获取panic行号
            PANIC_LINE=$(grep -n -i "panic" "$SERIAL_LOG" | tail -1 | cut -d: -f1)
            
            if [ -n "$PANIC_LINE" ]; then
                START_LINE=$((PANIC_LINE - CONTEXT_LINES))
                [ "$START_LINE" -lt 1 ] && START_LINE=1
                END_LINE=$((PANIC_LINE + CONTEXT_LINES))
                
                echo "Panic at line $PANIC_LINE, showing lines $START_LINE-$END_LINE:"
                echo ""
                sed -n "${START_LINE},${END_LINE}p" "$SERIAL_LOG"
            fi
            
            echo ""
            echo "========================================"
            echo "END OF PANIC CONTEXT"
            echo "========================================"
        } >> "$QEMU_LOG"
        
        echo "Panic context saved to $QEMU_LOG"
        
        # 通知系统panic发生（可选）
        touch /tmp/qemu_panic_detected
        
        # 停止监控
        break
    fi
done &

MONITOR_PID=$!
echo $MONITOR_PID > /tmp/panic_monitor.pid
echo "Panic monitor started with PID: $MONITOR_PID"
