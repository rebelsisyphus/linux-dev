# 普通 TCP backlog 与 psock work：socket 所有权和最终 skb 释放

源码基线：`45c13f3f9e3bb15fd89ff2864c6f627a3b4b4229`。
本篇展开前一轮答复第 4、5 点：socket 锁如何协调进程与收包，以及为何最后一份
skb 引用可能让 psock 的接收记账发生在无 socket 锁的 worker 中。

## 1. 先区分两种 backlog

| 项目 | 普通 TCP 的 `sk->sk_backlog` | `psock->ingress_skb` |
|---|---|---|
| 用途 | socket 被进程占用时，推迟 TCP 协议处理 | 接收 verdict 已决定 redirect/慢路径后，等待交付 |
| 处理入口 | `__release_sock()` → `sk_backlog_rcv()` → `tcp_v4_do_rcv()` | `sk_psock_backlog()` → ingress/egress |
| 常见执行者 | 当前持有 socket 所有权的进程，执行 release/flush 时处理 | 工作队列 worker |
| 处理期间的 socket 所有权 | 保持 `sk_lock.owned=1` | ingress work 没有取得该所有权 |
| 数据进入应用队列前的串行化 | 沿 TCP 的 socket 锁协议进行 | 当前代码的队列锁没有覆盖 socket 接收记账 |

**普通 TCP 的这些 backlog 路径没有前述“worker 绕过 socket 所有权并修改
`sk_forward_alloc`”的同类缺口。** 此结论限定为本文追踪的普通 TCP 原生接收、
backlog 消费和正常读取释放路径，不是对所有 TCP 扩展和所有代码的无缺陷声明。

## 2. socket 锁有两层配合

### 2.1 `slock`：短时间互斥

`sk->sk_lock.slock` 是自旋锁，`bh_lock_sock()` 获取它。它用于协调所有权切换、
串行化未被进程占用时的协议处理，以及保护 backlog 链表操作。

### 2.2 `owned`：跨较长操作的 socket 所有权

`lock_sock()` 取得的进程侧所有权由 `sk->sk_lock.owned` 表示；等待者使用
`sk_lock.wq`。获取过程中用 slock 或当前 64 位 combined 字段的原子快速路径
保证所有权交接安全。

逻辑上可以把慢路径理解成：

```text
lock_sock(S):
    获取 slock
    如果 owned == 1：放开 slock，睡眠等待，再尝试
    owned = 1
    放开 slock
    返回；调用者继续拥有 S
```

实际入口是 [lock_sock_nested](../net/core/sock.c#L3826)。
它返回之后进程可以复制用户数据、执行协议操作，并不一直持有 slock。

**“没有持 slock”不能单独证明没有串行化。还要看当前是否持有 socket 所有权，
以及其他访问者是否遵守 `owned` 检查。**

## 3. 持有 owned 时，新报文怎样处理

[tcp_v4_rcv](../net/ipv4/tcp_ipv4.c#L2244) 的核心选择为：

```c
bh_lock_sock_nested(sk);
if (!sock_owned_by_user(sk))
	tcp_v4_do_rcv(sk, skb);
else
	tcp_add_backlog(sk, skb);
bh_unlock_sock(sk);
```

### 3.1 `owned == 0`

当前收包者持 slock，直接处理 TCP。其他收包者要等待同一 slock；进程想
获得 socket 所有权也要等当前 slock 临界区结束。因此普通 TCP 记账不会同时被
两个这样的执行者修改。

### 3.2 `owned == 1`

收包者可以取得 slock，但只做 backlog 入队，不直接执行 TCP 正常数据接收处理。
此时原进程仍可修改协议状态和接收记账。

```text
进程 A                                  收包 CPU
lock_sock(S)
owned = 1；释放 slock
开始 recv/send，更新协议记账
                                        获取 slock
                                        看到 owned == 1
                                        tcp_add_backlog(S, B)
                                        释放 slock
继续 recv/send
```

另一进程 B 调用 `lock_sock(S)` 时则进入所有权等待，不能与 A 同时进入受保护的收发操作。

这里仍允许收包侧更新 backlog 链表、backlog 长度和特定收包统计；不是把整个收包函数
完全阻塞。关键限制是它不能绕过当前所有者，直接执行会修改同一套 TCP 收发记账的路径。

## 4. 入 backlog 为什么不会同时 charge `sk_forward_alloc`

普通入队路径为：

```text
tcp_add_backlog()
  → 必要的 skb 整理、校验、backlog 尾部合并
  → sk_add_backlog()
    → 检查 backlog / receive queue 占用
    → __sk_add_backlog()
    → sk_backlog.len += skb->truesize
```

[sk_add_backlog](../include/net/sock.h#L1160) 不调用 `skb_set_owner_r()` 或
`sk_mem_charge()`；这批普通入站 skb 还没有记入目标 S 的接收内存。
入队占用先由 `sk_backlog.len` 跟踪，该字段在 slock 下更新。

合并分支也需要区别：

- [tcp_add_backlog](../net/ipv4/tcp_ipv4.c#L1901) 调用通用 `skb_try_coalesce()`，
  并把 delta 加到 `sk_backlog.len`；它不在这里 charge S 的 forward allocation。
- 已进入 TCP receive/ofo 队列后的 [tcp_try_coalesce](../net/ipv4/tcp_input.c#L5244)
  才会调整 `sk_rmem_alloc` 和 `sk_mem_charge()`。此操作处于 TCP 的串行处理上下文。

backlog 阶段可以整理/释放某个 skb 的页片段，不等于已经对 S 调用接收 charge/uncharge。
应沿调用链确认它的 socket owner 和 destructor，不能只看到 `kfree_skb_partial()`
就认为它在修改 S 的 `sk_forward_alloc`。

## 5. `__release_sock()` 放开 slock 后为什么仍安全

### 5.1 `release_sock()` 的顺序

[release_sock](../net/core/sock.c#L3854) 的处理顺序为：

```text
获取 slock
如果 backlog 非空：__release_sock(S)
执行 TCP release_cb（如有待处理的延迟操作）
sock_release_ownership(S)             到这里才令 owned = 0
唤醒所有权等待者
释放 slock
```

所以进入和执行 `__release_sock()` 时，当前进程仍拥有 S。

### 5.2 分批摘取并处理

[__release_sock](../net/core/sock.c#L3243) 的结构为：

```text
while (sk_backlog.head 非空):
    在 slock 下：摘走当前整批 skb，head/tail 清空
    释放 slock                           owned 仍为 1

    遍历当前批次：
        sk_backlog_rcv(S, skb)
          → tcp_v4_do_rcv(S, skb)
            → tcp_rcv_established()
              → 正常 TCP 接收和记账
        批次较长时可 cond_resched()

    重新获取 slock，检查是否来了新一批

清零 sk_backlog.len
返回 release_sock，仍持 slock
```

### 5.3 同时到来的其他执行者会怎样

```text
进程 A：消费 backlog                   收包 CPU / 进程 B

owned = 1
在 slock 下摘走批次 [a, b]
释放 slock
处理 a，修改 forward_alloc
                                        收包 CPU 获取 slock
                                        看到 owned == 1
                                        将 c 放到新的 sk_backlog
                                        释放 slock
处理 b，修改 forward_alloc
                                        进程 B 尝试 lock_sock
                                        看到 owned == 1，等待
重新获取 slock
摘走并处理下一批 [c]
```

因此此时修改协议记账的仍只有当前所有者 A。收包 CPU 只更新新的 backlog，
不会碰 A 已摘下的私有批次。`cond_resched()` 允许 CPU 调度其他任务，但不释放
socket 所有权；其他 TCP 访问者仍须遵守 owned 协议。

最后确认队列为空、清零 backlog 长度、清除 owned 都在 slock 的保护下完成。
新收包者不会在“最后一次检查为空”与“放弃所有权”之间插入未受处理的一批数据。

### 5.4 flush 和等待数据

`__sk_flush_backlog()` 可以在已持有 socket 所有权时主动处理积压，而不放弃所有权。
例如当前 `tcp_recvmsg_locked()` 中存在该调用，`tcp_sendmsg_locked()` 也可经
`sk_flush_backlog()` 调用它。

如果 recv 真正需要等待数据，`sk_wait_data → sk_wait_event` 会先
`release_sock()`，完成积压处理并放弃所有权，再等待事件；返回继续操作前重新
`lock_sock()`。不能把整个系统调用期间都想象成永久持有所有权，应该逐段检查。

## 6. psock 的最后一份引用问题：两个队列怎样关联同一个 skb

在正常 `SK_SKB → ingress` work 中，传入 `take_ref=true`。
[sk_psock_skb_ingress_enqueue](../net/core/skmsg.c#L545) 执行：

```c
msg->skb = skb_get(skb);
sk_psock_queue_msg(psock, msg);
sk_psock_data_ready(sk, psock);
```

`skb_get()` 增加的是同一个 `struct sk_buff` 的 `users` 引用数；它不创建新的 skb。

```text
psock.ingress_skb ── 队列持有的引用 ──┐
                                    ├── skb X
psock.ingress_msg ── msg->skb 引用 ───┘
```

`ingress_msg` 链的是 `sk_msg`，不是把 skb 的同一组链表指针插到两个队列。
两个持有者共享 skb 生命周期，但使用不同的队列组织。

worker 此时持有 `work_mutex`，应用 recv 会拿 `lock_sock()`；它们没有共同的
socket 互斥关系。msg 一旦发布，应用就能读，不需要等 work 从 `ingress_skb` 出队。

## 7. 两种释放顺序，destructor 运行在哪个上下文

以下引用数是所有权关系的逻辑表示。`skb_unref()` 对最后一份引用有快速路径，
不要求在调试输出里实际观察到 `users=0`。

### 7.1 worker 先释放

```text
worker：msg->skb = skb_get(X)             引用数 1 → 2
worker：发布 msg
worker：从 ingress_skb 出队，kfree_skb(X) 引用数 2 → 1

应用：lock_sock(S)
应用：__sk_msg_recvmsg() 读完 msg
应用：kfree_sk_msg() → consume_skb(X)     最后一份引用
       → __kfree_skb → skb_release_head_state → sock_rfree
应用：release_sock(S)
```

这次 `sock_rfree()` 在应用拥有 S 的上下文中运行，接收记账得到 socket 锁协议保护。

### 7.2 应用先释放

```text
worker：msg->skb = skb_get(X)             引用数 1 → 2
worker：发布 msg，尚未从 ingress_skb 出队

应用：lock_sock(S)
应用：读完 msg，consume_skb(X)           引用数 2 → 1，不执行 destructor
应用：release_sock(S)

worker：出队，kfree_skb(X)                最后一份引用
        → __kfree_skb → skb_release_head_state → sock_rfree
          → sk_mem_uncharge(S, X->truesize)
```

第二种顺序完全可达，例如另一 CPU 的应用在 work 发布消息后立即读完，而 worker
还没执行队尾的出队释放。`sock_rfree()` 不会自动获取 socket 锁：它对
`sk_rmem_alloc` 做原子减法，再调用会修改 `sk_forward_alloc` 的 `sk_mem_uncharge()`。

所以最后的 uncharge 在仅持 `work_mutex` 的 worker 中运行，可以与新的 TCP 收包
或用户收发并发。即使应用尚未 `release_sock()`，worker 也不会等待它，问题仍然存在。

引用计数在这里保证 X 不会在仍有持有者时释放；它不保证最终释放者处于正确的
socket 记账临界区。这一证明本身描述的是记账数据竞争，不是引用计数失效导致的 UAF。

## 8. 普通 TCP 为什么没有同样的最终释放缺口

### 8.1 原生 recv 在 socket 所有权内消费

普通路径为：

```text
tcp_recvmsg()
  → lock_sock(S)
  → tcp_recvmsg_locked()
    → 消费 receive_queue 的数据
    → 读完 skb：tcp_eat_recv_skb()
  → release_sock(S)
```

backlog 中转入 receive_queue 时，是转移那份队列所有权，没有像 psock 这样
额外留下“worker 对同一个 skb 的引用”并交给独立 worker 日后释放。

### 8.2 延迟释放前先结算 socket 记账

当前 [tcp_eat_recv_skb](../net/ipv4/tcp.c#L1614) 有明确的处理：

```c
__skb_unlink(skb, &sk->sk_receive_queue);
if (likely(skb->destructor == sock_rfree)) {
	sock_rfree(skb);
	skb->destructor = NULL;
	skb->sk = NULL;
	return skb_attempt_defer_free(skb);
}
__kfree_skb(skb);
```

顺序是：

1. 在 TCP 的受保护上下文中调用 `sock_rfree()`，结算接收内存。
2. 清除 destructor 和 socket owner。
3. 将已经没有 socket 记账动作的 skb 交给可能的延迟释放。

因此即使 [skb_attempt_defer_free](../net/core/skbuff.c#L7306) 把物理释放安排到
另一个 CPU，那里也不会再次调用 `sock_rfree()` 修改 S 的 forward allocation。
该函数还检查待延迟释放 skb 是否仍带有 destructor；真正的依据是上游已经清除它，
不只是这个检查本身。

### 8.3 共享数据页不等于共享同一个 skb 的析构上下文

原生 TCP 可能涉及 clone、零拷贝或共享数据页。需要分别看：

- `skb->users`：同一个 skb 头部对象的引用；psock 的 `skb_get()` 属于这一类。
- `skb_shared_info.dataref` / page 引用：数据存储的共享，不等同于前者。
- `skb->destructor` / `skb->sk`：该 skb 与 socket 记账的关联。

例如当前 `__skb_clone()` 新建的 skb 有自己的 users，并把其 sk/destructor
初始化为空；共享的是数据引用。不能因 TCP 存在共享页或延迟页释放，就推断目标
socket 的 `sock_rfree()` 一定也被延期到无锁上下文。

## 9. 对比及适用范围

| 问题 | 普通 TCP backlog / 原生 recv | 当前 psock ingress work |
|---|---|---|
| 处理时可能不持 slock？ | 是，但 backlog 消费者仍有 owned | 是，且没有取得 socket owned |
| 新 TCP 收包是否能直接与其做协议记账？ | owned=1 时只排 backlog | worker 本身不能阻止 |
| 应用是否可与消费者并发进入 socket 临界区？ | 同一 owned 阻止并发进入 | 应用的 socket 锁与 work_mutex 不互斥 |
| 入等待队列是否立即 charge 目标 socket？ | 普通 sk_backlog 先记 backlog.len | work 的 owner 转换会修改 forward allocation |
| 最终 skb 释放能否交给其他 CPU？ | 可以；已先撤销 socket 记账并清 destructor | 最后释放者可能仍携带 sock_rfree |

如果 S 已经挂上 sockmap，普通 TCP backlog 消费期间同样可能调用接收 verdict，
再把数据交给 `sk_psock_backlog()`。这段异步交接以后仍然面临本文的 psock 缺口；
原来 TCP 所有者的 owned 不会被“传递”给后续 worker。

本次为源码检查，无内核实现变更，也没有新增运行复现结果。
