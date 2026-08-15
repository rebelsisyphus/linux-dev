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

## Same-class paths (theoretical analysis, no reproducer)

The stale-header pattern also exists in other decapsulation paths.  A
follow-up fix (`skb_unset_transport_header()`) was added at the following
sites, validated by code analysis only (no QEMU reproduction):

- `net/l2tp/l2tp_eth.c`: `l2tp_eth_dev_recv()` before `dev_forward_skb()`.
  L2TPv3 (UDP or IP encap) pulls the session header in `l2tp_recv_common()`
  and leaves `transport_header` pointing at the removed outer L4 header; a
  bridged `l2tpeth` device hits the same qdisc path.
- `drivers/net/macsec.c`: `macsec_reset_skb()` now unconditionally unsets
  the header (previously it only reset an *unset* header, so a stale value
  survived SecTAG/ICV stripping), plus the four delivery branches in
  `handle_not_macsec()` that bypass `macsec_reset_skb()` (exact-match
  ANOTHER, multicast clone, promisc ANOTHER, untagged clone).
- `drivers/net/geneve.c`: the unset was *moved* from
  `geneve_udp_encap_recv()` (after `geneve_post_decap_hint()`) into
  `geneve_rx()` just before the delivery point, and is now conditional on
  `!skb->encapsulation`.  The original placement had two problems:
  1. `geneve_rx()` calls `udp_tun_rx_dst()`, which reads the *outer* UDP
     header via `udp_hdr()` (net/ipv4/udp_tunnel_core.c); with the header
     already unset this triggered `DEBUG_NET` warnings and wrote garbage
     ports into the tunnel key (collect_md path).
  2. For GSO packets `geneve_post_decap_hint()` deliberately sets
     `transport_header` to the nested L4 header ("GSO expect a valid
     transport header") and `encapsulation = 1`; unsetting it right after
     discarded that value while it was still consumed by
     `udp_tun_rx_dst()`.
  The moved site keeps the outer-header consumer intact, and the
  `encapsulation` guard preserves the hint-set nested value (the only
  code path that sets `encapsulation` in the encap RX context is
  `geneve_post_decap_hint()`, so the guard is reliable) while still
  clearing the stale outer value before GRO/`netif_rx()` hands the inner
  frame over.

The defensive bounds check added to `qdisc_pkt_len_segs_init()` remains the
global safety net for any future decap path that misses the unset.

