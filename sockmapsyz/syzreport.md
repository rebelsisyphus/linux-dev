问题：WARNING in tcp_recvmsg_locked
syzkaller report来源：
Reported-by: syzbot+06dbd397158ec0ea4983@syzkaller.appspotmail.com
Closes: https://syzkaller.appspot.com/bug?extid=06dbd397158ec0ea4983
复现程序：
c程序: /home/sisyphus/code/test/sockmap_test/sockmap.c
二进制：/home/sisyphus/code/test/sockmap_test/sockmap

已完成的分析：
之前已有的修复, 但只修复了一个场景，仍存在问题：
https://lore.kernel.org/all/20260124113314.113584-1-jiayuan.chen@linux.dev/T/#m518a2f8ec88da2a4ddf05a3044d7641fe2a97c71
补丁集参考：fix-patch-1.md
问题模型：
1. 前提：TCP socket 先以原生方式接收数据，用户态部分读取了某个 skb（例如 200 字节只读 50 字节），剩下 150 字节仍留在 sk_receive_queue 中，此时 copied_seq 已推进到 seq + 50。
2. 添加 sockmap：socket 被加入 sockmap，sk_data_ready 被替换为 sk_psock_verdict_data_ready。
3. 新数据到达并聚合：后续又来一个有序报文（例如 1 字节），TCP 调用 tcp_try_coalesce 把它和队列中残留的旧 skb 合并。合并后 skb->len = 201，但 copied_seq 仍然只表示已读到的位置。
4. 触发 verdict：tcp_read_skb 把合并后的 skb 出队，交给 BPF verdict。若返回 SK_DROP 或 SK_REDIRECT（到其它 socket），会调用 tcp_eat_skb。
5. bug：tcp_eat_skb 无条件执行：
copied = tcp->copied_seq + skb->len;
把 skb->len（201）全部算作新消费，但其中 50 字节其实早被用户读过，导致 copied_seq 超过 rcv_nxt，触发 copied_seq > rcv_nxt 的告警或后续异常。
根因：tcp_eat_skb 没有按原生 TCP 的语义精确推进 copied_seq。原生 tcp_recvmsg 会根据 copied_seq - skb->seq 的 offset 跳过已读部分，只推进实际复制的字节数；而 tcp_eat_skb 直接加 skb->len，忽略了 skb 中可能包含已读数据。
修复：将 copied_seq 推进到 TCP_SKB_CB(skb)->end_seq，且只推进 end_seq - copied_seq 的差值，同时把 __tcp_cleanup_rbuf 的参数也改为该差值。

要求todo：
1.使用复现程序复现问题，确认上述的问题模型分析是否正确
2.修复问题，提交成符合linux社区规范的补丁，补丁commit msg严谨规范
3.根据复现程序，在selftest中添加用例，完成回归测试，selftest用例与修复补丁生成补丁集和cover letter

