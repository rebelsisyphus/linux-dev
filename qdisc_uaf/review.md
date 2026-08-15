#### Medium Findings

1. **Other UDP tunnels may need the same treatment, but only if they carry Ethernet inner frames**
   - **Location**: `drivers/net/bareudp.c:195`, `drivers/net/pfcp.c:97`, `drivers/net/gtp.c:340`, `drivers/net/amt.c:2359`
   - **Description**: Several other tunnel drivers call `iptunnel_pull_header()` and then hand the skb to `gro_cells_receive()` / `__netif_rx()` without clearing `transport_header`. The same stale-header hazard exists in principle.
   - **Impact**: For the reported crash to occur, the decapsulated inner frame must be forwarded at L2 by a bridge, which requires the inner protocol to be `ETH_P_TEB`.  `bareudp` restricts `multiproto` mode to `ETH_P_IP`/`ETH_P_MPLS_UC`, but in non-multiproto mode it accepts any ethertype, including `ETH_P_TEB`.  If a bareudp device is configured with `ETH_P_TEB`, enslaved to a bridge, and receives a GSO-encapsulated UDP packet, the inner Ethernet frame can be forwarded at L2 with a stale outer UDP `transport_header` and hit the same crash.  GTP carries IP/IPv6 and the inner packet passes through `ip_rcv_core()`, which resets the transport header.  PFCP is control-plane traffic and not typically GSO-bearing.  AMT builds Ethernet frames for multicast but the inner payload is IP and is delivered through normal IP receive (and AMT explicitly resets `transport_header` on its membership-query path).
   - **Conclusion**: `bareudp` is an exploitable omission for the same bug class and has been fixed by adding `skb_unset_transport_header()` after ECN decapsulation and before `gro_cells_receive()`.  GTP, PFCP, and AMT are not currently exploitable.
   - **Recommendation**: Consider a brief code comment or follow-up audit documenting that any tunnel using `iptunnel_pull_header()` and then forwarding to L2 must clear the transport header.

2. **VXLAN build failure is pre-existing, not introduced by the patch**
   - **Location**: `drivers/net/vxlan/vxlan_core.c`
   - **Description**: Attempting to build `drivers/net/vxlan/vxlan_core.o` fails with redefinition errors for `vxlan_fdb_find_uc`, `vxlan_fdb_replay`, and `vxlan_fdb_clear_offload`. The same failure occurs on the unpatched base tree.
   - **Impact**: This prevents a clean compile test of the VXLAN portion of the change in this workspace. It is unrelated to the patch content.
   - **Recommendation**: Re-test the VXLAN build after the unrelated header/implementation conflict is resolved in the base tree.

#### Low / Nits

1. **Helper lacks kerneldoc comment**
   - **Location**: `include/linux/skbuff.h:3085-3088`
   - **Description**: `skb_unset_transport_header()` is added without a doc comment. Neighboring helpers such as `skb_reset_transport_header_careful()` have kerneldoc.
   - **Impact**: Negligible; the function name and single-line body are self-documenting.
   - **Fix suggestion**: Optional follow-up to add a short `/** ... */` block describing when to call the helper.

2. **Geneve comment is slightly misleading after the change**
   - **Location**: `drivers/net/geneve.c:709-712`
   - **Description**: The comment says "After hint processing, the transport header points to the inner one and we can't use anymore on geneve_hdr()." Immediately after, the patch unsets the transport header. The comment remains technically true (the header did point to the inner one during hint processing), but a reader might wonder why it is then discarded.
   - **Impact**: Low; does not affect correctness.
   - **Fix suggestion**: Optional comment update clarifying that the inner transport header is intentionally invalidated before L2/GRO handoff to prevent stale offsets in forwarding paths.
