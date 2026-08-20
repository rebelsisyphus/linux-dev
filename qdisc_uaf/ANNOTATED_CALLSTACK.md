# vxlan 本地互发：从传包到 qdisc 崩溃 完整调用栈逐点注释

> 变体：`vxlan_loopback_repro/repro_vxlan_loopback.sh`（真实 TCP 流量，无构造报文）
> 崩溃内核：7.2.0-rc7（unpatched）
> 配套：`vxlan_loopback_repro/ANALYSIS_REPORT.md`（机制/RTL）、`ANALYSIS_REPORT.md`（注入变体）

---

## 0. 全景一图流

```
sendns:                        root:                                testns:
TCP栈生成GSO ─► dev_queue_xmit ─► vxlan_xmit ─► vxlan_build_skb      TCP server
   (TCPV4,MSS)    (vxlanA)        (LOCALBYPASS=off)  │ iptunnel_    :9000
        │                          │                  │ _handle_offloads
        │                          │ iptunnel_xmit(outer UDP/VXLAN)  │
        ▼                          ▼                  │  (+=UDP_TUNNEL)
  外层帧─────vethBp───► vethB─────► 外层GRO ─► ip_rcv ─► udp:4789
                                          (transport_header=外层UDP)   │
                                              ▼    udp_unexpected_gso放行
                                          vxlan_udp_encap_recv ─► vxlan_rcv
                                              │  pull16B/清隧道位/encap=0
                                              ▼  ❌GRO off
                                        gro_cells_receive ─► netif_rx ─► process_backlog
                                              ▼
                                        __netif_receive_skb_core ─► br_handle_frame(br0)
                                              │  FDB查表 → br_forward 克隆
                                              ▼
                                   br_dev_queue_push_xmit ─► __dev_queue_xmit(veth0)
                                              ▼
                                 qdisc_pkt_len_segs_init ←── ⚠ 崩溃点
```

---

## A. 发送半程：TCP → vxlan 封装 → 上线

```
Python sendall(8MB)
  └► sys_sendto → tcp_sendmsg → tcp_write_xmit
      ↳ ① GSO 生成：tcp_transmit_skb 产出 GSO skb
          状态：gso_type=SKB_GSO_TCPV4, gso_size=MSS(~1448), encapsulation=0, transport_header=未设
  └► ip_local_out → ip_output → ip_finish_output2 → neigh_output
  └► dev_queue_xmit(skb)          # 目标10.1.0.2 走 vxlanA 路由
      ↳ __dev_queue_xmit → 查 vxlanA 的 ndo_start_xmit = vxlan_xmit
      └► vxlan_xmit → vxlan_xmit_one
          ↳ ② 关键：RTCF_LOCAL 判定 —— sendns 路由表无本机目标，
            不命中 vxlan_core.c:2328 的 VXLAN_F_LOCALBYPASS 直通分支，
            才真正走外部封装（跨 netns 的意义）
          └► vxlan_build_skb
              ↳ ③ KEY1：iptunnel_handle_offloads (net/ipv4/ip_tunnel_core.c)
                  ├─ skb->encapsulation = 1        # 首次封装开启内层头域
                  └─ gso_type |= SKB_GSO_UDP_TUNNEL  # |＝ 保留 TCPV4 ⇒ TCPV4|UDP_TUNNEL
              └► udp_tunnel_xmit_skb(rt, sock4...)  # 推外层 IPv4/UDP/VXLAN 头
          └► 外层帧经 vethBp 上线（走外层 IP 栈第二次 ip_local_out）
```

**注释①**：`gso_size` 是 TCP 栈给的真实 MSS——本变体"GSO 非伪造"的根基。
**注释②**：VXLAN 默认 `VXLAN_F_LOCALBYPASS`，同机目标会被 `vxlan_encap_bypass` 短路（根本不出网），所以发送方必须放独立 netns 且不含本机路由。
**注释③**：`|=` 是**保留式**叠加，这是让外层 GSO 包能"活着"抵达接收隧道 socket 的前提（对应 4 条件里的 `gso_size≠0`）。

---

## B. 接收半程：外层 GRO → UDP → vxlan_rcv 解封装

```
vethB RX → net_rx_action
  └►（外层 GRO，veth 默认可 GRO）
      └► inet_gro_receive
          ↳ ④ 外层 transport_header 写入：net/ipv4/af_inet.c:1530
            skb_gro_pull(skb, sizeof(*iph));              # data → 外层UDP
            skb_set_transport_header(skb, skb_gro_offset(skb));  # transport_header=外层UDP
          （UDP:4789 为 encap socket → udp4_gro/udp_tunnel_gro_receive 处理内层，合并 GSO）
  └► ip_rcv → ip_local_deliver → __udp4_lib_rcv → udp_queue_rcv_skb
      ↳ ⑤ KEY2：udp_unexpected_gso (include/linux/udp.h:187)
          skb 带 SKB_GSO_UDP_TUNNEL 位 ⇒ 返回 false ⇒ 不切分
          （若无隧道位且是 encap socket ⇒ true ⇒ udp_rcv_segment 切分、gso_size 清零，则不崩）
      └► udp_encap_rcv → vxlan_udp_encap_recv → vxlan_rcv
          ↳ ⑥ KEY3：解封装制造负偏移（drivers/net/vxlan/vxlan_core.c:1712）
              __iptunnel_pull_header(skb, VXLAN_HLEN, ...)
                ├─ 外层 UDP(8) + VXLAN(8) 被拉，data 前移 16
                ├─ iptunnel_pull_offloads (include/net/ip_tunnels.h:649)
                │     gso_type &= ~ENCAP_ALL    # 清 UDP_TUNNEL ⇒ 剩 TCPV4
                │     skb->encapsulation = 0     # ← qdisc 将走外层偏移分支
                └─ transport_header 原地不动     # 相对新 data = -16
          └► gro_cells_receive(&vxlan->gro_cells, skb)
              ↳ ⑦ 交付 vxlan 设备；因 ethtool -K vxlan0 gro off
                 ⇒ 不走内层 GRO 的 inet_gro_receive（否则会把 transport_header
                    重设为内层 TCP，负偏移被救回）⇒ 走 netif_rx → process_backlog
```

**注释④**：这是"transport_header=外层L4"的唯一写入源（GRO，非隧道代码）；负偏移的"负"来自此处记下的正数。
**注释⑤**：隧道包能否维持 GSO 穿越 UDP 层，卡在这里；前面 `|=` 就是专门让它通过。
**注释⑥**：缺陷现场——拉头不重置。此处 skb 状态 =>
`data` 已前移 16，`transport_header` 仍在外层 UDP ⇒ `skb_transport_offset() = -16 = 0xfffffff0`。
**注释⑦**：关 GRO 的必要性（第二个救回点）。

---

## C. bridge 转发 → egress qdisc 崩溃（实测完整栈，逐帧注释）

```
<IRQ>
 [核心崩溃点]
 qdisc_pkt_len_segs_init+0x128/0x300          ── ⚠ 越界读（net/core/dev.c）
 __dev_queue_xmit+0x13a/0x1960                ── egress 发送入口,veth0 的 qdisc
 br_dev_queue_push_xmit+0x77/0x140            ── bridge 克隆 skb 推入转发口队列
 br_handle_frame_finish+0x5f6/0xae0           ── FDB 查表 → br_forward → 本函数
   ? __pfx_vxlan_rcv+0x10/0x10                ──↳ 内联提示：解封装源确为 vxlan_rcv
   ? ip_local_deliver_finish+0x129/0x1e0
   ? ip_local_deliver+0xfb/0x1e0
   ? ip_rcv+0x243/0x260                        ──↳ 外层 IP 递交给 vxlan socket 的链
 br_handle_frame+0x289/0x410                  ── br0 对 vxlan0 端口的 rx_handler
 __netif_receive_skb_core.constprop.0+0x698/0x1440  ── 收包核心：先 rx_handler 后协议分发
 __netif_receive_skb_one_core+0x91/0x130      ── 单包入口
 process_backlog+0x110/0x260                  ── backlog 软中断（vxlan0 交付=netif_rx，GRO 已关）
 __napi_poll+0x53/0x2c0
 net_rx_action+0x5ad/0x690
 handle_softirqs ... do_softirq               ── 软中断上下文
</IRQ>
```

### qdisc_pkt_len_segs_init 逐行对上 4 条件（net/core/dev.c）

```c
qdisc_skb_cb(skb)->pkt_len = skb->len;
if (!shinfo->gso_size) { pkt_segs=1; return; }  // ① gso_size=MSS≠0 ✓（TCP 栈真实值）
qdisc_skb_cb(skb)->pkt_segs = gso_segs = shinfo->gso_segs;

if (!skb->encapsulation)                          // ③ encapsulation=0 ✓（vxlan 已清）
    hdr_len = skb_transport_offset(skb);          // ④ = -16 → 无符号 0xfffffff0
else
    hdr_len = skb_inner_transport_offset(skb);

if (likely(shinfo->gso_type & (SKB_GSO_TCPV4|SKB_GSO_TCPV6))) {   // ② TCPV4 ✓（保留）
    th = (const struct tcphdr *)(skb->data + hdr_len);   // 野指针 = data - 16 + ...
    tlen = __tcp_hdrlen(th);                        // ← __asan_load2 读 th->doff → Oops
    ...
}
```

实测寄存器：`R12: 00000000fffffff0`（即 -16），与"外层 UDP8+VXLAN8 被拉"完全对应。
崩溃性质：`pkm→data + 0xfffffff0` 落到已分配的 kmalloc 缓冲区外 → KASAN slab 越界 / 页错误。

---

## D. skb 状态机一览（关键量全程变化）

| 环节 | data | transport_header | gso_type | encap | gso_size | 备注 |
|---|---|---|---|---|---|---|
| TCP 生成 | 内层IP | (未设) | TCPV4 | 0 | MSS | ① 真实值 |
| handle_offloads | 内层IP | (未设) | TCPV4｜UDP_TUNNEL | 1 | MSS | ③ |= |
| 外层上线 | 外层IPv4 | (未设) | 同上 | 1 | MSS | |
| 外层GRO | 外层UDP | =外层UDP | 同上 | 1 | MSS | ④ 写入位置 |
| vxlan_rcv pull16 | +16 | 仍在+0 | 只剩TCPV4 | **0** | MSS | ⑤⑥ 偏差-16 |
| bridge 转发 clone | +16 | 偏移**-16** | TCPV4 | 0 | MSS | ⑦ 不重置 |
| qdisc 崩溃 | — | hdr_len=0xfffffff0 | TCPV4 | 0 | MSS | ⚠ |

> 表中"不重置"发生在 `vxlan_rcv`（pull）与 bridge 转发（克隆）两处——这正是缺陷与"为什么 bridge 转发/GRO-off 必要"的根源。

---

## E. 两个"救回点"为什么必须被绕过

| 救回点 | 动作 | loopback 如何绕过 |
|---|---|---|
| 内层 GRO `inet_gro_receive`(vxlan0) | 重设 transport_header=内层TCP | `ethtool -K vxlan0 gro off` |
| 本地 IP 栈 `ip_rcv_core` | 重设 transport_header | bridge L2 转发（不进本地栈） |

外加第三处：**发送侧 LOCALBYPASS**（本机直通），用跨 netns 绕开。三者缺一，4 条件无法同时成立。

---

## F. 源头定位（供 Fixes/评审引用）

- 负偏移**状态**：GRE/TEB 驱动收包"拉头不重置 transport_header"自内核 2.6.12 导入（2005，`1da177e4c3f4`）；vxlan 驱动首发（2012）沿用同一模式。
- 负偏移→**UAF 崩溃**：`7fb4c1967011`（2026-04-03，"net: pull headers in qdisc_pkt_len_segs_init()"）把 `skb_header_pointer` 换成 `pskb_may_pull`+直接解引用后必现。
- 修复：v1 `iptunnel_rebuild_transport_header`（GRE/TEB）；v2/v3 各隧道交付前 `skb_unset_transport_header()`（vxlan_rcv 等）；qdisc 侧 `pskb_may_pull`+边界检查兜底。

---

## G. 复现速查

```bash
# 需要 root + CONFIG_VXLAN/BRIDGE/VETH
cd qdisc_uaf/vxlan_loopback_repro
./repro_vxlan_loopback.sh            # 默认发 8MB；可传字节数
# unpatched 7.2.0-rc7：VM panic（serial_vxlan_loopback_rc7_panic.log）
# patched（skb_unset_transport_header in vxlan_rcv）：传输正常完成、无 panic
```
