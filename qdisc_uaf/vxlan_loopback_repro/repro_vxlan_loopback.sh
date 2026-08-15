#!/bin/bash
# repro_vxlan_loopback.sh
#
# Non-crafted-packet reproducer for the qdisc_pkt_len_segs_init() stale
# transport_header bug, using ONLY the local tunnel transmit path.
#
# Model:
#   A plain TCP bulk transfer is sent through a local vxlan device
#   (vxlanA in netns sendns).  vxlan_build_skb() -> iptunnel_handle_offloads()
#   sets SKB_GSO_UDP_TUNNEL via |=, preserving SKB_GSO_TCPV4 and gso_size of
#   the inner TCP GSO frame.  The outer frame is routed to the receiver
#   (root netns, 10.0.0.1), where the vxlan socket decapsulates it:
#   iptunnel_pull_offloads() clears the UDP_TUNNEL bits but keeps
#   TCPV4 + gso_size and clears encapsulation, while transport_header still
#   points at the removed outer UDP header (negative offset).  With GRO off
#   on vxlan0 and the inner frame L2-forwarded by the bridge, the bridge
#   egress path reaches qdisc_pkt_len_segs_init() with the stale header and
#   overflows the negative offset -> KASAN fault on an unpatched kernel.
#
# Key design points (why a cross-netns pair is required):
#   * vxlan enables VXLAN_F_LOCALBYPASS by default (vxlan_core.c
#     "default to local bypass on a new device"), so a same-host loopback
#     destination (RTCF_LOCAL) would be short-circuited inside vxlan_xmit_one()
#     and never produce an outer packet.  Sending from a separate netns whose
#     routing table does NOT contain the destination avoids the bypass.
#   * iproute2 has no "nolocalbypass" knob, so the cross-netns setup is the
#     only way to get a real encapsulated packet on a single host.
#   * The receiver vxlan0 must have GRO off, otherwise inet_gro_receive()
#     would reset transport_header to the inner TCP header.
#   * The inner frame must be L2-forwarded by the bridge (not delivered to
#     the local IP stack), otherwise ip_rcv_core() would reset
#     transport_header.
#
# Topology:
#
#   netns sendns                         root netns                        netns testns
#   +----------------------+             +------------------------------+  +----------------------+
#   | vxlanA 10.1.0.1/24   |             | vxlan0 (GRO off) --+          |  | veth1 10.1.0.2/24   |
#   |  (id 100, local      |             |                    |         |  |  (TCP server :9000) |
#   |   192.168.99.1,      |             |                  br0          |  +----------------------+
#   |   remote 10.0.0.1)   |             |                    |         |
#   |       |              |             |                 veth0 --------|-- veth1
#   |   vethBp 192.168.99.1|---vethB-----+ 192.168.99.2      |         |
#   +----------------------+             | lo: 10.0.0.1/32   |         |
#                                        +------------------------------+
#
#   inner TCP flow: sendns 10.1.0.1 -> testns 10.1.0.2
#     - TX: vxlanA xmit, outer dst 10.0.0.1 -> vethB -> root
#     - RX: root vxlan0 socket -> vxlan_rcv decap -> bridge -> veth0 -> testns
#     - crash point: br_dev_queue_push_xmit -> __dev_queue_xmit ->
#       qdisc_pkt_len_segs_init()

set -u

PKT_BYTES="${1:-8388608}"

cleanup() {
	ip link del br0 2>/dev/null || true
	ip link del vxlan0 2>/dev/null || true
	ip link del veth0 2>/dev/null || true
	ip link del vethB 2>/dev/null || true
	ip netns del testns 2>/dev/null || true
	ip netns del sendns 2>/dev/null || true
}

cleanup

echo "=== setting up topology ==="
ip addr replace 10.0.0.1/32 dev lo

# --- root netns receiver ---
ip link add vethB type veth peer name vethBp
ip link add vxlan0 type vxlan id 100 local 10.0.0.1 remote 192.168.99.1 \
	dstport 4789 dev vethB
ip link add br0 type bridge forward_delay 0 stp_state 0 mcast_snooping 0
ip link set vxlan0 master br0
ip link add veth0 type veth peer name veth1
ip link set veth0 master br0
ip link set vethB up
ip link set vxlan0 up
ip link set br0 up
ip link set veth0 up
ip link set lo up
ethtool -K vxlan0 gro off
ip addr add 192.168.99.2/24 dev vethB

# --- sender netns (vxlan TX side, remote is NOT local here) ---
ip netns add sendns
ip link set vethBp netns sendns
ip netns exec sendns ip link set lo up
ip netns exec sendns ip link set vethBp up
ip netns exec sendns ip addr add 192.168.99.1/24 dev vethBp
ip netns exec sendns ip link add vxlanA type vxlan id 100 \
	local 192.168.99.1 remote 10.0.0.1 dstport 4789 dev vethBp
ip netns exec sendns ip link set vxlanA up
ip netns exec sendns ip addr add 10.1.0.1/24 dev vxlanA
ip netns exec sendns ip route add 10.0.0.1/32 via 192.168.99.2 dev vethBp

# --- receiver testns ---
ip netns add testns
ip link set veth1 netns testns
ip netns exec testns ip link set lo up
ip netns exec testns ip link set veth1 up
ip netns exec testns ip addr add 10.1.0.2/24 dev veth1

echo "=== connectivity check (ARP learning may need two rounds) ==="
ip netns exec sendns ping -c 2 -W 2 10.1.0.2 >/dev/null 2>&1 || true
ip netns exec sendns ping -c 2 -W 2 10.1.0.2 >/dev/null 2>&1 || \
	{ echo "FAIL: sendns -> testns ping"; exit 1; }
echo "ping OK"

echo "=== starting TCP server in testns ==="
ip netns exec testns python3 -c "
import socket
s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('10.1.0.2', 9000))
s.listen(1)
c, a = s.accept()
print('server: accepted', flush=True)
while c.recv(65536):
    pass
print('server: done', flush=True)
" &
SRV=$!
sleep 2

echo "=== sending $PKT_BYTES bytes TCP from sendns (plain traffic) ==="
ip netns exec sendns python3 -c "
import socket, sys
s = socket.create_connection(('10.1.0.2', 9000), timeout=10)
print('client: connected', flush=True)
s.sendall(b'x' * int(sys.argv[1]))
s.close()
print('client: sent', flush=True)
" "$PKT_BYTES"

sleep 2
kill $SRV 2>/dev/null
wait $SRV 2>/dev/null

echo "=== counters ==="
ip -s link show vxlan0 | grep -A1 -E 'RX:|TX:'
ip netns exec sendns ip -s link show vxlanA | grep -A1 'RX:'
echo "=== done ==="
