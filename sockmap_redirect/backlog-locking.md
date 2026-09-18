# sk_psock_backlog：何时执行、socket 锁与接收记账并发

补充分析日期：2026-09-15。
源码基线仍为 `45c13f3f9e3bb15fd89ff2864c6f627a3b4b4229`。

## 1. 结论

**当前代码的 `sk_psock_backlog()` 不持有目标 socket 锁。其 skb ingress 路径也没有
补拿 socket 锁，却会修改 `sk_forward_alloc`。因此它与同一 socket 的 TCP 收包、
用户收发之间缺少共同的记账同步机制，可发生读改写丢失。**

`work_mutex` 保护 work 处理的串行性，`ingress_lock` 保护 psock 队列、状态与消息长度；
它们都不是 TCP 的 `sk_lock`，也没有覆盖 `skb_set_owner_r()` 内的记账操作。
`sk_rmem_alloc` 是原子变量，并不能让旁边普通整数 `sk_forward_alloc` 的操作也变成原子。

这是当前源码上的可达并发路径分析，本次没有进行 KCSAN 或测试内核运行复现。
这个问题与主分析中的 `copied_seq` 重复推进是两个独立问题。

socket 所有权和最终 skb 释放的详细展开，以及与普通 TCP backlog 的对照，见
[tcp-backlog-vs-psock.md](tcp-backlog-vs-psock.md)。

## 2. 什么时候调度 backlog

初始化位于 [sk_psock_init](../net/core/skmsg.c#L785)：

```c
INIT_DELAYED_WORK(&psock->work, sk_psock_backlog);
```

已对 `net/` 和 `include/` 下 psock work 的引用进行搜索；本基线的调度点如下：

| 调度位置 | 条件 | 延迟 |
|---|---|---|
| [sk_psock_skb_redirect](../net/core/skmsg.c#L991) | SK_SKB redirect 成功将 skb 放入目标 `ingress_skb`，包括 self ingress/egress | 0 |
| [sk_psock_verdict_apply](../net/core/skmsg.c#L1034) | SK_PASS 的直接 ingress 失败，或已有排队 skb 而需要保持顺序 | 0 |
| [sk_psock_write_space](../net/core/skmsg.c#L1066) | write-space 回调且 TX_ENABLED，尝试继续处理积压 | 0 |
| [sk_psock_backlog](../net/core/skmsg.c#L728) | 当前处理返回 `-EAGAIN`，保存剩余 off/len 后重试 | 1 jiffy |

`delay=0` 是立即排队供 worker 调度，不是同步调用该函数，不保证在当前收包回调
返回之前或之后的某个固定时刻完成。队列合并、work 是否 pending 和 CPU 调度也会
影响实际执行次数；调度调用次数不等于 worker 实际执行次数。

[schedule_delayed_work](../include/linux/workqueue.h#L853) 使用 `system_percpu_wq`。
worker 在进程上下文运行，可睡眠；per-CPU workqueue 不等于 socket 专属执行上下文，
不会阻止这个 socket 在其他 CPU 收包，也不会给 socket 加锁。

正常 `SK_MSG → 自身 ingress` 走 `bpf_tcp_ingress()`，直接创建并排入 `ingress_msg`，
其 payload 不需要经过这个 backlog。只有 write-space 回调等另外触发 work 时，
才可能看到该 socket 的空队列 work 被调度。

## 3. 执行时到底持有什么锁

### 3.1 work 主体

[sk_psock_backlog](../net/core/skmsg.c#L671) 的关键结构：

```text
检查 TX_ENABLED
sk_psock_get()                     引用，保护生命周期
mutex_lock(work_mutex)
  peek ingress_skb
  sk_psock_handle_skb()
  更新 work_state / 出队 / kfree_skb
mutex_unlock(work_mutex)
sk_psock_put()
```

不存在包住整个 work 的 `lock_sock()`、`bh_lock_sock()`，也没有设
`sk->sk_lock.owned = 1`。workqueue 调用函数不会继承当初调度者持有的锁。

### 3.2 分方向查看

| 路径 | socket 锁覆盖情况 |
|---|---|
| work → `sk_psock_skb_ingress` → self 或通用入口 | 当前没有 socket 锁；消息排队时短暂持 `ingress_lock` |
| work → `skb_send_sock` → `sock_sendmsg` → 普通 TCP send | `tcp_sendmsg()` 内部获取 socket 锁；不覆盖进入发送入口前或返回后的 work 代码 |
| work → egress → 目标 `tcp_bpf_sendmsg` | 发送入口按 SK_MSG 路径获取 socket 锁；redirect 时释放源锁再进入目标 |
| SK_MSG → `bpf_tcp_ingress` | 函数内明确 `lock_sock()`，是另一条已有锁的路径 |
| 接收 verdict → SK_PASS 直接调用 self ingress | 上游 TCP 正在串行处理 socket；与 work 调用同一个 helper 的上下文不同 |

因此“egress 最后会拿锁”不能证明 ingress 安全；“self ingress 的直接调用者有锁”
也不能证明 work 调用它时有锁。

### 3.3 每种保护的边界

| 机制 | 实际保护 | 不能据此证明的事情 |
|---|---|---|
| `work_mutex` | 本 psock work 处理及其状态的串行化 | 与 TCP 收包或应用 recv 的互斥：这些路径不拿该 mutex |
| `ingress_lock` | 队列变更、TX_ENABLED 检查、msg_tot_len 更新 | 整个 socket 的内存记账互斥；charge 发生在拿该锁之前 |
| `ingress_skb` 的队列自旋锁 | skb 链表操作 | skb 出队前后的数据处理和 socket 字段更新 |
| RCU / psock、socket 引用 | 查找和对象生命周期 | 普通字段的并发读改写 |
| `sk_callback_lock` | 回调安装、恢复与调用协调 | enqueue 之前的 `skb_set_owner_r` 记账 |
| `sk_rmem_alloc` 原子操作 | 这个计数自身的加减 | 同步更新 `sk_forward_alloc` 或全局内存预算 |

## 4. 正常 TCP 怎样保证进程与收包互斥

`lock_sock()` 不意味着进程一直持有 `sk_lock.slock` 自旋锁。它获得的是
socket 的进程侧所有权，设置 `sk_lock.owned`；获取所有权时与 `slock` 协调。
当前 64 位实现还可能通过 combined 字段的 cmpxchg 快速获得该状态。

[tcp_v4_rcv](../net/ipv4/tcp_ipv4.c#L2244) 的正常收包代码：

```c
bh_lock_sock_nested(sk);
if (!sock_owned_by_user(sk))
	tcp_v4_do_rcv(sk, skb);
else
	tcp_add_backlog(sk, skb);
bh_unlock_sock(sk);
```

应用或 worker 若已经 `lock_sock(S)`：新收包侧仍可能运行并获取 slock，
但看到 `owned=1` 就只排入 `S.sk_backlog`，不会并发执行 TCP 正常接收记账。
`release_sock()` 在放弃所有权前处理积压，维持这种串行化。

**当前 ingress work 没有参与这个协议。** 它仅拿 `work_mutex`，不能让
`sock_owned_by_user(S)` 变成 true。没有其他进程持锁时，收包侧可以继续
`tcp_v4_do_rcv → tcp_rcv_established → tcp_queue_rcv`。
如果另一个进程正在持 socket 锁，work 也不会等待它，两者同样可能并发修改记账。

## 5. sk_forward_alloc 不是原子更新

定义为普通 `int`。其更新入口是
[sk_forward_alloc_add](../include/net/sock.h#L1125)：

```c
WRITE_ONCE(sk->sk_forward_alloc, sk->sk_forward_alloc + val);
```

这是读取旧值、计算、写回。`WRITE_ONCE` 约束写入访问，不提供多个写者之间的
原子加减，也不获取锁。给右侧再加一个 `READ_ONCE` 同样不能解决丢失更新。

### 5.1 work 的可达修改点

```text
sk_psock_backlog                       仅有 work_mutex
  → sk_psock_skb_ingress
    → skb->sk == S
      → sk_psock_skb_ingress_self
        → skb_set_owner_r(skb, S)
          → skb_orphan                 旧 destructor 若是 sock_rfree，也会 uncharge
          → atomic_add(truesize, sk_rmem_alloc)
          → sk_mem_charge(S, truesize)
            → sk_forward_alloc_add(S, -truesize)
```

这里不能以 `sk_has_account()` 为由排除 TCP：
[tcp_prot.memory_allocated](../net/ipv4/tcp_ipv4.c#L3381) 非 NULL，
因此 TCP 会执行 `sk_mem_charge()` 的记账分支。

通用 ingress 还有 `sk_rmem_schedule → __sk_mem_schedule`，必要时增加
forward allocation 预算；该操作也不在 socket 锁下。

### 5.2 TCP 收包的修改点

```text
tcp_v4_rcv                            持 S.sk_lock.slock，检查 owned
  → tcp_v4_do_rcv → tcp_rcv_established
    → tcp_queue_rcv                    选取未合并、正常排队的 skb
      → skb_set_owner_r(skb, S)
        → sk_mem_charge
          → sk_forward_alloc_add
```

这两个写入点操作同一个 S，但前者的 `work_mutex` 与后者的 `sk_lock` 没有互斥关系。
收包两次涉及的是不同 skb；不需要让同一个 skb 被两个 TCP 收包者同时操作。

### 5.3 可达的丢失更新时序

前提：skb A 已通过 self-redirect 排到 work，应用尚未读取它；TCP 又接收 skb B。
选取当时 forward allocation 足以覆盖两次 charge，B 不与原生队尾合并，
因此不需要依赖分配失败、close 或报文乱序。以下数值只演示运算，不是实测值。

```text
S.sk_forward_alloc = 3072
A.truesize = B.truesize = 1024

CPU 0：ingress worker                 CPU 1：同一 S 的 TCP 收包
持 work_mutex                         持 sk_lock.slock，owned == 0

charge(A) 读旧值 3072
                                      charge(B) 读旧值 3072
                                      写回 2048
写回 2048

预期：3072 - 1024 - 1024 = 1024
实际：2048，丢失了一次 charge
```

后续 B 被 verdict 摘走、orphan 或释放，不会自动修复已经丢失的 A/B 更新；
正常后续加减以错误的值继续。`sk_mem_reclaim()` 还会根据 forward allocation
计算应归还的页预算，所以问题不仅是统计显示偏差。

这里没有声称必然触发某个具体 WARN、内存泄漏数量或崩溃；这些后果需分别动态验证。

## 6. 还要追踪“谁释放最后一份 skb 引用”

只保护 ingress 创建消息时的 charge 还不足以完成整个记账审计。
入队过程先 `msg->skb = skb_get(skb)`，然后把 msg 发布给应用：

```text
worker：enqueue msg，skb 引用数为 2
应用：lock_sock → recv 读完 msg → consume_skb，引用数 2 → 1
worker：从 ingress_skb 出队 → kfree_skb，引用数 1 → 0
        → sock_rfree → sk_mem_uncharge → 修改 sk_forward_alloc
```

应用和 worker 的两个释放顺序都可能出现。上面的顺序中，最终 destructor 在
无 socket 锁的 worker 中执行，而不是在持有 socket 锁的应用 recv 中执行。
work 此时可能与新的收包或新的用户收发并发。

主分析第 9 节描述的是引用释放关系；“由应用读完触发释放”不能被理解为
“最终 `sock_rfree()` 必然运行在 recv 的 socket 锁内”。

## 7. 修正同步时应覆盖的范围

本次仅分析，未修改内核。若后续修复，需要让 work 中相关的预算申请、owner 转移、
charge/uncharge 以及可能触发最终 destructor 的释放，与同一个 socket 的原生
收发记账共享同步机制，并检查错误/重试分支。

需要避免两个具体的错误改法：

1. 在 `sk_psock_skb_ingress_self()` 开头无条件 `lock_sock()`。
   它也从 TCP SK_PASS 的直接路径调用，那里可能已在 socket 收包锁/softirq 上下文。
   应区分 work 入口与已串行化的直接入口。
2. 在整个 `sk_psock_backlog()` 外层加锁，却原样保留 `skb_send_sock()`。
   egress 会进入自行获取 socket 锁的 `tcp_sendmsg()`，造成同 socket 锁重入。
   必须同时核对 locked/unlocked 发送接口和 SK_MSG 可重入路径。

也不能只把 `sk_forward_alloc` 改成原子类型就宣称整体修复：预算检查、申请、
charge 和 reclaim 是多步操作，涉及其他 socket/协议记账状态。

## 8. 历史与验证边界

本地 git 历史确认，提交
`799aa7f98d53e0f541fa6b4dc9aa47b4ff2178e3`
（`skmsg: Avoid lock_sock() in sk_psock_backlog()`）
把 work 主体的 socket 锁替换成 `work_mutex`，同时将 egress 的
`skb_send_sock_locked()` 改为 `skb_send_sock()`。
它解释了为什么旧版本资料可能展示不同的锁结构；不能用历史版本的锁覆盖证明当前实现安全。

已核对所有当前 psock work 调度点、mutex 使用点、相关记账函数和 TCP 锁协议。
没有运行匹配内核/KCSAN，没有修改内核源码，也没有将网络上的候选补丁当作已合入的实现。
动态验证应使用匹配内核，在 self-ingress 的持续接收和并发读取下跟踪
`skb_set_owner_r`、最终 `sock_rfree` 的执行上下文、socket 锁状态及 forward allocation。
仅收发内容正确或队列锁无告警，不能证明这些普通字段不存在数据竞争。

更广范围的审计见 [concurrency-audit.md](concurrency-audit.md)：包含 parser 的
`ingress_bytes` 并发结算、删除再挂载时的 psock 引用绑定、析构 uncharge，以及
仅改 atomic 和机械扩大 socket 锁的局限。
