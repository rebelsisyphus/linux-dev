#### 🟠 High Findings

1. **Incomplete fix: same stale-transport-header bug remains in `drivers/net/amt.c` and `drivers/net/pfcp.c`**
   - **Location**: `drivers/net/amt.c:2315`, `drivers/net/amt.c:2393`, `drivers/net/amt.c:2503`, `drivers/net/pfcp.c:97`
   - **Description**: Both drivers use `iptunnel_pull_header()` to strip outer tunnel headers and then pass the resulting skb to `gro_cells_receive()` (or `__netif_rx`), but they never clear the stale `transport_header`. `iptunnel_pull_header()` clears `skb->encapsulation`, so any GSO packet that subsequently gets forwarded at L2 and queued will hit the same negative-offset calculation in `qdisc_pkt_len_segs_init()`.
   - **Impact**: Same class of KASAN/page-fault crash as the one this patch fixes. For AMT, the affected path is `amt_mcast_data_handler()` which receives encapsulated multicast data and forwards it into GRO. For PFCP, `pfcp_encap_recv()` decapsulates a PFCP-encapsulated packet and passes it to `gro_cells_receive()`. Both can satisfy the four conditions described in the commit message.
   - **Fix**: Add `skb_unset_transport_header(skb)` immediately before the `gro_cells_receive()` call in AMT and PFCP, mirroring the pattern used in this patch. For AMT, the relevant sites are:
     ```c
     // drivers/net/amt.c:amt_mcast_data_handler(), after line 2357
     skb_unset_transport_header(skb);
     err = gro_cells_receive(&amt->gro_cells, skb);
     ```
     For PFCP:
     ```c
     // drivers/net/pfcp.c:pfcp_encap_recv(), after line 95
     skb_unset_transport_header(skb);
     gro_cells_receive(&pfcp->gro_cells, skb);
     ```
     The `amt_update_handler()` path uses `__netif_rx()` and does not currently reset the transport header either; it should also be audited for stale-header exposure.

#### 🟡 Medium Findings

1. **No new helper usage in `skb_scrub_packet()` despite the function being a generic decapsulation scrubber**
   - **Location**: `net/core/skbuff.c:skb_scrub_packet()`
   - **Description**: `skb_scrub_packet()` is documented as usable after decapsulating a packet, but it does not clear `transport_header`. Adding the unset there would be too broad because it is also used after encapsulation and in cross-namespace injection, but it highlights that the chosen boundary-only fix requires manually auditing every tunnel driver. A centralized solution (e.g. inside `iptunnel_pull_offloads()` or `iptunnel_pull_header()`) would be less error-prone.
   - **Impact**: Medium. The current approach is correct but fragile; future tunnel drivers can re-introduce the bug.
   - **Fix**: Consider unsetting `transport_header` inside `iptunnel_pull_header()`/`__iptunnel_pull_header()` after clearing `skb->encapsulation`, or add a warning comment to those helpers reminding callers to reset/unset transport header before passing to GRO.

#### 🔵 Low / Nits

1. **Commit message could mention the defensive check in `qdisc_pkt_len_segs_init()`**
   - The current commit message explains the crash well. A short note that the patch pairs with the existing `skb_transport_header_was_set()` check in `qdisc_pkt_len_segs_init()` would make the rationale clearer.

2. **FOU uses `skb_reset_transport_header()` instead of `skb_unset_transport_header()`**
   - `net/ipv4/fou_core.c:69` resets the transport header to `skb->data`. This is not equivalent to unsetting it: `skb_transport_header_was_set()` will return true, and a forwarded FOU packet could still compute a wrong transport offset (zero) in `qdisc_pkt_len_segs_init()`. FOU does not use `gro_cells_receive()`, so it is outside the scope of this patch, but it is worth auditing separately.
