#!/bin/bash
# Minimal VXLAN regression reproducer for patch v2.
# Triggers KASAN UAF in vxlan_rcv if transport_header is cleared too early.

LOG="/mnt/shared/qdisc_uaf_vxlan_test.log"
DMESG_LOG="/mnt/shared/qdisc_uaf_vxlan_dmesg.log"

echo "=== VXLAN regression reproducer ===" | tee "$LOG"

echo 8 > /proc/sys/kernel/printk
dmesg -c > /dev/null 2>&1
dmesg -w > "$DMESG_LOG" 2>&1 &
DMESG_PID=$!
sleep 1

cleanup() {
    ip netns del vxlan_ns2 2>/dev/null || true
    ip netns del vxlan_ns1 2>/dev/null || true
}
trap cleanup EXIT

cleanup

# Create two namespaces connected by a veth pair
ip netns add vxlan_ns1
ip netns add vxlan_ns2
ip link add veth0 netns vxlan_ns1 type veth peer veth0 netns vxlan_ns2

ip -n vxlan_ns1 addr add 10.0.0.1/24 dev veth0
ip -n vxlan_ns2 addr add 10.0.0.2/24 dev veth0
ip -n vxlan_ns1 link set veth0 up
ip -n vxlan_ns2 link set veth0 up

# Create VXLAN devices in both namespaces (GBP on both sides to exercise
# udp_tun_rx_dst() path that needs transport_header)
ip -n vxlan_ns1 link add vxlan0 type vxlan id 100 remote 10.0.0.2 dstport 4789 gbp
ip -n vxlan_ns2 link add vxlan0 type vxlan id 100 remote 10.0.0.1 dstport 4789 gbp

ip -n vxlan_ns1 addr add 192.168.200.1/24 dev vxlan0
ip -n vxlan_ns2 addr add 192.168.200.2/24 dev vxlan0
ip -n vxlan_ns1 link set vxlan0 up
ip -n vxlan_ns2 link set vxlan0 up

# Give interfaces time to come up
sleep 2

# Trigger vxlan receive by pinging the remote overlay address
echo "Pinging remote VXLAN overlay address..." | tee -a "$LOG"
ip netns exec vxlan_ns1 ping -c 3 -W 2 192.168.200.2 2>&1 | tee -a "$LOG" || true

sleep 2
sync
kill $DMESG_PID 2>/dev/null || true
wait $DMESG_PID 2>/dev/null || true

echo "" | tee -a "$LOG"
echo "=== dmesg ===" | tee -a "$LOG"
cat "$DMESG_LOG" | tee -a "$LOG"

# Check for KASAN crash in the VXLAN receive path
# (patch v2 triggers it in udp_tun_rx_dst(), called from vxlan_rcv())
if grep -qE "KASAN.*(vxlan_rcv|udp_tun_rx_dst)" "$DMESG_LOG"; then
    echo "VXLAN KASAN UAF DETECTED" | tee -a "$LOG"
    exit 1
else
    echo "No VXLAN KASAN UAF detected" | tee -a "$LOG"
    exit 0
fi
