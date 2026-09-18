# sockmap 内存记账：CPU0 / CPU1 并发调用栈

日期：2026-09-15。本文是源码推导，没有运行复现或并发测试。

## 1. 版本与记号

- 原始基线：`45c13f3f9e3bb15fd89ff2864c6f627a3b4b4229`。
- 本轮开始时的旧方案：`61895c2fb113`（psock 引用）和
  `25c619569844`（TCP psock 记账锁）。本轮先归档模型，再由 `61c044e00cf8`
  revert 并拆分实现；完成状态见 [补丁说明](patches-v2/README.md)。
- 社区对照：Junseo Lim 的
  [v3 1/2](https://lists.openwall.net/linux-kernel/2026/08/17/1597)。
  第 4 节专门说明仅在 owner assignment 周围加锁的缺口。
- `S`：目标 TCP socket；跨 socket redirect 的源 socket 记为 `A`。
- `F`：`S->sk_forward_alloc`；`T`：某个 skb 的 `truesize`。
- `P`：`PAGE_SIZE`。数值交错示例省略 reserved memory 等无关条件。
- 调用栈省略系统调用包装和不改变锁/记账关系的中间层；缩进表示调用。
  两列从上往下给出一种允许的交错，不是实际采集的 trace。

`lock_sock(S)` 设置 logical ownership，返回时不持续持有
`S->sk_lock.slock`。因此另一 CPU 可以取得 `bh_lock_sock(S)`；是否允许
执行 TCP 操作，还必须检查 `sock_owned_by_user(S)`。

`work_mutex` 仅保护同一 psock 的工作处理；`ingress_lock` 保护消息队列和
stop 状态；两者均不是原生 TCP 记账操作共同使用的锁。

### 1.1 必须区分的三种“是不是同一个 sk”

1. **报文来源与 redirect 目标**：来源记为 `R`，目标记为 `S`；self 为
   `R == S`，cross-socket 为 `R == A != S`。
2. **并发记账操作的参数**：下面 F 的竞争模型中，两侧最终操作的记账参数
   都是 `S`。cross-socket 不表示 CPU0 改 A 的 F、CPU1 改 S 的 F。
3. **`skb->sk` 和 psock 身份**：它们不等于报文的逻辑来源。
   parser clone 入 work 前可有 `skb->sk == NULL`；无 parser 的 skb 可保留
   源 R 和 `sock_efree`，到目标 ingress 才变成 `S + sock_rfree`。
   psock P0/P1 不同，也可能有 `P0->sk == P1->sk == S`。

| 模型 | 生产者/来源 socket | work/资源所属 socket | CPU1 操作的 socket | 最终比较 |
|---|---|---|---|---|
| A | R=S，self ingress redirect | S | TCP RX 的 S | 两边记账 sk 相同 |
| B | R=A，A!=S，cross ingress redirect | S | recv(S) | 来源不同，两边记账 sk 相同 |
| C | 主例为 R=S 的 strparser SK_PASS 回退 | S | recv(S) 后的新操作仍是 S | 两边记账 sk 相同；同一个 skb 的两份引用 |
| D | 主例为此前 R=S 的 ingress | old_psock->sk=S | 活着的 S 或新 psock 的 S | 旧/新 psock 可不同，记账 sk 相同 |
| E | send(S) 产生 S 的 cork | S；不经过 psock backlog | TCP RX/recv 的 S | 两边记账 sk 相同 |
| F1 | R=A，A!=S；A 可运行 verdict | 未 accept 的 child S | listener L 销毁 child S，L!=S | 来源和 listener 都不同，最终记账 sk 同为 S |
| F2 | send(A)，A!=S；SK_MSG egress | 未 accept 的 child S；无 psock backlog | listener L 销毁 S，L!=S | 目标发送和目标销毁同为 S |
| F3 | 此前 A!=S 的 cross ingress | old_psock->sk=S | listener L 销毁 S，L!=S | destroy work 和 TCP 销毁同为 S |
| G | 主例 R=S，排入 P0 的 work | P0->sk=S | 删除/重新插入同一个 S，生成 P1 | P0!=P1，但 P0->sk==P1->sk==S |
| H | 假设模型，无固定报文来源 | 两次退还均属于 S | 同一个 S | 若分别为不同 sk，则不是本文这个 F 竞争 |
| I | 主例 R=S，self ingress 排 work | S | close(S)，本节画在 CPU0 | close 与 worker 等待同一个 S 的锁 |

### 1.2 sk_psock_backlog 的实际调用者与调度来源

**入队/调度与执行是两个调用栈。** 调度者可以运行在任意 CPU；
`schedule_delayed_work()` 不在原调用栈中直接执行 `sk_psock_backlog()`。
以下各模型先给出生产/调度来源，再给 CPU0 / CPU1 的并发栈，不能把两者
拼成一条同步调用栈，也不能假定原来的源 socket 锁会传递给 kworker。

注册与执行：

```text
sk_psock_init(S)
  psock->sk = S
  INIT_DELAYED_WORK(&psock->work, sk_psock_backlog)

[schedule_delayed_work(...); asynchronous handoff]

kworker: worker_thread()
  process_scheduled_works()
    process_one_work()
      worker->current_func(work)
        sk_psock_backlog(&psock->work.work)
          container_of(...) -> psock
          psock->sk == S
```

**来源 N：TCP 接收，无 stream parser。** `R` 是收到网络报文并运行 verdict 的
socket；这里需要 `R->sk_socket` 提供 `ops->read_skb`。

```text
tcp_v4_rcv(R)                         [IPv6 对应 tcp_v6_rcv]
  bh_lock_sock(R); !sock_owned_by_user(R)
  tcp_v4_do_rcv(R)
    tcp_rcv_established(R)
      tcp_data_queue(R) / 接收快路径
      tcp_data_ready(R)
        R->sk_data_ready(R) = sk_psock_verdict_data_ready(R)
          ops->read_skb(R, sk_psock_verdict_recv) = tcp_read_skb(R, ...)
            skb_set_owner_sk_safe(skb, R)
            sk_psock_verdict_recv(R, skb)
              bpf_prog_run_pin_on_cpu(verdict, skb)
                [redirect 时: bpf_sk_redirect_map/hash(..., target=S)]
              sk_psock_map_verd(...)
              sk_psock_verdict_apply(psock_R, skb, verdict)
```

**来源 P：TCP 接收，有 stream parser。** 到 `tcp_data_ready(R)` 的接收栈同 N，
后续回调如下；完整 parser 消息产生后才调用 verdict。

```text
tcp_data_ready(R)
  R->sk_data_ready(R) = sk_psock_strp_data_ready(R)
    strp_data_ready(&psock_R->strp)
      strp_read_sock(strp_R)
        strp->cb.read_sock = tcp_bpf_strp_read_sock(strp_R, ...)
          tcp_read_sock_noack(R, ..., strp_recv, ...)
            __tcp_read_sock(R, ...)
              strp_recv(...)
                __strp_recv(...)
                  strp->cb.rcv_msg = sk_psock_strp_read(strp_R, skb)
                    bpf_prog_run_pin_on_cpu(verdict, skb)
                    skb_bpf_set_strparser(skb)
                    skb->sk = NULL
                    sk_psock_verdict_apply(psock_R, skb, verdict)
```

如果 `strp_data_ready()` 发现 R 被 owner 持有，或读取返回 `-ENOMEM`，
会先 `queue_work(strp_wq, &strp_R->work)`；对应 parser work 执行
`strp_work() -> do_strp_work() -> strp->cb.lock()`，取得 R 的 socket 锁后
进入相同的 `strp_read_sock()` 栈。**parser work 和目标 S 的 psock work
是不同的工作项。** `strp_read_sock()` 同样检查 `R->sk_socket`。

N/P 最终有两种入 psock backlog 的分支：

```text
REDIRECT，R==S 或 R!=S 均可：
sk_psock_verdict_apply(psock_R, skb, __SK_REDIRECT)
  tcp_eat_skb(R, skb)
  sk_psock_skb_redirect(psock_R, skb)
    sk_other = S; psock_other = psock_S
    spin_lock_bh(&psock_S->ingress_lock)
    skb_queue_tail(&psock_S->ingress_skb, skb)
    schedule_delayed_work(&psock_S->work, 0)
    spin_unlock_bh(...)

SK_PASS，R==S：
sk_psock_verdict_apply(psock_S, skb, __SK_PASS)
  skb_bpf_set_ingress(skb)
  [队列空时尝试 sk_psock_skb_ingress_self(..., take_ref=false)]
  [已有 backlog，或直接交付失败]
  spin_lock_bh(&psock_S->ingress_lock)
  skb_queue_tail(&psock_S->ingress_skb, skb)
  schedule_delayed_work(&psock_S->work, 0)
  spin_unlock_bh(...)
```

另有两种重新调度来源，它们不改变队列中报文原来的 R/S 关系：

```text
EAGAIN retry：
sk_psock_backlog(psock_S->work)
  sk_psock_handle_skb(..., psock_S, ...) -> -EAGAIN
  sk_psock_skb_state(...)
  restore skb redirect metadata
  schedule_delayed_work(&psock_S->work, 1)
  return
  [下一次 workqueue 异步执行 sk_psock_backlog]

目标 S 的 write-space 回调，例如 TCP ACK 腾出发送空间：
tcp_rcv_established(S)
  tcp_data_snd_check(S)
    tcp_check_space(S)
      __tcp_check_space(S)
        tcp_new_space(S)
          S->sk_write_space(S) = sk_psock_write_space(S)
            psock = sk_psock(S)
            schedule_delayed_work(&psock->work, 0)
  [之后 workqueue 异步执行；队列可能已经为空]
```

源码入口：[skmsg.c](../net/core/skmsg.c)、[tcp.c](../net/ipv4/tcp.c)、
[tcp_bpf.c](../net/ipv4/tcp_bpf.c)、[strparser.c](../net/strparser/strparser.c)。

## 2. 模型 A：self ingress worker 与同一 socket 的 TCP 收包

适用：原始基线。verdict 将报文 ingress redirect 回 S，处理进入 psock work。
有 parser 时 clone 可以无 owner；无 parser 时还要完成接收 owner 转换。
两者的具体 helper 分支可能不同，但最终修改同一个 S 的 F。

**socket 关系：R=S；CPU0 的 `psock->sk` 与 CPU1 的 TCP 接收 sk 都是 S。**
本例选择来源 N，排 work 的前置调用栈如下；来源 P 的 self redirect 也可到达。

```text
tcp_v4_rcv(S) -> tcp_v4_do_rcv(S) -> tcp_rcv_established(S)
  -> tcp_data_ready(S) -> sk_psock_verdict_data_ready(S)
  -> tcp_read_skb(S, sk_psock_verdict_recv)
  -> sk_psock_verdict_recv(S, skb)
  -> verdict: bpf_sk_redirect_map(..., S, BPF_F_INGRESS)
  -> sk_psock_verdict_apply(psock_S, skb, __SK_REDIRECT)
  -> sk_psock_skb_redirect(psock_S, skb)
  -> skb_queue_tail(&psock_S->ingress_skb, skb)
  -> schedule_delayed_work(&psock_S->work, 0)
  [异步边界；以下 CPU0 是消费该 work 的 kworker]
```

```text
CPU0: psock backlog work                 CPU1: TCP RX for S
-------------------------------------   -------------------------------------
sk_psock_backlog()
  mutex_lock(&psock->work_mutex)
  [no lock_sock(S)]
  sk_psock_handle_skb(..., ingress=1)
    sk_psock_skb_ingress()
      create_ingress_msg() or self()
                                        tcp_v4_rcv()
                                          bh_lock_sock(S)
                                          !sock_owned_by_user(S)
                                          tcp_v4_do_rcv()
                                            tcp_rcv_established()
                                              tcp_data_queue()/tcp_queue_rcv()
      skb_set_owner_r(skb, S)                    skb_set_owner_r(rx_skb, S)
        sk_mem_charge(S, T0)                      sk_mem_charge(S, T1)
          read F = X
                                                  read F = X
                                                  write F = X - T1
          write F = X - T0
```

正确结果应包含两次扣账，实际丢失 CPU1 的更新。额度调度
`sk_rmem_schedule() -> __sk_mem_schedule()` 也会写 F，因此不仅 owner
assignment 需要保护。

修复边界：在异步 TCP ingress 外层持 socket 锁，覆盖 self 和非 self 分支。
正常 RX 在看到 owner 后进入原生 TCP backlog，不能继续上述记账。

## 3. 模型 B：cross-socket ingress 与目标 socket 的 recv

适用：原始基线，以及仅给 self helper 加锁的方案。
条件：A 的报文 redirect 到 S，worker 处理时 `skb->sk != S`。

**socket 关系：A!=S；CPU0/CPU1 的记账参数仍同为 S。**
这里选择 A/S 都已 accept、A 无 parser 的来源 N；进入 work 时可有
`skb->sk == A`，不能因此把目标 F 的写入误认为对 A 的记账。

```text
tcp_v4_rcv(A) -> tcp_v4_do_rcv(A) -> tcp_rcv_established(A)
  -> tcp_data_ready(A) -> sk_psock_verdict_data_ready(A)
  -> tcp_read_skb(A, sk_psock_verdict_recv)
  -> sk_psock_verdict_recv(A, skb)
  -> verdict: bpf_sk_redirect_map(..., S, BPF_F_INGRESS)
  -> sk_psock_verdict_apply(psock_A, skb, __SK_REDIRECT)
  -> sk_psock_skb_redirect(psock_A, skb)
       sk_other=S; psock_other=psock_S
  -> skb_queue_tail(&psock_S->ingress_skb, skb)
  -> schedule_delayed_work(&psock_S->work, 0)
  [异步边界；源 A 的接收锁不保护目标 S 的记账]
```

```text
CPU0: backlog of target psock S          CPU1: application recv(S)
-------------------------------------   -------------------------------------
sk_psock_backlog()
  mutex_lock(&psock->work_mutex)
  sk_psock_handle_skb(..., ingress=1)
    sk_psock_skb_ingress()
      skb->sk != S
      sk_psock_create_ingress_msg()
        sk_rmem_schedule(S, skb, T)
                                        tcp_bpf_recvmsg()
                                          lock_sock(S)
                                          sk_msg_recvmsg()
                                            __sk_msg_recvmsg()
                                              kfree_sk_msg()
                                                consume_skb(old_skb)
                                                  sock_rfree(old_skb)
      skb_set_owner_r(skb, S)                        sk_mem_uncharge(S, U)
        sk_mem_charge(S, T)
          read F = X
                                                      read F = X
                                                      write F = X + U
          write F = X - T
```

这里选择被消费的 old_skb 只有消息持有的最后一个引用；它可以是此前 worker
已完成交付的另一报文。不要求 CPU1 消费 CPU0 正在构造的 skb。

错误不是源 A 的锁未持有，而是目标 S 的两条记账路径没有互斥。
`sk_psock_skb_ingress_self_backlog()` 的锁不覆盖 `skb->sk != S` 分支。

## 4. 模型 C：最后一个 skb 引用落在 worker 中

适用：原始基线，以及社区 v3 的 owner-only 锁范围。
条件：enqueue 使用 `take_ref=true`，消息发布前执行 `skb_get()`。

**socket 关系：主例 R=S，CPU0/CPU1 都使用 S；两个引用指向同一个 skb。**
为了具体展示 v3 self helper 的缺口，选择来源 P 的 SK_PASS 回退：

```text
tcp_data_ready(S) -> sk_psock_strp_data_ready(S) -> strp_data_ready(strp_S)
  -> strp_read_sock() -> tcp_bpf_strp_read_sock() -> tcp_read_sock_noack(S)
  -> __tcp_read_sock() -> strp_recv() -> __strp_recv()
  -> sk_psock_strp_read(strp_S, skb)
  -> verdict: SK_PASS
  -> sk_psock_verdict_apply(psock_S, skb, __SK_PASS)
       [直接交付失败，或 ingress_skb 已非空]
  -> skb_queue_tail(&psock_S->ingress_skb, skb)
  -> schedule_delayed_work(&psock_S->work, 0)
  [异步边界；后续在 worker 中 enqueue，take_ref=true]
```

原始基线的 cross ingress（来源 N/P，A!=S）也会取得第二份引用并允许相同
最终释放交错；那种情况下两侧记账 sk 仍同为 S。

```text
CPU0: psock worker                      CPU1: application / next socket op
-------------------------------------   -------------------------------------
sk_psock_backlog()
  handle_skb()
    [v3 self path: lock_sock(S)]
    skb_set_owner_r(skb, S)
    [v3 self path: release_sock(S)]
    sk_psock_skb_ingress_enqueue()
      skb_get(skb)           users: 1 -> 2
      sk_psock_queue_msg()
                                        tcp_bpf_recvmsg_parser()
                                          lock_sock(S)
                                          ... __sk_msg_recvmsg()
                                            kfree_sk_msg()
                                              consume_skb(skb)
                                                users: 2 -> 1
                                          release_sock(S)
                                        next recv/send on S
                                          lock_sock(S)
                                          ... charge/uncharge/reclaim F
  skb_dequeue(&psock->ingress_skb)
  kfree_skb(skb)              users: 1 -> 0
    skb_release_head_state()
      sock_rfree(skb)
        sk_mem_uncharge(S, T)
          update/reclaim F concurrently
```

`skb->users` 的原子减引用可以正确工作；它没有保证 destructor 的执行上下文。
因此不能把该模型描述为已经证明的 skb refcount 丢失更新或 UAF。

修复边界：TCP ingress worker 必须在 `kfree_skb()` 之后才释放 socket 锁。
失败、拒绝入队、重试后遗留 skb 的清理也必须检查其最后一次释放位置。

## 5. 模型 D：detach 后的旧 psock cleanup 与仍存活的 socket

适用：原始基线。map 删除不等于 socket close；S 的 fd 可以保持打开，
恢复原生 TCP 收发，也可以挂载新 psock。旧 psock 仍可能持有未读消息。

**socket 关系：old_psock->sk=S，CPU1 继续使用同一个 S；psock 可不同。**
未读消息的来源可选模型 A 的 self redirect 调度栈：

```text
来源 N 的 sk_psock_verdict_recv(S, skb)
  -> sk_psock_verdict_apply(psock_S, skb, __SK_REDIRECT)
  -> sk_psock_skb_redirect(psock_S, skb)
  -> schedule_delayed_work(&psock_S->work, 0)
  [异步] process_one_work() -> sk_psock_backlog()
    -> sk_psock_skb_ingress() -> sk_psock_skb_ingress_enqueue()
    -> sk_psock_queue_msg()                  [消息未被应用读取]

之后删除最后 map 关联：
sock_map_delete_elem() -> sock_map_unref() -> sk_psock_put()
  -> sk_psock_drop(old_psock)
       detach S->sk_user_data; stop old_psock
  -> queue_rcu_work(..., &old_psock->rwork)
  [RCU 宽限期及异步 workqueue 调度；以下 CPU0 执行 destroy work]
```

```text
CPU0: old psock destroy RCU work         CPU1: native TCP operation on S
-------------------------------------   -------------------------------------
sk_psock_destroy()
  sk_psock_done_strp()
  cancel_delayed_work_sync(&old->work)
  [old worker is now quiescent]
  [no lock_sock(S) in baseline]
  __sk_psock_zap_ingress(old)
    ingress_skb: sock_drop()/kfree_skb()
    ingress_msg: sk_msg_free()
      __sk_msg_free()
        consume_skb()/sk_mem_uncharge()
          sock_rfree() if last skb ref
            sk_mem_uncharge(S, T)
                                        tcp_sendmsg()/tcp_recvmsg()
                                          lock_sock(S)
                                          ... native TCP accounting
                                            charge/uncharge/reclaim F
```

RCU 等待的是旧读者；cancel 等待的是 old worker。它们都不能停止 CPU1
后续对同一个 S 的操作。旧/新 psock 各自的 mutex 也不能互斥共享的 F。

修复边界：先同步结束 parser/backlog，再持 S 的 socket 锁清理队列和剩余 cork，
完成后才释放 psock 持有的 socket 引用。

## 6. 模型 E：map 删除触发 cork 释放，与 TCP/SK_MSG 并发

适用：原始基线；需要存在 SK_MSG cork，纯 SK_SKB self redirect 不要求有 cork。

**socket 关系：cork 归属于 S，CPU1 的原生 TCP 操作也使用 S。此模型不通过
`sk_psock_backlog()` 生成 cork。** cork 的实际来源为：

```text
send(S) -> tcp_bpf_sendmsg(S)
  lock_sock(S)
  sk_msg_alloc(S, msg, ...) -> sk_mem_charge(S, ...)
  tcp_bpf_send_verdict(S, psock_S, msg, ...)
    sk_psock_msg_verdict(S, psock_S, msg)
      BPF SK_MSG program: bpf_msg_cork_bytes(...)
    [msg->cork_bytes > msg->sg.size]
    allocate psock_S->cork; copy msg into cork
  release_sock(S); sk_psock_put(S, psock_S)
  [发送者已退出；之后 CPU0 才删除最后 map 关联]
```

```text
CPU0: remove last map link              CPU1: socket operation on S
-------------------------------------   -------------------------------------
map_delete_elem()
  sock_map_delete_elem()/sock_hash_delete_elem()
    [map lock; RCU/BH context possible]
    sock_map_unref()
      sk_psock_put()
        sk_psock_drop()
          sk_psock_stop()
            spin_lock_bh(ingress_lock)
            clear TX_ENABLED
            sk_psock_cork_free()
              sk_msg_free(S, cork)
                __sk_msg_free()
                  sk_mem_uncharge(S, N)
                  or consume_skb()
                    sock_rfree()
                                        lock_sock(S)
                                        TCP or SK_MSG accounting
                                          charge/uncharge/reclaim F
```

CPU1 可以选择原生 TCP 收包/读取；无需假设仍有一个持 old psock 引用的
SK_MSG 发送者，避免与“最后一个 psock 引用”前提矛盾。

修复边界：stop 只改变状态；不允许在 map/ingress 自旋锁内等待 socket 锁。
detach 的 cork 延迟到已同步且持 socket 锁的 destroy work。
正常 `sock_map_close()` 则在已有 socket 锁内立即清理 cork，随后解锁并等待 work。

## 7. 模型 F：未 accept 子连接的销毁越过 socket ownership

适用：原始基线、旧锁方案，以及仅修 psock 持锁的方案。
连接尚未 accept，child S 就能经 passive-established sockops 回调加入 sockmap。
`lock_sock(S)` 不要求已经存在应用 fd。

### F1. ingress 与 listener teardown

**socket 关系：A!=S，L!=S；CPU0 的目标 sk 与 CPU1 最终销毁的 child 都是 S。**
本例必须给出可达的生产者：已 accept 的 A 接收报文，verdict 将其投递到尚未
accept、但已加入 sockmap 的 S。不能把来源直接写成“未 accept 的 S 自己
触发接收 verdict”：前述 N/P 的读取入口都检查源 socket 的 `sk_socket`。

```text
S 的准备：
被动连接建立 -> tcp_init_transfer(S, BPF_SOCK_OPS_PASSIVE_ESTABLISHED_CB, ...)
  -> sockops BPF program -> bpf_sock_map_update(..., S)
  [S 在 L 的 accept queue 中；尚未 accept]

work 的生产者，来源 N，运行在 A 的接收上下文：
tcp_v4_rcv(A) -> tcp_v4_do_rcv(A) -> tcp_rcv_established(A)
  -> tcp_data_ready(A) -> sk_psock_verdict_data_ready(A)
  -> tcp_read_skb(A, sk_psock_verdict_recv) -> sk_psock_verdict_recv(A, skb)
  -> verdict: bpf_sk_redirect_map(..., S, BPF_F_INGRESS)
  -> sk_psock_verdict_apply(psock_A, skb, __SK_REDIRECT)
  -> sk_psock_skb_redirect(psock_A, skb)
       sk_other=S; psock_other=psock_S
  -> skb_queue_tail(&psock_S->ingress_skb, skb)
  -> schedule_delayed_work(&psock_S->work, 0)
  [异步] process_one_work() -> sk_psock_backlog(psock_S->work)
```

```text
CPU0: TCP ingress psock worker          CPU1: listener L close/disconnect
-------------------------------------   -------------------------------------
sk_psock_backlog()
  lock_sock(S)
    S->sk_lock.owned = 1
    return with slock released
                                        inet_csk_listen_stop(L)
                                          reqsk_queue_remove(...)
                                          child = req->sk = S
                                          bh_lock_sock(S)  [succeeds]
                                          WARN_ON(sock_owned_by_user(S))
                                          [continues despite owned == 1]
                                          inet_child_forget(L, req, S)
                                            tcp_disconnect(S, O_NONBLOCK)
  sk_psock_skb_ingress()                       purge receive/write/ofo queues
    sk_rmem_schedule()/skb_set_owner_r()         ... sock_rfree()/uncharge
      charge/schedule F                           update/reclaim F
                                            inet_csk_destroy_sock(S)
                                              sock_map_destroy(S)
                                                [old fix: cork_free()]
                                              sk_stream_kill_queues(S)
                                                sk_mem_reclaim_final(S)
```

listener 使用的是 parent L 的 socket 锁和 child S 的 BH 锁；CPU0 使用 child S
的 logical ownership。CPU1 没有遵守 owned 检查后的等待/延迟协议，两边可以写 F。
`sock_hold(S)` 只保证 struct sock 的生存期，不阻止协议层提前 disconnect/destroy。

### F2. SK_MSG egress 与 listener teardown

**socket 关系：A!=S，L!=S；目标发送与 child 销毁均操作同一个 S。**
这条路径没有 `sk_psock_backlog()`，不能用 SK_SKB worker 的来源栈代替：

```text
send(A) -> tcp_bpf_sendmsg(A)
  lock_sock(A)
  tcp_bpf_send_verdict(A, psock_A, msg, ...)
    sk_psock_msg_verdict(A, ...)
      SK_MSG program: bpf_msg_redirect_map/hash(..., S, flags=0)
    [__SK_REDIRECT; sk_redir=S; redir_ingress=false]
    release_sock(A)
    tcp_bpf_sendmsg_redir(S, false, msg, ...)
      tcp_bpf_push_locked(S, ...)           [以下 CPU0 所示]
    [返回后重新 lock_sock(A)，归还源 A 的额度]
```

源 A 的后续 uncharge 与目标 S 的 uncharge 是不同账本；这里竞争的是目标 S
发送期间的记账与 CPU1 对 S 的销毁，不是 A/S 两个计数本身相互竞争。

```text
CPU0: SK_MSG redirect A -> S            CPU1: listener L teardown
-------------------------------------   -------------------------------------
tcp_bpf_sendmsg_redir(S, msg, ...)
  tcp_bpf_push_locked(S, ...)
    lock_sock(S)
    tcp_bpf_push(S, ...)
      tcp_sendmsg_locked(S, ...)
        allocate/charge TCP send memory
                                        inet_csk_listen_stop(L)
                                          bh_lock_sock(S)
                                          WARN_ON(owned) and continue
                                          inet_child_forget()
                                            tcp_disconnect(S, ...)
                                              tcp_write_queue_purge(S)
                                                uncharge/reclaim F
```

该 SK_MSG 路径直接调用 `tcp_sendmsg_locked()`，不能用 SK_SKB
`skb_send_sock()` 对 `sk_socket == NULL` 的拒绝证明它不可达。
因此这个 TCP teardown 缺口不只影响 ingress。

### F3. 为什么只改 sock_map_destroy 不够

**socket 关系：old_psock->sk=S，L!=S；两边清理的记账 socket 仍是 S。**
资源的生产链沿用 F1：A 的 verdict 调度 S 的 `sk_psock_backlog()`，worker
将 skb 放入 S 的消息队列；S 尚未 accept，消息保留。随后删除 S 的最后 map
关联，`sk_psock_put() -> sk_psock_drop() -> queue_rcu_work()` 调度旧 psock 的
destroy work。CPU0 的当前入口因此是 destroy work，并非 backlog work：

```text
CPU0: old psock destroy work            CPU1: listener L teardown
-------------------------------------   -------------------------------------
process_one_work()
  sk_psock_destroy(old_psock->rwork)
    cancel_delayed_work_sync(old->work)
    lock_sock(S)
      owned(S)=1; slock(S) released
                                        inet_csk_listen_stop(L)
                                          bh_lock_sock(S)
                                          WARN_ON(owned(S)) and continue
                                          inet_child_forget(L, req, S)
                                            tcp_disconnect(S, ...)
    __sk_psock_zap_ingress(old_psock)           purge TCP queues of S
      ... sk_mem_uncharge(S, ...)                ... uncharge/reclaim F(S)
```

即使把 `sock_map_destroy()` 自己的 cork 释放延迟，
`tcp_disconnect()` 和 `sk_stream_kill_queues()` 的竞争仍然存在。

完整修复需要在 child migration/disconnect 之前处理 owned 情况。
直接给 `inet_child_forget()` 加睡眠锁不成立：其调用者还包括持 accept-queue
自旋锁、BH/RCU 上下文的路径。直接在 `tcp_release_cb()` 销毁也未证明安全，
因为 `__sk_flush_backlog()` 会在当前 socket 操作中途调用它。

该 TCP 修复尚未实现：延迟载体、引用交接、分配失败、TFO 和 reuseport 迁移，
以及 listener 重新 listen 的队列代际问题，需要单独完成设计。
本轮 sockmap 局部修复不能声称消除了 F1/F2/F3。

## 8. 模型 G：旧 work 获取新 psock 引用，却 put 旧对象

适用：原始基线。与 F 的互斥独立；旧引用补丁本身的修复方向仍然保留。

**socket 关系：P0!=P1，但 `P0->sk == P1->sk == S`；CPU1 删除再插入的是
同一个 S，不是另一个 socket。** work 的来源选择模型 A 的 self redirect：

```text
来源 N: sk_psock_verdict_recv(S, skb)
  -> sk_psock_verdict_apply(P0, skb, __SK_REDIRECT)
  -> sk_psock_skb_redirect(P0, skb)
       sk_other=S; psock_other=P0
  -> skb_queue_tail(&P0->ingress_skb, skb)
  -> schedule_delayed_work(&P0->work, 0)
  [异步] process_one_work() -> sk_psock_backlog(P0->work)
  [随后才发生下方 TX 检查与 map 删除/插入的交错]
```

```text
CPU0: work belonging to psock P0         CPU1: delete and reinsert S
-------------------------------------   -------------------------------------
sk_psock_backlog(P0->work)
  test P0.TX_ENABLED == 1
                                        delete last map link
                                          P0.refcnt: 1 -> 0
                                          sk_psock_drop(P0)
                                            S.sk_user_data = NULL
                                            stop P0
                                            queue_rcu_work(P0.destroy)
                                        insert S into map again
                                          allocate P1
                                          S.sk_user_data = P1
  sk_psock_get(P0->sk)
    lookup S.sk_user_data -> P1
    increment P1.refcnt
  discard returned pointer
  continue using P0
  sk_psock_put(S, P0)
    decrement zero refcount of P0
```

确定后果是 get/put 对象错配、新引用泄漏和旧 refcount 下溢条件。
旧 destroy 的同步 cancel 仍保护 work 执行期的对象内存，不能仅凭此证明 UAF。
修复是直接 `refcount_inc_not_zero(&P0->refcnt)`，不是新增 TX 状态检查。

## 9. 模型 H：原子加减不能保护整个 reclaim 事务

这是反驳“只把 F 改 atomic”的假设模型，并非当前源码已经使用 atomic。
初始 F=0，各 CPU 归还一页，且没有 reserved memory。

**socket 关系：CPU0/CPU1 的 uncharge 参数必须都是 S。** 可以把调用来源
具体绑定为模型 D：CPU0 由 map detach 调度 `sk_psock_destroy()`，释放此前
S 的 backlog 交付的消息；CPU1 在 `tcp_recvmsg(S)` 下释放另一 skb。
两边最终分别经过 `sock_rfree(skb0/1) -> sk_mem_uncharge(S, P)`。
以下只替换其 F 操作为假设的 atomic，不代表代码已经这样实现。

```text
CPU0: uncharge P                        CPU1: uncharge P
-------------------------------------   -------------------------------------
atomic_add(P, F) -> F=P
sk_mem_reclaim()
  snapshot reclaimable=P
                                        atomic_add(P, F) -> F=2P
                                        sk_mem_reclaim()
                                          snapshot reclaimable=2P
                                          __sk_mem_reclaim(S, 2P)
                                            atomic_sub(2P, F) -> F=0
                                            memcg uncharge 2 pages
  __sk_mem_reclaim(S, P)
    atomic_sub(P, F) -> F=-P
    memcg uncharge 1 more page
```

共归还两页可回收额度，却向 memcg 归还三页。需要序列化完整事务，或者重新设计
原子的额度领取/回滚协议，不能只替换 F 的类型。

## 10. 模型 I：恢复 socket 锁时必须避免的旧死锁

历史关联：`799aa7f98d53`。以下是必须避免的锁/等待环，不是当前新方案的时序。

**socket 关系：CPU0 close 的 S 与 CPU1 worker 的 `psock->sk` 完全相同。**
这里选择“给 ingress 恢复 socket 锁，却仍在 close 持锁 cancel”的错误组合，
work 的来源为模型 A 的 self ingress：

```text
来源 N: sk_psock_verdict_recv(S, skb)
  -> sk_psock_verdict_apply(psock_S, skb, __SK_REDIRECT)
  -> sk_psock_skb_redirect(psock_S, skb)
  -> schedule_delayed_work(&psock_S->work, 0)
  [异步] process_one_work() -> sk_psock_backlog()
```

历史 egress 也可形成相同等待环：redirect helper 的 flags 为 0，仍排入目标
S 的 `ingress_skb` 工作队列；worker 通过发送路径内部获取 S 的 socket 锁。
队列名 `ingress_skb` 本身不能用来判定最终是 ingress 还是 egress。

```text
CPU0: sock_map_close(S)                  CPU1: psock backlog work
-------------------------------------   -------------------------------------
lock_sock(S)
cancel_delayed_work_sync(&psock->work)
  wait for CPU1 to finish                lock_sock(S)
                                          wait for CPU0 to release S
```

新方案保留 `8bbabb3fddcd` 的次序：close 先 `release_sock()` 再 cancel；
destroy work 先 cancel 再 lock；egress 不在 `skb_send_sock()` 外重复加锁。
这三项需要作为回移补丁的前提，而不是仅在提交说明中声称“不会死锁”。

## 11. 本轮修复边界与静态验收

- A/B/C：TCP ingress 外层锁一直持有到 worker 释放 skb。
- D/E：取消工作后持锁释放旧资源；普通 close 保留即时 cork 释放。
- G：独立固定 work 所属 psock 的引用。
- H：不使用 atomic F 替代完整串行化。
- I：检查所有退出和 cancel 的锁顺序，保留 work_mutex。
- F1/F2/F3：独立 TCP ownership 缺口仍待修复，不能宣称完整闭环。
- strparser 的已有 owner 保留、已有负额度补足，以及 `_sk_redir` 重试恢复，
  合并为自投递与重试补丁；TCP worker 与清理的锁保护也合并成一个补丁。
  strparser 补丁不能替代 TCP 并发修复，psock 引用修复仍独立。
- `copied_seq` 及其语义修复不在本轮代码范围内。

验收使用源码调用链、引用转移、锁顺序、补丁静态检查和两个修改对象的编译。
没有复现程序、QEMU、压力测试或运行时通过结论，详见补丁说明。
