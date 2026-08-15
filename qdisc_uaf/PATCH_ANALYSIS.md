# Patch Analysis and Work Plan

## Background

The `qdisc_pkt_len_segs_init()` use-after-free / out-of-bounds read is caused by
a stale `transport_header` offset left over after tunnel decapsulation.  When a
tunnel receiver pulls the outer headers, `skb->data` advances to the inner frame
but `skb->transport_header` is not updated.  It still points to the outer L4
header (GRE/UDP), which is now before `skb->data`.  Any downstream code that
trusts `skb_transport_header_was_set()` and dereferences the offset reads from
invalid memory.

Our local reproduction shows the bug is **broader than the original patch
authors assumed**:

* It is **not limited to malformed inner frames** (e.g. `ETH_P_802_2`).
* Even **normal IPv4/IPv6 inner frames** trigger the crash when they are
  forwarded at L2 (bridge, HSR) because the forwarding clone reaches
  `__dev_queue_xmit()` before the local IP/GRO path can reset
  `transport_header`.

Two upstream patches attempted to fix this.  Both have problems.

---

## Patch v1: Jiayuan Chen

**Subject:** `[PATCH net v2] net: iptunnel: fix stale transport header after GRE/TEB decap`

### What it does

* Adds `iptunnel_rebuild_transport_header()` in `include/net/ip_tunnels.h`.
* Calls it from `ip_tunnel_rcv()` and `__ip6_tnl_rcv()`.
* For GSO packets only:
  1. Resets `transport_header` to `~0U`.
  2. Re-probes the inner transport header with the flow dissector.
  3. If re-probing fails, clears GSO metadata via `skb_gso_reset()`.

### Problems

1. **Over-engineered and maintainer push-back**
   Eric Dumazet rejected the "mangle instead of fix" approach:
   > "I do not think this makes sense. What is a valid case for this packet
   > being processed further? The buggy packet must be dropped, instead of
   > being mangled like this."

2. **Does not address the root cause**
   The patch tries to repair the stale offset downstream instead of restoring
   the skb invariant at the decapsulation boundary.

3. **Coverage is too narrow**
   Only handles GRE/TEB (`ip_tunnel_rcv` / `__ip6_tnl_rcv`).  VXLAN, Geneve,
   and other tunnels that pull outer headers have the same issue.

4. **Assumes only GSO packets are affected**
   Our reproduction shows that bridge/HSR forwarding can carry the stale offset
   into `qdisc_pkt_len_segs_init()` regardless of whether the reproducer uses
   GSO injection.  The bug class is a stale skb header, not a GSO-specific
   corner case.

5. **Adds fast-path cost**
   Even well-formed inner IPv4 traffic runs through the flow dissector on the
   GRE receive path.

---

## Patch v2: Eric Dumazet

**Subject:** `[PATCH net] net: clear transport header during tunnel decapsulation`

### What it does

* Introduces `skb_unset_transport_header()` helper in `include/linux/skbuff.h`.
* Calls it in two places:
  1. `__iptunnel_pull_header()` (common pull routine used by GRE, IPIP, SIT,
     Geneve, VXLAN, etc.)
  2. `vxlan_rcv()` in `drivers/net/vxlan/vxlan_core.c`
* Clears `transport_header` to `~0U` so downstream consumers see
  `!skb_transport_header_was_set()` and skip validation.

### Problems

Patch v2 has the right idea (restore the skb invariant at the decap boundary)
but the **placement is wrong**.

#### 1. Geneve regression: `WARNING in geneve_udp_encap_recv`

`geneve_udp_encap_recv()` does:

```c
__iptunnel_pull_header(...);          // v2 clears transport_header here
// ...
geneveh = geneve_hdr(skb);             // uses udp_hdr(skb)
```

`geneve_hdr()` indirectly accesses `skb_transport_header()` through `udp_hdr()`.
Because v2 clears the header inside `__iptunnel_pull_header()`, the subsequent
`udp_hdr()` read triggers:

```text
!skb_transport_header_was_set(skb)
WARNING: ./include/linux/skbuff.h:3094 at geneve_udp_encap_recv+...
```

**syzbot CI reported this immediately.**

#### 2. VXLAN regression: `KASAN: slab-use-after-free in vxlan_rcv`

`vxlan_rcv()` also goes through `__iptunnel_pull_header()`, so v2 already
unsets the transport header there.  v2 then **unsets it a second time** inside
`vxlan_rcv()` itself.  Worse, `vxlan_rcv()` and the functions it calls still
expect the outer UDP header to be reachable via `udp_hdr(skb)`; clearing the
header causes it to compute a bogus offset and read freed memory.

**Kernel Test Robot reproduced this in LTP `net.features`.**

#### 3. `__iptunnel_pull_header()` is a shared primitive

Many tunnel types call this function at different stages of their receive
paths.  Some still need the outer transport header after the pull.  A single
unconditional clear in the common helper is too aggressive.

---

## What Our Reproduction Adds

Our HSR and bridge reproducers demonstrate that the bug is **not about
malformed inner protocols**:

* The inner Ethernet frame can use a normal `ETH_P_IP` ethertype.
* Disabling GRO is unnecessary.
* The crash happens because the bridge/HSR forwarding clone enters
  `__dev_queue_xmit()` while `transport_header` still points behind `skb->data`.

This means any fix that only drops or resets GSO for "unparseable" inner
frames is insufficient.  The correct fix must restore the skb invariant for
**all** decapsulated packets, regardless of inner protocol.

---

## Recommended Solution

### Principle

Clear `transport_header` to `~0U` **after** each tunnel type has finished all
processing that needs the outer L4 header, but **before** the skb is handed to
the network stack / GRO.

### Why this is the right place

* Downstream IP/GRO code will set `transport_header` correctly when it parses
  the inner packet.
* L2 forwarding paths (bridge, HSR) do not need `transport_header`; leaving it
  unset is safe and matches the `skb_transport_header_was_set()` contract.
* `qdisc_pkt_len_segs_init()` already returns early when the header is not set.

### Proposed patch structure

1. Keep the `skb_unset_transport_header()` helper from v2.  It is useful and
   correctly named.

2. **Remove** the `skb_unset_transport_header()` call from
   `__iptunnel_pull_header()`.  This location is too early for Geneve/VXLAN.

3. **Remove** the redundant/dangerous call from `vxlan_rcv()`.

4. Add calls at the end of each tunnel receive function, after the outer
   header is no longer needed:

| Tunnel type | Function | Insertion point |
|-------------|----------|-----------------|
| GRE/TEB IPv4 | `ip_tunnel_rcv()` | After `eth_type_trans()` / inner-frame setup, before `gro_cells_receive()` |
| GRE/TEB IPv6 | `__ip6_tnl_rcv()` | After inner-frame setup, before `gro_cells_receive()` |
| VXLAN | `vxlan_rcv()` | After all `udp_hdr()` / VXLAN header parsing, before `gro_cells_receive()` |
| Geneve | `geneve_udp_encap_recv()` | After `geneve_hdr()` / outer-UDP usage, before delivering to stack |

5. Optional safety net in `__netif_receive_skb_core()`:

```c
if (!skb_transport_header_was_set(skb) ||
    skb_transport_offset(skb) < 0)
    skb_reset_transport_header(skb);
```

This catches any future path that forgets to clear the header, at the cost of
one extra check on the receive fast path.

---

## Work Plan

### Phase 1: Code analysis

* Locate all call sites of `__iptunnel_pull_header()`.
* Locate every use of `udp_hdr()` / `skb_transport_header()` in
  `geneve_udp_encap_recv()` and `vxlan_rcv()`.
* Determine the exact line where each tunnel type no longer needs the outer
  transport header.

### Phase 2: Reproduce patch v2 CI regressions

* Apply patch v2 to the current tree.
* Build and boot with `test-kernel`.
* Run a minimal Geneve/VXLAN smoke test (or LTP `net.features` if available).
* Confirm:
  * Geneve warning (`!skb_transport_header_was_set`).
  * VXLAN KASAN use-after-free.

### Phase 3: Implement corrected patch

* Add `skb_unset_transport_header()` helper.
* Remove bad calls from `__iptunnel_pull_header()` and `vxlan_rcv()`.
* Add correct calls in `ip_tunnel_rcv()`, `__ip6_tnl_rcv()`, `vxlan_rcv()`,
  and `geneve_udp_encap_recv()`.
* Build and fix any compile errors.

### Phase 4: Verification

Run the following in `test-kernel`:

1. **Original bug** (must NOT panic):
   * `qdisc_uaf_repro_hsr`
   * `qdisc_uaf_repro_bridge`

2. **Patch v2 CI regressions** (must NOT trigger):
   * Geneve receive path: no `!skb_transport_header_was_set` warning.
   * VXLAN receive path: no KASAN UAF.
   * LTP `net.features` passes (if available).

3. **Regression smoke test**:
   * Basic GRE/IPIP/SIT forwarding still works.
   * Bridge and HSR forwarding still work for normal traffic.

### Phase 5: Documentation

* Update this file with the final patch diff and test results.
* Update `REPRO.md` if the recommended fix differs materially from earlier
  notes.

---

## Files and Logs

| File | Description |
|------|-------------|
| `maillist-patch/patch-v1/0001.patch` | Jiayuan Chen's v1 patch |
| `maillist-patch/patch-v1/comment1.mbox` | Eric Dumazet's review of v1 |
| `maillist-patch/patch-v1/comment2.mbox` | Paolo Abeni's review + Jiayuan's reply |
| `maillist-patch/patch-v2/0001.patch` | Eric Dumazet's v2 patch |
| `maillist-patch/patch-v2/comment1.mbox` | Eric's reply about Fixes tag |
| `maillist-patch/patch-v2/comment2.mbox` | Paolo Abeni's review + syzbot CI Geneve warning |
| `maillist-patch/patch-v2/comment3.mbox` | Kernel Test Robot VXLAN UAF report |
| `PATCH_ANALYSIS.md` | This document |

---

## Open Questions

1. Should we also clear `transport_header` in other tunnel types that use
   `__iptunnel_pull_header()` (IPIP, SIT, etc.)?  They may not use the outer
   header after the pull, so the common helper change might be safe for them,
   but per-tunnel placement is safer.

2. Do we want the optional safety net in `__netif_receive_skb_core()`?  It
   adds a small fast-path check but prevents future similar bugs.

3. Should the corrected patch be submitted upstream as a v3, or as a new
   independent patch?  This depends on whether we can keep Eric's helper and
   reuse his Signed-off-by.
