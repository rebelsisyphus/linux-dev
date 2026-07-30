#!/bin/bash
# Run the VXLAN GRO -> bridge -> qdisc reproducer inside a test VM.
# The binary is expected to be mounted at /mnt/shared/qdisc_uaf_vxlan_repro.

LOG="/mnt/shared/qdisc_uaf_vxlan_test.log"
DMESG_LOG="/mnt/shared/qdisc_uaf_vxlan_dmesg.log"

echo "=== VXLAN GRO/bridge qdisc reproducer test ===" | tee "$LOG"

if [ ! -x /mnt/shared/qdisc_uaf_vxlan_repro ]; then
    echo "Binary not found at /mnt/shared/qdisc_uaf_vxlan_repro" | tee -a "$LOG"
    exit 1
fi

cp /mnt/shared/qdisc_uaf_vxlan_repro /tmp/qdisc_uaf_vxlan_repro
chmod +x /tmp/qdisc_uaf_vxlan_repro

echo 8 > /proc/sys/kernel/printk
dmesg -c > /dev/null 2>&1
dmesg -w > "$DMESG_LOG" 2>&1 &
DMESG_PID=$!
sleep 1

echo "Running VXLAN GRO/bridge reproducer..." | tee -a "$LOG"
timeout 30 /tmp/qdisc_uaf_vxlan_repro 2>&1 | tee -a "$LOG"
echo "Reproducer exit: $?" | tee -a "$LOG"

sleep 3
sync
kill $DMESG_PID 2>/dev/null || true
wait $DMESG_PID 2>/dev/null || true

echo "" | tee -a "$LOG"
echo "=== dmesg ===" | tee -a "$LOG"
cat "$DMESG_LOG" | tee -a "$LOG"

# Check for the original qdisc crash or any unexpected KASAN/UAF.
if grep -qE "KASAN.*qdisc_pkt_len_segs_init|BUG.*qdisc_pkt_len_segs_init|unable to handle.*qdisc_pkt_len_segs_init" "$DMESG_LOG"; then
    echo "VXLAN GRO/bridge qdisc crash DETECTED" | tee -a "$LOG"
    exit 1
elif grep -qE "KASAN.*vxlan_rcv|KASAN.*udp_tun_rx_dst" "$DMESG_LOG"; then
    echo "VXLAN early-clear regression DETECTED" | tee -a "$LOG"
    exit 1
else
    echo "No crash detected" | tee -a "$LOG"
    exit 0
fi
