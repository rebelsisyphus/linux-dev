#!/bin/bash
# Bridge GSO/TCP reproducer for qdisc_uaf_fix.
# Builds a valid inner IPv4/TCP header and disables TSO on the egress veth
# to force software GSO segmentation.  This verifies that the
# "unset transport_header" fix does not crash tcp4_gso_segment() because
# inet_gso_segment() resets transport_header before calling it.

LOG="/mnt/shared/qdisc_uaf_bridge_gso_tcp_test.log"
DMESG_LOG="/mnt/shared/qdisc_uaf_bridge_gso_tcp_dmesg.log"
echo "=== qdisc_uaf bridge GSO/TCP reproducer test ===" | tee "$LOG"

cp /mnt/shared/qdisc_uaf_repro_bridge_gso_tcp /tmp/qdisc_uaf_repro_bridge_gso_tcp
chmod +x /tmp/qdisc_uaf_repro_bridge_gso_tcp

echo 8 > /proc/sys/kernel/printk
dmesg -c > /dev/null 2>&1
dmesg -w > "$DMESG_LOG" 2>&1 &
DMESG_PID=$!
sleep 1

echo "Running bridge GSO/TCP reproducer (TSO disabled on egress veth)..." | tee -a "$LOG"
timeout 30 /tmp/qdisc_uaf_repro_bridge_gso_tcp 2>&1 | tee -a "$LOG"
echo "Reproducer exit: $?" | tee -a "$LOG"

sleep 3
sync
kill $DMESG_PID 2>/dev/null || true
wait $DMESG_PID 2>/dev/null || true

echo "" | tee -a "$LOG"
echo "=== dmesg ===" | tee -a "$LOG"
cat "$DMESG_LOG" | tee -a "$LOG"
