# VXLAN GRO -> bridge -> qdisc reproducer

This directory contains a reproducer for the `qdisc_pkt_len_segs_init()`
crash via a VXLAN GRO/bridge forwarding path.

## Files

- `qdisc_uaf_vxlan_repro.c` — userspace C reproducer
- `qdisc_uaf_vxlan_repro` — statically linked x86-64 binary
- `run_vxlan_repro.sh` — wrapper script to run inside a test VM
- `README.md` — this file

## Trigger model

The four conditions required to trigger the bug are:

1. **Outer transport header is set.**
   A VXLAN-encapsulated frame is injected into a TUN device.  The outer
   IPv4/UDP header is processed and `skb->transport_header` is set to the
   outer UDP header.

2. **VXLAN decap leaves a stale transport header.**
   `vxlan_rcv()` strips the outer IPv4/UDP/VXLAN headers and advances
   `skb->data` to the inner Ethernet frame.  If the fix is not present,
   `skb->transport_header` is not cleared and becomes a negative offset
   relative to the new data.

3. **Inner frame is forwarded at L2 instead of being processed by the IP
   stack.**
   The VXLAN device is a member of a bridge.  The decapsulated inner frame
   is forwarded by the bridge to a veth port, so `ip_rcv_core()` is never
   reached and the transport header is not reset.

4. **qdisc path uses the stale offset.**
   `__dev_queue_xmit()` on the egress veth enters
   `qdisc_pkt_len_segs_init()`.  The skb still carries GSO metadata from
   the outer frame, so the function computes `skb_transport_offset(skb)`.
   The negative offset is cast to a huge unsigned value and
   `pskb_may_pull()` reads past the end of the skb, causing a KASAN or
   page fault.

## Topology

```
 +----------------------------------------------------------+
 |  host / netns                                            |
 |                                                          |
 |  tun0(syz_tun) --IPv4/UDP/VXLAN--> vxlan0 --+            |
 |                                             |            |
 |                                          bridge(br0)     |
 |                                             |            |
 |                                          veth0 ----> qdisc crash
 +----------------------------------------------------------+
```

- `tun0` is a TUN device with `IFF_VNET_HDR` so injected frames can carry
  virtio-net GSO metadata.
- `vxlan0` is configured with `id 100`, parent `tun0`, local `10.0.0.1`,
  remote `10.0.0.2`, dstport 4789.
- `br0` contains `vxlan0` and `veth0`.
- The injected frame has:
  - outer IPv4/UDP/VXLAN header
  - VNI 100
  - inner Ethernet broadcast frame with ethertype 0x0800 (IPv4)
  - virtio-net header marking the outer frame as GSO

## Build

```bash
gcc -o qdisc_uaf_vxlan_repro qdisc_uaf_vxlan_repro.c -static
```

## Run

Inside a test VM with a KASAN-enabled kernel, run the wrapper script:

```bash
./run_vxlan_repro.sh
```

Or run the binary directly (it creates a new network namespace and
requires root):

```bash
./qdisc_uaf_vxlan_repro
```

## Expected behavior

- **Unpatched kernel** (missing `skb_unset_transport_header()` in
  `vxlan_rcv()`): KASAN or page fault in `qdisc_pkt_len_segs_init()`.
- **Patched kernel**: the reproducer completes without crashing because
  `vxlan_rcv()` clears `skb->transport_header` to `~0U` before handing the
  inner frame to the bridge/GRO path.

## Relationship to other reproducers

- `qdisc_uaf/qdisc_uaf_repro_bridge.c` uses `ip6gretap` to trigger the same
  qdisc bug.
- `qdisc_uaf_fix/reproducers/repro_vxlan.sh` is a regression test that
  checks whether `transport_header` is cleared *too early* (before
  `udp_tun_rx_dst()`).  This reproducer is different: it tests whether the
  header is cleared *at all* before the L2 forwarding/qdisc path.
