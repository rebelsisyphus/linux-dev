#!/bin/bash
# Bridge IP ethertype reproducer for qdisc_uaf_fix.
# Uses the qdisc_uaf_repro_bridge binary (ETH_P_IP inner frame).

LOG="/mnt/shared/qdisc_uaf_bridge_test.log"
DMESG_LOG="/mnt/shared/qdisc_uaf_bridge_dmesg.log"
echo "=== qdisc_uaf bridge IP ethertype reproducer test ===" | tee "$LOG"

cp /mnt/shared/qdisc_uaf_repro_bridge /tmp/qdisc_uaf_repro_bridge
chmod +x /tmp/qdisc_uaf_repro_bridge

echo 8 > /proc/sys/kernel/printk
dmesg -c > /dev/null 2>&1
dmesg -w > "$DMESG_LOG" 2>&1 &
DMESG_PID=$!
sleep 1

echo "Running bridge IP ethertype reproducer (GRO default)..." | tee -a "$LOG"
timeout 30 /tmp/qdisc_uaf_repro_bridge 2>&1 | tee -a "$LOG"
echo "Reproducer exit: $?" | tee -a "$LOG"

sleep 3
sync
kill $DMESG_PID 2>/dev/null || true
wait $DMESG_PID 2>/dev/null || true

echo "" | tee -a "$LOG"
echo "=== dmesg ===" | tee -a "$LOG"
cat "$DMESG_LOG" | tee -a "$LOG"
