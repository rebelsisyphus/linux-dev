# VXLAN 复现路径分析报告

> qdisc_pkt_len_segs_init() 陈旧 transport_header UAF（syzbot 83181a31）
> 本文基于实测 panic 日志（unpatched kernel）与源码逐行核对，聚焦 VXLAN 收包 + 桥接转发路径。

## 0. 结论摘要

VXLAN 路径复现的是同一缺陷`qdisc_pkt_len_segs_init()` 越界读（UAF），触发模型与 GTP/HSR/bridge 变体一致，仅**生产端（谁把 GSO 元数据和负偏移送到 qdisc）**不同：

| 环节 | VXLAN 路径 | GTP 路径 |
|---|---|---|
| 负偏移制造者 | `vxlan_rcv()` 拉掉外层 UDP+VXLAN(16B) | `gtp_rx()` 拉掉 UDP+GTP-U(16B) |
| 消费点 | bridge 转发 egress qdisc | gtp0 的 ingress qdisc |
| GSO 元数据来源 | 注入的 virtio_net_hdr / 真实流量 GRO | 注入的 virtio_net_hdr |

核心不变式：**`encapsulation==0` 且带 TCPv4 GSO 的 skb 带着一个负的 `transport_header` 进入 `qdisc_pkt_len_segs_init()`**。

---

## 1. 触发模型（4 个必要条件）

`qdisc_pkt_len_segs_init()`（本树 net/core/dev.c，未拆分前为 `qdisc_pkt_len_init`）：

```c
qdisc_skb_cb(skb)->pkt_len = skb->len;
if (!shinfo->gso_size) return;                    // (1) gso_size != 0
qdisc_skb_cb(skb)->pkt_segs = gso_segs = ...;
if (!skb->encapsulation)                          // (3) encapsulation == 0
    hdr_len = skb_transport_offset(skb);          // (4) transport_header 负偏移 → 下溢
if (gso_type & (SKB_GSO_TCPV4|SKB_GSO_TCPV6)) {   // (2) TCPv4/TCPv6 GSO
    th = (const struct tcphdr *)(skb->data + hdr_len);  // ← 野指针
    hdr_len += __tcp_hdrlen(th);                  // ← __asan_load2 崩溃
}
```

四个条件：
1. `shinfo->gso_size != 0` —— 否则提前返回；
2. `gso_type & (SKB_GSO_TCPV4|SKB_GSO_TCPV6)` —— 进入解引用分支；
3. `skb->encapsulation == 0` —— 才使用外层 `skb_transport_offset`；
4. `transport_header` 为负偏移（指向已被 pull 掉的外层 L4 头）。

---

## 2. 拓扑

```
 /dev/net/tun (syz_tun, IFF_VNET_HDR)   ← 注入 outer VXLAN 帧
        │  tun_get_user
        ▼
  IP/UDP 栈 → vxlan_udp_encap_recv → vxlan_rcv()   ← 解封装（产生负偏移）
        │  gro_cells_receive(vxlan0)
        ▼
  vxlan0 ──(br0 端口1)── bridge(br0)
        │  br_handle_frame → br_forward
        ▼
  veth0 ──► __dev_queue_xmit → qdisc_pkt_len_segs_init()  ← 崩溃
```

- `tun0`：TAP/TUN，`IFF_VNET_HDR` 注入 GSO 元数据（或真实流量经 vxlan loopback 由 GRO 生成）；
- `vxlan0`：`id 100`、基于 tun0、local 10.0.0.1 / remote 10.0.0.2；
- `br0`：成员 = vxlan0 + veth0。

## 3. 报文

外层：`Ethernet / IPv4 / UDP(4789) / VXLAN(VNI=100)`；内层：`Ethernet(广播, ethertype 0x0800) / payload`。

vnet 头（注入变体）：`gso_type = VIRTIO_NET_HDR_GSO_TCPV4`、`gso_size=4`、`hdr_len` 覆盖到内层 TCP。

---

## 4. 问题流程（逐步）

1. **注入**：userspace `write(fd, vnet_hdr + 帧)` → `tun_get_user` → `netif_receive_skb_list_internal`（tun 的 gro 组）。
2. **外层 GRO 写 transport_header**：`inet_gro_receive()` 处理外层 IPv4：
   ```c
   skb_gro_pull(skb, sizeof(*iph));                        // data → 外层 UDP
   skb_set_transport_header(skb, skb_gro_offset(skb));     // transport_header = 外层 UDP
   ```
   此后 `transport_header` 相对原始 data 为 +20。
3. **UDP encap 收包**：外层 UDP(2152/4789) 命中 VXLAN socket（`encap_type` 非 0，`udp_unexpected_gso()` 放行 GSO 包不被切分）。
4. **vxlan_rcv 解封装**（drivers/net/vxlan/vxlan_core.c）：
   - `__iptunnel_pull_header(skb, VXLAN_HLEN, ...)` 拉掉外层 UDP+VXLAN 共 16B；
   - `iptunnel_pull_offloads()`：清掉外层 tunnel GSO 位，`encapsulation = 0`，**保留内层 TCPV4 位与 gso_size**；
   - **全程不更新 transport_header** → 数据前移 16，`skb_transport_offset() = 20 - 36 = -16`。
5. **交付 vxlan 设备**：`gro_cells_receive(&vxlan->gro_cells, skb)` → vxlan 的 gro cell NAPI（`gro_cell_poll`）→ GRO flush → `netif_receive_skb_list_internal` → `__netif_receive_skb_core`。
6. **bridge 转发**：`__netif_receive_skb_core` 命中 br0 对 vxlan0 的 rx_handler（`br_handle_frame`）→ `br_handle_frame_finish` → `br_forward` → `br_dev_queue_push_xmit` 克隆 skb 并 `__dev_queue_xmit`。
7. **Qdisc 崩溃**：`__dev_queue_xmit` → `qdisc_pkt_len_segs_init()`：四个条件全满足 → `hdr_len = 0xfffffff0` → `th = skb->data + 0xfffffff0` 野指针 → `__tcp_hdrlen(th)` 的 `__asan_load2` 触发 KASAN/oops。

---

## 5. 实测调用栈（unpatched 7.2.0-rc4 #271）

```
BUG: unable to handle page fault for address: ffffed1028d4760d
Oops: 0000 [#1] SMP KASAN NOPTI
RIP: 0010:__asan_load2+0x48/0xa0
 R12: 00000000fffffff0        ← skb_transport_offset() = -16 的无符号值
Call Trace:
 <IRQ>
  qdisc_pkt_len_segs_init+0x128/0x300
  __dev_queue_xmit+0x13a/0x1960
  br_dev_queue_push_xmit+0x77/0x140
  br_handle_frame_finish+0x2b6/0xae0
  br_handle_frame+0x289/0x410
  __netif_receive_skb_core.constprop.0+0x698/0x1440
  __netif_receive_skb_list_core+0x1f4/0x430
  netif_receive_skb_list_internal+0x395/0x520
  napi_complete_done+0x11b/0x400
  gro_cell_poll+0xcb/0xf0          ← vxlan0 gro cell NAPI
  __napi_poll+0x53/0x2c0
  net_rx_action+0x5ad/0x690
  do_softirq / __local_bh_enable_ip / tun_get_user / tun_chr_write_iter / vfs_write
```

要点：
- 崩溃发生在软中断（bridge 转发克隆到 egress 的 `__dev_queue_xmit`）；
- `vxlan_rcv` 不在栈帧里：它已在 tun 的 BH 上下文同步完成（`udp_tunnel` 收包），可见帧是它的 gro cell NAPI 异步接续执行；
- `R12 = 0xfffffff0` 是负偏移的硬证据（-16 = 外层 UDP 8 + VXLAN 8）。

真实流量 loopback 变体（`vxlan_loopback_repro/serial_vxlan_loopback_rc7_panic.log`）栈结构相同，仅生产端不同：

```
qdisc_pkt_len_segs_init → __dev_queue_xmit → br_dev_queue_push_xmit
 → br_handle_frame_finish → br_handle_frame → __netif_receive_skb_core
 → __netif_receive_skb_one_core → process_backlog ...   （netif_rx backlog）
  ? ip_rcv / ip_local_deliver / vxlan_rcv               （内联提示帧）
 R12: 00000000fffffff0
```

即：真实 TCP 流经 vxlan 发送时 `iptunnel_handle_offloads()` 写入 `SKB_GSO_UDP_TUNNEL|TCPv4`，接收端 GRO/解封装后保留 TCPv4+gso_size 并置负偏移，再由 loopback/backlog 交付 bridge 转发触发。**该变体完全无需构造报文，验证了"普通流量+管理员配置即可触发"的攻击面。**

---

## 6. 为什么"bridge 转发"是必要条件

- 若内层帧走本地 IP 交付：`ip_rcv` / `inet_gro_receive` / TCP/UDP 处理会把 `transport_header` 重设到内层 L4（有效偏移），负偏移被"救回"。
- bridge 转发在 `__netif_receive_skb_core` 的 rx_handler 阶段直接克隆 skb 并 `br_dev_queue_push_xmit` 推入 egress 的 `__dev_queue_xmit`，**跳过了 IP 层重设环节** → 负偏移原样抵达 qdisc。

GTP 变体则是"反例的镜像"：走 gtp0 的 **ingress qdisc**（`sch_handle_ingress` 在协议分派前执行）→ 同样在 IP 层重设前被消费。两者都说明：**只要在 transport_header 被内层协"救回"前有 qdisc 消费点，缺陷就必现。**

---

## 7. 修复对照

| 补丁 | 做法 | 覆盖 |
|---|---|---|
| v1 `iptunnel_rebuild_transport_header`（GRE/TEB） | 解封装后按 GSO 重探/清空 transport header | IPv4/IPv6 GRE/TEB（`ip_tunnel_rcv`/`__ip6_tnl_rcv`） |
| v2/v3（Eric Dumazet 系列） | 各隧道收包点交付前 `skb_unset_transport_header()`：vxlan_rcv / geneve_rx / gtp_rx / l2tp / macsec | 逐隧道收口 |
| 兜底 | `qdisc_pkt_len_segs_init` 保留 `pskb_may_pull` + 边界检查 | 任何未来遗漏的解封装点 |

---

## 8. 相关文件

- 注入变体：`vxlan_repro/qdisc_uaf_vxlan_repro.c`（TAP 注入 GSO 帧）、`test_kernel_vxlan_unpatched_serial.log`（panic）
- 真实流量变体：`vxlan_loopback_repro/repro_vxlan_loopback.sh`、`serial_vxlan_loopback_rc7_panic.log`（panic）
- 同源变体：`gtp_repro/`（ingress qdisc）、`qdisc_uaf_repro_hsr.c` / `qdisc_uaf_repro_bridge.c`（原 syzbot 栈）
- 回迁复现：`olk510_repro/`（OLK-5.10 + 引入补丁 7fb4c1967011 等价改动后必现）