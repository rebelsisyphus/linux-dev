 确认：旧分析确有关键遗漏。原始 syz 复现器中的 BPF flow dissector 把反向 flow 解码成 0.0.0.0 -> 0.0.0.0，不是“污染导致阻断”，反而正是生成 RTN_BROADCAST +
  dst.output=ip_rt_bug 路由的关键条件。

  ### 修正后的完整流程

  TUN 注入：
    src=172.30.0.1
    dst=172.20.20.17
    SSRR={len=7, ptr=0xa2, addr=255.255.255.255}
    CIPSO len=6

  ip_options_compile()
    SSRR 已记录到 IPCB(skb)->opt
    CIPSO 长度非法，返回 -EINVAL
    -> icmp_send(ICMP_PARAMETERPROB)

  __icmp_send()
    入站路由是正常单播，第一次广播/组播检查通过

  __ip_options_echo()
    SSRR ptr 0xa2 > len 7
    -> 钳制到 len + 1
    -> 提取 255.255.255.255
    -> replyopts.opt.faddr = 255.255.255.255

  icmp_route_lookup()
    fl4->daddr = replyopts.opt.faddr
                = 255.255.255.255

    ip_route_output_key_hash()
      -> 前向广播路由
      -> RTCF_BROADCAST | RTCF_LOCAL
      -> dst.output = ip_mc_output

    xfrm_lookup()
      -> 返回原路由
    fl4->daddr 不是 RTN_LOCAL
      -> 进入 XFRM ICMP reverse relookup

    xfrm_decode_session_reverse()
      -> BPF flow dissector 接管
      -> fl4_dec.saddr = 0.0.0.0
      -> fl4_dec.daddr = 0.0.0.0

    ip_route_input(skb, 0, 0, ...)
      -> 命中 route.c 的特殊广播分支
      -> RTN_BROADCAST input route
      -> RTCF_BROADCAST | RTCF_LOCAL
      -> dst.output = ip_rt_bug

    RTN_LOCAL 补丁检查不命中
      因为 rt_type 是 RTN_BROADCAST，不是 RTN_LOCAL

    第二次 xfrm_lookup(XFRM_LOOKUP_ICMP)
      -> 返回该 RTN_BROADCAST input route

  __icmp_send()
    未打补丁：
      -> icmp_push_reply()
      -> ip_push_pending_frames()
      -> ip_send_skb()
      -> dst_output()
      -> ip_rt_bug() WARN

    打补丁：
      -> 检查 RTCF_BROADCAST
      -> goto ende
      -> 释放路由并停止发送

  ### 旧分析最关键的错误

  #### 1. “fl4_dec.daddr 永远等于原包源地址”不成立

  ip_rt_bug/ip_rt_ana.md第 7.1 节的结论只适用于内核原生 flow dissector。

  原始复现器已经附加 BPF flow dissector；文档自己的插桩也观察到：

  dissected src=0.0.0.0 dst=0.0.0.0 proto=0
  reverse daddr=0.0.0.0 saddr=0.0.0.0

  因此后面所有基于“反向目的地址必定是原始源地址 172.30.0.1”的推论，都不适用于原始社区复现路径。NOBPF 版本返回 RTN_UNICAST，不能用于否定包含 BPF 的原始复现
  器。

  #### 2. 0.0.0.0 -> 0.0.0.0 会被明确当成广播输入

  net/ipv4/route.c:2299中有专门分支：

  if (ipv4_is_lbcast(daddr) || (saddr == 0 && daddr == 0))
        goto brd_input;

  所以不需要“原包源地址是广播地址”。旧分析第 6、7 节认为进入 brd_input 必须让原包携带广播源地址，因此一定会被 martian-source 检查拒绝，这是遗漏了上述 0/0 特
  殊条件。

  更重要的是，这个判断位于 zeronet 源地址拒绝之前。进入 brd_input 后：

  if (!ipv4_is_zeronet(saddr))
        fib_validate_source_reason(...);

  当 saddr == 0 时反而跳过源地址校验，随后设置：

  flags |= RTCF_BROADCAST;
  res->type = RTN_BROADCAST;

  再经 net/ipv4/route.c:2426构造：

  rth->dst.output = ip_rt_bug;

  #### 3. 插桩结果 type=3 flags=0x90000000 已经证明核心路径成立

  旧文档记录了：

  input rt_type=3 flags=0x90000000
  __icmp_send: rt returned type=3 flags=0x90000000

  这实际上就是社区问题所需要的最终路由：

  - rt_type=3：RTN_BROADCAST
  - 0x10000000：RTCF_BROADCAST
  - 0x80000000：RTCF_LOCAL
  - 合计 0x90000000

  标志定义见 include/uapi/linux/in_route.h:24。

  这条路由已经满足两个关键条件：

  - dst.output == ip_rt_bug
  - 社区补丁的 RTCF_BROADCAST 检查能够命中

  所以 BPF 并没有阻断问题；它已经把流程推进到了正确的漏洞状态。

  #### 4. “write queue 为空”不足以推翻路径

  - ip_append_data() 失败会 flush queue；
  即使本地环境确实在 ip_append_data() 阶段失败，也只能说明该 VM 上还有额外环境差异，不能证明前面的社区路径不存在。社区告警栈已经证明，在 syzbot 环境中
  ip_append_data() 成功并进入了 ip_push_pending_frames()。

  社区补丁位于最终 net/ipv4/icmp.c:958之后：


  RTN_BROADCAST
  RTCF_BROADCAST | RTCF_LOCAL
  dst.output = ip_rt_bug

  因此补丁会在 icmp_push_reply() 前直接终止，问题自然不再复现。

  最终判断是：

  - “当前 VM 中原版程序没有打出 WARN”可以是事实。
  - “BPF flow dissector 阻断了社区路径”是错误结论。
  - “进入广播输入路由必须使用广播源地址”也是错误结论。
  - 旧分析自己的 type=3 flags=0x90000000 日志，实际上已经确认了社区补丁所修复的核心流程。
