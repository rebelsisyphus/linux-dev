# VXLAN loopback reproducer (non-crafted packets)

This directory contains a reproducer for the `qdisc_pkt_len_segs_init()`
stale `transport_header` crash that uses **only normal traffic** -- a plain
TCP bulk transfer through a locally configured vxlan device.  No crafted
packets, no tun/virtio-net header injection, no ingress qdisc.

The bug is reached through the plain L2-forwarding path
(`br_dev_queue_push_xmit -> __dev_queue_xmit`), so an ingress qdisc is
*not* required.

## Files

- `repro_vxlan_loopback.sh` -- sets up the topology and triggers the bug
- `serial_vxlan_loopback_rc7_panic.log` -- panic log from kernel
  7.2.0-rc7 (unpatched), produced by the script as-is

## Trigger model

The crash requires, at the time `qdisc_pkt_len_segs_init()` runs on the
egress of a bridge/HSR forward:

1. `shinfo->gso_size != 0` -- otherwise the function returns early.
2. `gso_type & (SKB_GSO_TCPV4 | SKB_GSO_TCPV6)` -- the branch that
   dereferences the transport header (`th = skb->data + hdr_len`).
3. `skb->encapsulation == 0` -- the branch that computes
   `hdr_len = skb_transport_offset(skb)`.
4. `transport_header` is a stale negative offset (points at the removed
   outer L4 header, i.e. before `skb->data`).

### Where the GSO metadata comes from (no crafted packets)

A TCP bulk transfer generates GSO skbs (`SKB_GSO_TCPV4`, `gso_size = MSS`).
When such a skb is transmitted through a vxlan device, `vxlan_build_skb()`
calls `iptunnel_handle_offloads()`, which does:

```c
skb_shinfo(skb)->gso_type |= SKB_GSO_UDP_TUNNEL;   /* ip_tunnel_core.c:188 */
```

`|=` preserves the inner `SKB_GSO_TCPV4` bit, producing the combined type
`TCPV4|UDP_TUNNEL`.  This bit combination is what lets the skb pass
`udp_unexpected_gso()` (include/linux/udp.h:187) at the receiver:
an encap socket that receives a GSO skb *without* the UDP_TUNNEL bits would
segment it (`udp_rcv_segment`), zeroing `gso_size` and making the qdisc
path harmless.

At the receiver, `vxlan_rcv()` -> `__iptunnel_pull_header()` ->
`iptunnel_pull_offloads()` (include/net/ip_tunnels.h:649):

```c
skb_shinfo(skb)->gso_type &= ~(NETIF_F_GSO_ENCAP_ALL >> NETIF_F_GSO_SHIFT);
skb->encapsulation = 0;
```

This clears the UDP_TUNNEL bits but **keeps** `TCPV4` and **does not touch
`gso_size`** -- exactly the state the crash branch needs.  The
`transport_header` still points at the outer UDP header which was just
pulled, i.e. a negative offset.

### Why a cross-netns vxlan pair

`encap_bypass_if_local()` (vxlan_core.c:2311) short-circuits encapsulation
when the outer destination is a local address (`RTCF_LOCAL`), unless
`VXLAN_F_LOCALBYPASS` is cleared.  vxlan enables `VXLAN_F_LOCALBYPASS` by
default (`vxlan_dev_configure()`, vxlan_core.c:4263: "default to local
bypass on a new device"), and iproute2 exposes no knob to disable it.
Therefore the sender must live in a netns whose routing table does not
contain the destination address, so the outer packet is really emitted.
The receiver netns must have that address local so the vxlan socket accepts
the packet.

### The two "fixers" that must be avoided

* `inet_gro_receive()` (net/ipv4/af_inet.c) resets `transport_header` to
  the inner L4 header -- avoid by `ethtool -K vxlan0 gro off`.
* `ip_rcv_core()` (net/ipv4/ip_input.c:575) resets `transport_header` for
  packets delivered to the IP stack -- avoid by L2-forwarding the inner
  frame through a bridge (the forwarding clone reaches `__dev_queue_xmit`
  before any IP-stack parsing).

## Topology

```
 netns sendns                         root netns                        netns testns
 +----------------------+             +------------------------------+  +----------------------+
 | vxlanA 10.1.0.1/24   |             | vxlan0 (GRO off) --+          |  | veth1 10.1.0.2/24   |
 |  (id 100, local      |             |                    |         |  |  (TCP server :9000) |
 |   192.168.99.1,      |             |                  br0          |  +----------------------+
 |   remote 10.0.0.1)   |             |                    |         |
 |       |              |             |                 veth0 --------|-- veth1
 |   vethBp 192.168.99.1|---vethB-----+ 192.168.99.2      |         |
 +----------------------+             | lo: 10.0.0.1/32   |         |
                                      +------------------------------+

 inner TCP flow: sendns 10.1.0.1 -> testns 10.1.0.2
   TX: vxlanA xmit (outer dst 10.0.0.1) -> vethB -> root
   RX: root vxlan0 socket -> vxlan_rcv decap -> br0 -> veth0 -> testns
```

## Usage

Run as root on the target host:

```bash
./repro_vxlan_loopback.sh [bytes]      # default 8 MiB
```

The script requires `iproute2`, `ethtool`, `python3`, and vxlan support in
the kernel.

## Expected behavior

* **Unpatched kernel**: KASAN fault / kernel panic in
  `qdisc_pkt_len_segs_init()` during the transfer.  Verified panic trace
  (7.2.0-rc7, `serial_vxlan_loopback_rc7_panic.log`):

  ```
  RIP: 0010:__asan_load2+0x48/0xa0
  Call Trace:
   qdisc_pkt_len_segs_init+0x128/0x300
   __dev_queue_xmit+0x13a/0x1960
   br_dev_queue_push_xmit+0x77/0x140
   br_handle_frame_finish+0x5f6/0xae0
   br_handle_frame+0x289/0x410
   __netif_receive_skb_core.constprop.0+0x698/0x1440
   __netif_receive_skb_one_core+0x91/0x130
   process_backlog+0x110/0x260
   ...
   __dev_queue_xmit+0x4f7/0x1960
   ip_finish_output2+0x6f9/0x9b0
  ```

  Note the crash is reached from the bridge forward path
  (`br_dev_queue_push_xmit -> __dev_queue_xmit`), *not* from an ingress
  qdisc.

* **Patched kernel** (with `skb_unset_transport_header()` at the tunnel
  decap boundary and/or the defensive bounds check in
  `qdisc_pkt_len_segs_init()`): the transfer completes, no crash.

## Comparison with the other reproducers

| Reproducer | Crafted packets? | Path | Needs ingress qdisc? |
|---|---|---|---|
| `../gtp_repro/qdisc_uaf_gtp_repro.c` | yes (vnet header GSO bits) | gtp -> ingress qdisc | yes |
| `../vxlan_repro/qdisc_uaf_vxlan_repro.c` | yes (vnet header GSO bits) | vxlan -> bridge | no |
| **this one** | **no (plain TCP bulk transfer)** | **vxlan TX loopback -> bridge** | **no** |

The non-crafted variant demonstrates that an attacker with root /
CAP_NET_ADMIN on the host (the usual post-exploitation / container-escape
capability) can trigger the bug with nothing but `iproute2` configuration
and ordinary TCP traffic -- no custom injection program is required.
