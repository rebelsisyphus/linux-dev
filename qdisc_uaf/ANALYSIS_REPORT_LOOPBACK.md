# vxlan 本地互发（loopback）复现路径分析报告

> qdisc_pkt_len_segs_init() 陈旧 transport_header UAF —— 无构造报文变体
> 内核：7.2.0-rc7（unpatched），实测 panic：`serial_vxlan_loopback_rc7_panic.log`
> 复现脚本：`repro_vxlan_loopback.sh`

---

## 0. 一句话结论

`vxlan_loopback` 变体用**纯普通 TCP 流量**复现同一个 `qdisc_pkt_len_segs_init` UAF：
TCP 栈天然生成的 GSO 元数据（`SKB_GSO_TCPV4 + gso_size`）经 vxlan 封装（保留）→ 解封装（还原成裸 TCPv4 并制造 -16 负偏移）→ bridge L2 转发 → egress qdisc 越界读。全程无 tun/virtio 注入、无伪造报文、无 ingress qdisc，证明"管理员配置 + 普通大流量"即可触发。

---

## 1. 与注入变体的本质区别

| 维度 | `vxlan_repro`（注入变体） | `vxlan_loopback`（本报告） |
|---|---|---|
| GSO 元数据来源 | 用户态 `virtio_net_hdr` 伪造 | **TCP 栈运行时生成**（真实） |
| 报文 | 手工构造 1 个 200B 帧 | 8MB 普通 TCP bulk 传输 |
| 触发设备 | TUN/TAP + vnet header | vxlan 设备 + bridge |
| 必需开关 | — | 跨 netns 绕 `LOCALBYPASS`、vxlan0 关 GRO |
| 攻击面含义 | 有注入能力的环境 | **root/CAP_NET_ADMIN 即可（容器逃逸后）** |

两变体崩溃机制完全一致（`R12 = 0xfffffff0` 均出现）。

---

## 2. 拓扑与三个"必须"开关

```
sendns(发送)              root(接收+桥)                  testns(收内层)
vxlanA 10.1.0.1/24        vxlan0(id100, GRO off)─┐      veth1 10.1.0.2/24
 local 192.168.99.1            +── bridge br0 ──+       (TCP server :9000)
 remote 10.0.0.1 ──vethBp──vethB 192.168.99.2 ──┘──veth0♯─veth1
                     lo 10.0.0.1/32
```

**① 跨 netns —— 绕开 VXLAN_F_LOCALBYPASS（vxlan_core.c:2328）**
VXLAN 默认开启本地直通：目的路由带 `RTCF_LOCAL` 且开 `VXLAN_F_LOCALBYPASS` 时，`vxlan_xmit_one` 直接把内层帧本地交付（`vxlan_encap_bypass`），**根本不会产生外层封装包**，GSO 会被本地 GRO 救回。把发送方放进独立 netns、使其路由表不含本机目标（`ip route add 10.0.0.1/32 via 192.168.99.2`），才能真正把封装帧发上线。—— iproute2 没有关 LOCALBYPASS 的开关，跨 netns 是单机唯一办法。

**② 接收侧 vxlan0 关 GRO**（`ethtool -K vxlan0 gro off`）
若开着 GRO，内层 IPv4 会再跑一遍 `inet_gro_receive()`，其 `skb_set_transport_header(skb, skb_gro_offset(skb))` 会把 transport_header 重设为**内层 TCP 头**（正偏移），负偏移被"救回"。关 GRO 后内层帧直接 `__netif_receive_skb` → bridge 转发，transport_header 保持负值。

**③ 内层帧必须被 bridge L2 转发（不进本地 IP 栈）**
若内层帧本机消费，`ip_rcv_core()` 会重设 transport_header。vxlan0 与 veth0 同挂 br0，内层帧被 `br_forward` 克隆进 egress qdisc——`transport_header` 重设环节完全被跳过。

---

## 3. GSO 元数据完整生命周期（TX→RX 逐环节）

### 3.1 生成（内核真值，非伪造）
TCP 发送侧做 8MB bulk 传输，TCP 栈正常产出 GSO skb：
```
SKB_GSO_TCPV4 + gso_size = MSS（如 1448×32 段/帧）
```

### 3.2 封装保留（vxlan_build_skb → iptunnel_handle_offloads）
`net/ipv4/ip_tunnel_core.c`：
```c
int iptunnel_handle_offloads(struct sk_buff *skb, int gso_type_mask)
{
	if (likely(!skb->encapsulation)) {
		skb_reset_inner_headers(skb);
		skb->encapsulation = 1;
	}
	if (skb_is_gso(skb)) {
		skb_header_unclone(...);
		skb_shinfo(skb)->gso_type |= gso_type_mask;  // |= SKB_GSO_UDP_TUNNEL
		return 0;
	}
	...
}
```
`|=` 是**保留式叠加**：内层 `TCPV4` 位不动，外层追加 `SKB_GSO_UDP_TUNNEL` → `gso_type = TCPV4|UDP_TUNNEL`。这一步是"GSO 能活着到接收端"的前提（见 3.4）。

### 3.3 上线 + 接收
外层帧沿 vethB 发出 → root 的 lo/vethB 上收到外层 IPv4/UDP(4789) → GRO 写 `transport_header = 外层 UDP` → `vxlan_udp_encap_recv` → `vxlan_rcv`。

### 3.4 UDP 层"不切分"判定（udp_unexpected_gso）
`include/linux/udp.h:187`：
```c
static inline bool udp_unexpected_gso(struct sock *sk, struct sk_buff *skb)
{
	...
	if (udp_encap_needed() && READ_ONCE(udp_sk(sk)->encap_rcv) &&
	    !(skb_shinfo(skb)->gso_type &
	      (SKB_GSO_UDP_TUNNEL | SKB_GSO_UDP_TUNNEL_CSUM)))
		return true;                    // ← 缺隧道位 → 切分，gso_size 清零
	return false;                            // ← 带隧道位 → 放行原文
}
```
GTP/VXLAN socket 是 encap socket（`encap_rcv` 非空）。正因为 3.2 把 `UDP_TUNNEL` 位 |= 上了，这里返回 false：**GSO skb 不被 `udp_rcv_segment()` 切分**，`gso_size/gso_type` 原样抵达解封装。
（对照：注入变体必须在 virtio_net_hdr 里显式带上 `UDP_TUNNEL_IPV4` 位，同样是过这一关。）

### 3.5 解封装还原 + 制造负偏移（vxlan_rcv）
`drivers/net/vxlan/vxlan_core.c`：
```c
__iptunnel_pull_header(skb, VXLAN_HLEN, protocol, raw_proto, ...);  // 拉 16B: UDP8+VXLAN8
...
gro_cells_receive(&vxlan->gro_cells, skb);                          // 交付 vxlan 设备
```
`__iptunnel_pull_header` 内的 `iptunnel_pull_offloads`（include/net/ip_tunnels.h:649）：
```c
if (skb_is_gso(skb)) {
	skb_unclone(...);
	skb_shinfo(skb)->gso_type &= ~(NETIF_F_GSO_ENCAP_ALL >> NETIF_F_GSO_SHIFT);
}                                        // 清掉 UDP_TUNNEL 外层位
skb->encapsulation = 0;                  // ← 关键：qdisc 因此走外层偏移分支
```
效果 = **清外层隧道位、置 encapsulation=0、保留 TCPV4 + gso_size、transport_header 停在已被移除的外层 UDP 上** → `skb_transport_offset() = -16`（外层 UDP8+VXLAN8 被拉掉）。

### 3.6 崩溃（bridge 转发 → egress qdisc）
vxlan0 的 gro cell NAPI（`gro_cell_poll`）→ `__netif_receive_skb_core` → br0 对 vxlan0 的 rx_handler `br_handle_frame` → `br_handle_frame_finish` + FDB 查表 → `br_forward` 克隆 → `br_dev_queue_push_xmit` → `__dev_queue_xmit` → `qdisc_pkt_len_segs_init`：

```c
if (!shinfo->gso_size) return;                       // 1) gso_size≠0 ✓（来自真实 TCP）
if (!skb->encapsulation)                             // 3) encapsulation==0 ✓（3.5 已置 0）
	hdr_len = skb_transport_offset(skb);           // 4) -16 → 0xfffffff0
if (gso_type & (SKB_GSO_TCPV4|SKB_GSO_TCPV6)) {      // 2) TCPV4 ✓（3.5 保留）
	th = (const struct tcphdr *)(skb->data + hdr_len);  // 野指针
	hdr_len += __tcp_hdrlen(th);                   // __asan_load2 → Oops
}
```

---

## 4. 实测现场（serial_vxlan_loopback_rc7_panic.log）

```
BUG: unable to handle page fault for address: ffffed1028d37ddd
RIP: 0010:__asan_load2+0x48/0xa0
 ...
 R12: 00000000fffffff0          ← skb_transport_offset() = -16
 Call Trace:
  <IRQ>
   qdisc_pkt_len_segs_init+0x128/0x300
   __dev_queue_xmit+0x13a/0x1960
   br_dev_queue_push_xmit+0x77/0x140
   br_handle_frame_finish+0x5f6/0xae0
    ? __pfx_vxlan_rcv+0x10/0x10        ← 内联提示：vxlan 解封装已完成
    ? ip_local_deliver_finish+0x129/0x1e0
    ? ip_local_deliver+0xfb/0x1e0
    ? ip_rcv+0x243/0x260
   br_handle_frame+0x289/0x410
   __netif_receive_skb_core.constprop.0+0x698/0x1440
   __netif_receive_skb_one_core+0x91/0x130
   process_backlog+0x110/0x260       ← 内层经 __netif_rx/backlog 交付（GRO 已关）
   __napi_poll ... net_rx_action ...
Kernel panic - not syncing: Fatal exception in interrupt
```

特征：
- 崩溃点是 egress `__dev_queue_xmit`（bridge 转发克隆），中断上下文；
- `? ip_rcv / ip_local_deliver / vxlan_rcv` 是内联优化残留的调用点提示——即解封装链确为 `ip_rcv → ip_local_deliver → vxlan_rcv`；
- `process_backlog` 证明 vxlan0 交付走 `__netif_rx`/backlog（GRO 关闭的直接结果）；
- `R12 = 0xfffffff0` 与"外层 UDP+VXLAN 共 16B 被拉"精确吻合。

---

## 5. 为什么这些条件环环相扣（因果链）

```
LOCALBYPASS 绕开（跨netns） → 才有真实封装帧
  → iptunnel_handle_offloads |= UDP_TUNNEL → 才有"带隧道位"的 GSO
    → udp_unexpected_gso 放行（不切分） → gso_size 不丢
      → iptunnel_pull_offloads 清隧道位、置 encapsulation=0、保留 TCPV4
        → qdisc 走外层偏移分支 + gso_size≠0 + TCPV4 命中
          → transport_header=-16（vxlan 拉 16B 不重置）
            → 关 GRO（避免内层 GRO 救回） + bridge 转发（避免 ip_rcv 救回）
              → qdisc_pkt_len_segs_init 越界读 = UAF
```

任一环节缺失都不会崩（例如开 GRO、本地交付、或 GSO 被切分）。

---

## 6. 攻击面结论

本变体所需的全部能力 = root / CAP_NET_ADMIN：
- 3 个 netns + veth + vxlan + bridge 的 iproute2 配置；
- `ethtool -K gro off`；
- 一段普通 TCP 流量（8MB）。

无需自定义注入程序、无需 tun/virtio、API 层无任何"越界"操作——这正是容器逃逸/提权后的典型操作集。缺陷本身是 2005 年以来 GRE/GRE 系隧道"拉头不重置 transport_header"的远古残留（引入对照见 `olk510_repro/README.md`），2026 年 `7fb4c1967011` 使 qdisc 直接解引用后才变成可致内核 panic 的 UAF。

---

## 7. 相关文件

| 文件 | 用途 |
|---|---|
| `repro_vxlan_loopback.sh` | 复现脚本（含全部配置与 TCP 传输） |
| `serial_vxlan_loopback_rc7_panic.log` | 实测 panic（7.2.0-rc7） |
| `README.md` | 触发模型要点 |
| `../vxlan_repro/ANALYSIS_REPORT.md` | 注入变体对照分析 |
| `../gtp_repro/` & `../olk510_repro/` | 同源变体与 OLK-5.10 回迁复现 |
