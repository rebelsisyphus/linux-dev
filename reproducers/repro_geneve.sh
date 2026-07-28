#!/bin/bash
# Minimal Geneve regression reproducer for patch v2.
# Triggers WARNING in geneve_udp_encap_recv if transport_header is cleared
# too early in __iptunnel_pull_header().

LOG="/mnt/shared/qdisc_uaf_geneve_test.log"
DMESG_LOG="/mnt/shared/qdisc_uaf_geneve_dmesg.log"

echo "=== Geneve regression reproducer ===" | tee "$LOG"

echo 8 > /proc/sys/kernel/printk
dmesg -c > /dev/null 2>&1
dmesg -w > "$DMESG_LOG" 2>&1 &
DMESG_PID=$!
sleep 1

cleanup() {
    ip netns del geneve_ns2 2>/dev/null || true
    ip netns del geneve_ns1 2>/dev/null || true
    ip link del geneve0 2>/dev/null || true
}
trap cleanup EXIT

cleanup

# Create two namespaces connected by a veth pair
ip netns add geneve_ns1
ip netns add geneve_ns2
ip link add veth0 netns geneve_ns1 type veth peer veth0 netns geneve_ns2

ip -n geneve_ns1 addr add 10.0.0.1/24 dev veth0
ip -n geneve_ns2 addr add 10.0.0.2/24 dev veth0
ip -n geneve_ns1 link set veth0 up
ip -n geneve_ns2 link set veth0 up

# Create geneve devices in both namespaces
ip -n geneve_ns1 link add geneve0 type geneve id 100 remote 10.0.0.2 dstport 6081
ip -n geneve_ns2 link add geneve0 type geneve id 100 remote 10.0.0.1 dstport 6081

ip -n geneve_ns1 addr add 192.168.100.1/24 dev geneve0
ip -n geneve_ns2 addr add 192.168.100.2/24 dev geneve0
ip -n geneve_ns1 link set geneve0 up
ip -n geneve_ns2 link set geneve0 up

# Give interfaces time to come up
sleep 2

# Trigger geneve receive by pinging the remote overlay address
echo "Pinging remote Geneve overlay address..." | tee -a "$LOG"
ip netns exec geneve_ns1 ping -c 3 -W 2 192.168.100.2 2>&1 | tee -a "$LOG" || true

sleep 2
sync
kill $DMESG_PID 2>/dev/null || true
wait $DMESG_PID 2>/dev/null || true

echo "" | tee -a "$LOG"
echo "=== dmesg ===" | tee -a "$LOG"
cat "$DMESG_LOG" | tee -a "$LOG"

# Check for either the DEBUG_NET warning or a KASAN crash in geneve_udp_encap_recv
if grep -q "skb_transport_header_was_set" "$DMESG_LOG" || \
   grep -q "KASAN.*geneve_udp_encap_recv" "$DMESG_LOG"; then
    echo "GENEVE REGRESSION DETECTED" | tee -a "$LOG"
    exit 1
else
    echo "No Geneve regression detected" | tee -a "$LOG"
    exit 0
fi
