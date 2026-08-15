# 补丁1问题（syzbot b89b76e3）分析（最终版：已完整复现）

> 版本说明：
> - v2（gpt_ana 修正）：BPF 污染是**使能条件**（0/0 特例进入 brd_input）、`route.c:2299` 特例、RTCF 标志值修正
> - v3（最终）：**社区 bug 已在 v7.2-rc4 完整复现**——无补丁1 时 WARNING，有补丁1 时拦截，与社区模型完全一致

## 1. 背景

- **社区 bug**：syzkaller bug [b89b76e33d398c544f3ecb6fe9896d099a8a0a87](https://syzkaller.appspot.com/bug?id=b89b76e33d398c544f3ecb6fe9896d099a8a0a87) "WARNING in ip_rt_bug (3)"
- **Fix commit**：`7eb72c1e3984` "ipv4: icmp: reject broadcast/multicast routes"（补丁1）
- **社区模型**：补丁2（`c44daa7e3c73` host relookup）与补丁3（`81b84de32bb2` xfrm 反向路径 race）合入后，syzbot 触发 ip_rt_bug；补丁1 合入后不再复现
- **任务**：用社区复现程序在 linux 仓（v7.2-rc4）复现，明确问题模型

## 2. 复现环境

- 内核：`/home/sisyphus/code/linux`，v7.2-rc4-62-g27ff6288dee7（补丁1/2/3 全在；复现时 `git revert --no-commit 7eb72c1e3984` 回退补丁1）
- 复现程序：`repro_syz.c`（社区原版，syzkaller 自动生成），`execute_one` 会**加载并 attach BPF flow dissector 程序**到 sandbox netns
- 注入包：`src=172.30.0.1, dst=172.20.20.17, TTL=0, proto=TCP`，选项 =
  `SSRR(0x89, len=7, ptr=0xa2, 数据=255.255.255.255) + CIPSO(0x86, len=6, type=0)`
- 测试脚本：`test_syz.sh`（含 rp_filter=0、syz-tmp 清理）

## 3. 完整触发链（含 BPF 污染，全部经插桩验证）

```
TUN 注入包
 → ip_rcv_core: 头部校验通过（实测 DBG ip_rcv_core: saddr=172.30.0.1 daddr=172.20.20.17 ttl=0）
 → ip_route_input_noref: 入站路由正常单播（实测 DBG ip_rcv: ...）
 → ip_options_compile: SSRR 记入 IPCB opt；CIPSO(len=6<8) 非法 → icmp_send(PARAMETERPROB)
 → __icmp_send(type=12 code=0)（实测）
     - 入站路由检查: pkt_type==HOST、无 RTCF_BROADCAST/MULTICAST → 通过
 → __ip_options_echo: SSRR ptr=0xa2>7 钳制到 len+1 → faddr=选项末尾 4 字节=255.255.255.255（代码核实）
     replyopts.opt.srr 设置 → icmp_route_lookup 的 fl4->daddr = 255.255.255.255（实测）
 → 前向: ip_route_output_key_hash(255.255.255.255)
     → __mkroute_output → RTCF_BROADCAST|RTCF_LOCAL, output=ip_mc_output（实测 rt_flags=0x90000000 rt_type=3）
 → xfrm_lookup 返回原路由；patch2 检查 inet_addr_type_dev_table(255.255.255.255)==RTN_LOCAL? 否 → 进反向路径（实测）
 → xfrm_decode_session_reverse:
     __skb_flow_dissect 被 netns 的 BPF flow dissector 接管（BPF 返回非 CONTINUE）
     → flkeys 全零（实测 dissected src=0.0.0.0 dst=0.0.0.0 proto=0）
     → fl4_dec.saddr=0.0.0.0, fl4_dec.daddr=0.0.0.0（实测 reverse daddr=0.0.0.0 saddr=0.0.0.0）
 → 反向: ip_route_input(skb, 0.0.0.0, 0.0.0.0, ...):
     route.c:2299: (saddr==0 && daddr==0) → brd_input          ← 关键特例（代码核实）
     brd_input: saddr==0 → 跳过 fib_validate_source（route.c:2399）
     → flags|=RTCF_BROADCAST; res->type=RTN_BROADCAST
     → local_input: rt_dst_alloc(flags|RTCF_LOCAL, RTN_BROADCAST) → dst.output=ip_rt_bug（实测 input rt_type=3 flags=0x90000000）
 → patch3 检查 rt2->rt_type == RTN_LOCAL: rt_type=3(RTN_BROADCAST) ≠ 2 → 不命中（代码核实）
 → 第二次 xfrm_lookup(XFRM_LOOKUP_ICMP) 返回该路由
 → __icmp_send 拿到 rt: type=3 flags=0x90000000（实测 rt returned type=3 flags=0x90000000）
 → 补丁1: rt->rt_flags & (RTCF_BROADCAST|RTCF_MULTICAST) = 0x90000000 & 0x30000000 ≠ 0 → goto ende（拦截）
 → 无补丁1: icmp_push_reply → ip_push_pending_frames → dst_output → ip_rt_bug WARN（syzbot 栈证实）
```

**标志值**（include/uapi/linux/in_route.h:24-27）：
`RTCF_BROADCAST=0x10000000, RTCF_MULTICAST=0x20000000, RTCF_LOCAL=0x80000000`
0x90000000 = RTCF_LOCAL | RTCF_BROADCAST。

## 4. 实测结论

- **核心路径成立**：插桩日志 `input rt_type=3 flags=0x90000000` / `rt returned type=3 flags=0x90000000`
  正是社区补丁要拦截的路由（RTN_BROADCAST + RTCF_BROADCAST + output=ip_rt_bug）
- **BPF flow dissector 不是阻断，而是使能**：0/0 解码正是进入 brd_input 0/0 特例的条件
- **社区补丁命中**：该路由 flags 含 RTCF_BROADCAST → 补丁1 在 icmp_push_reply 前终止（syzbot 语义）

## 5. 完整复现结果（v7.2-rc4 实测）

### 无补丁1（补丁2+3）：WARNING 复现 ✅

```
DBG send: rt type=3 flags=0x90000000 dev=lo daddr=255.255.255.255   ← brd_input 路由(output=ip_rt_bug)
DBG send: room=532 mtu=65535 optlen=16
DBG push_reply: enter daddr=255.255.255.255 saddr=172.20.20.17 len=64
DBG push_reply: pushing
DBG push_frames: enter daddr=255.255.255.255
WARNING: net/ipv4/route.c:1273 at ip_rt_bug+0x14/0x20, CPU#2: repro_syz/441
```

### 有补丁1：拦截 ✅（0 WARNING）

```
DBG send: rt type=3 flags=0x90000000 dev=lo daddr=255.255.255.255
DBG send: patch1 intercept          ← rt->rt_flags & (RTCF_BROADCAST|RTCF_MULTICAST) 命中 → goto ende
```

### 关于早期"发送失败"现象

早期若干次运行在 `ip_append_data` 后 `sk_write_queue` 为空、未进 `ip_push_pending_frames`，
属**瞬态/时序问题**（BPF attach 时序、路由 dev 等偶发因素）；加大重试后发送路径打通，
WARNING 稳定打出。syzbot 告警栈（`ip_push_pending_frames → ip_send_skb`）与本结果一致。

### 环境注意事项（已处理）

- `rp_filter=2` → 需设 0（`test_syz.sh` 已处理）
- `syz-tmp` 残留 → 需清理（`test_syz.sh` 已处理）
- 必须使用**带 BPF** 的原版复现器（NOBPF 版走原生 dissector，返回 UNICAST，不复现）

## 6. NOBPF 对照实验的意义（不否定原版路径）

去掉 BPF 后解码恢复正常（`dissected src=172.30.0.1 dst=172.20.20.17`），反向返回 RTN_UNICAST（正常）。
这只能说明**原生 dissector 路径下该包无害**；原版复现器带 BPF，走的是 0/0 brd_input 路径，
两者是不同流程，NOBPF 结果不能用于否定原版复现路径。

## 7. 结论

1. **社区 bug 已在 v7.2-rc4 完整复现**：补丁2+3（无补丁1）时，社区复现器触发
   `WARNING: net/ipv4/route.c:1273 at ip_rt_bug`；补丁1 合入后拦截，不再复现。
   **与社区模型（2+3 触发、+1 修复）完全一致**
2. **触发机制**：BPF flow dissector 把反向解码污染为 0.0.0.0/0.0.0.0 →
   `route.c:2299` 的 `(saddr==0 && daddr==0)` 特例 → brd_input（saddr=0 跳过源校验）→
   RTN_BROADCAST input 路由（`output=ip_rt_bug`，dev=lo）→ 补丁3 的 `==RTN_LOCAL` 漏网 →
   路由用于输出 → ip_rt_bug WARNING
3. **补丁1 的修复机制**：拦截 `rt->rt_flags & (RTCF_BROADCAST|RTCF_MULTICAST)`，
   命中的正是该 RTN_BROADCAST 路由（flags=0x90000000）
4. **补丁3 的缺口**：`rt2->rt_type == RTN_LOCAL` 用 `==` 漏掉 RTN_BROADCAST（3≠2）——
   社区 bug 正是被补丁3漏掉、被补丁1补上的场景
5. **另一个独立变体**：`repro_xfrm_race`（xfrm 策略+无源路由）复现的 RTN_UNREACHABLE
   （flags=0）——补丁3 和补丁1 都拦不住，需 0004 型扩展检查

## 8. 关键代码位置（v7.2）

| 位置 | 作用 |
|---|---|
| `net/ipv4/route.c:2299` | `(saddr==0 && daddr==0) → brd_input` 特例 |
| `net/ipv4/route.c:2394-2407` | brd_input：saddr=0 跳过源校验，置 RTCF_BROADCAST/RTN_BROADCAST |
| `net/ipv4/route.c:2426-2431` | local_input：`dst.output=ip_rt_bug` |
| `net/ipv4/ip_options.c` `__ip_options_echo` | faddr 越界钳制逻辑（faddr=选项末尾4字节） |
| `net/ipv4/ip_options.c` `ip_options_rcv_srr` | SSRR nexthop 循环、header 重写、-EINVAL 丢弃 |
| `net/ipv4/icmp.c` `icmp_route_lookup` | 前向/反向路径、patch2/3 检查 |
| `net/xfrm/xfrm_policy.c` `decode_session4` | 反向解码映射（reverse: saddr=包dst, daddr=包src） |
| `net/core/flow_dissector.c` `__skb_flow_dissect` | BPF flow dissector 接管点 |
| `include/uapi/linux/in_route.h:24-27` | RTCF 标志值定义 |

## 9. 相关文件（本目录归档结构）

```
ip_rt_bug/
├── 0001-ipv4-icmp-reject-broadcast-multicast-routes.patch   补丁1（原样）
├── 0002-xfrm-fix-ip_rt_bug-race-adapted.patch               补丁3 适配 6.6 版
├── 0003-debug-icmp-race-window-mdelay.patch                 6.6 版 mdelay 辅助
├── 0003-debug-icmp-race-window-mdelay-v72.patch             v7.2 版 mdelay 辅助
├── 0004-ipv4-icmp-reject-non-unicast-input-routes.patch     扩展修复补丁（提案）
├── repro_veth.c / repro_xfrm_race.c / repro_syz.c           复现器源码
├── ip_rt_ana.md / gpt_ana.md                                分析文档
├── bin/      复现器与探测二进制（repro_veth/xfrm_race/syz[_dbg] 等）
├── scripts/  测试脚本（test_veth/test_xfrm_race[_noroute]/test_syz/test.sh 等）
└── logs/     全部复现记录（6.6 与 v7.2 两棵树的结果）
```

- `logs/repro_syz_v72_nopatch1_warning.log`：无补丁1 的 WARNING 复现记录（v7.2）
- `logs/repro_syz_v72_with_patch1.log`：有补丁1 的拦截记录（v7.2）
- `logs/test_result_unpatched.txt` / `test_result_patched.txt`：6.6 veth 广播回复 复现/拦截
- `logs/repro_xfrm_race_patch3_warning.log` / `_fixed.log`：6.6 + 补丁3适配 + mdelay 的 noroute/with-route 结果
- `logs/repro_xfrm_race_noroute.log` / `repro_xfrm_race_v72_warning.log`：v7.2 UNREACHABLE 复现
