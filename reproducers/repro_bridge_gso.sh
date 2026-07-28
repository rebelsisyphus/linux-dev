#!/bin/bash
# Bridge GSO reproducer for qdisc_uaf_fix.
# Disables TSO on the egress veth to force software GSO segmentation.
# If the current "unset transport_header" fix leaves the GSO path without
# a valid transport header, this should crash in tcp4_gso_segment.

LOG="/mnt/shared/qdisc_uaf_bridge_gso_test.log"
DMESG_LOG="/mnt/shared/qdisc_uaf_bridge_gso_dmesg.log"
echo "=== qdisc_uaf bridge GSO reproducer test ===" | tee "$LOG"

cp /mnt/shared/qdisc_uaf_repro_bridge_gso /tmp/qdisc_uaf_repro_bridge_gso
chmod +x /tmp/qdisc_uaf_repro_bridge_gso

echo 8 > /proc/sys/kernel/printk
dmesg -c > /dev/null 2>&1
dmesg -w > "$DMESG_LOG" 2>&1 &
DMESG_PID=$!
sleep 1

echo "Running bridge GSO reproducer (TSO disabled on egress veth)..." | tee -a "$LOG"
timeout 30 /tmp/qdisc_uaf_repro_bridge_gso 2>&1 | tee -a "$LOG"
echo "Reproducer exit: $?" | tee -a "$LOG"

sleep 3
sync
kill $DMESG_PID 2>/dev/null || true
wait $DMESG_PID 2>/dev/null || true

echo "" | tee -a "$LOG"
echo "=== dmesg ===" | tee -a "$LOG"
cat "$DMESG_LOG" | tee -a "$LOG"
