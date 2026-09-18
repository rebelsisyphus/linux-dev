# psock ingress 的共享状态审计与锁机制分析

日期：2026-09-15。基线：`45c13f3f9e3bb15fd89ff2864c6f627a3b4b4229`。
本文承接 [work 锁分析](backlog-locking.md) 和
[普通 TCP backlog 对比](tcp-backlog-vs-psock.md)。仅分析代码，没有修改内核实现，
没有在匹配内核上运行复现或 KCSAN。

## 1. 结论与适用条件

**应该修正同步边界，不能仅把 `sk_forward_alloc` 或几个计数改为 atomic。**
但也不能直接在整个 `sk_psock_backlog()` 外层加 `lock_sock()`：当前 egress
使用要求 socket 未锁定的发送入口，公共 ingress helper 又会被已持锁的同步接收路径调用。

设 `S` 为 self-redirect 的源和目标，接收方向为 `BPF_F_INGRESS`。

| 状态/操作 | 当前代码结论 | 触发条件 |
|---|---|---|
| `sk_forward_alloc`、schedule/charge/uncharge/reclaim | 无共同 socket 串行化；不仅存在丢失更新，还存在整套记账操作交错的问题 | skb ingress worker 与同一 S 的 TCP RX、recv 或其他记账路径并行 |
| `psock->ingress_bytes` | 明确存在 worker 与 parser 的并发读写，可导致 `tp->copied_seq` 结算错误 | 启用并挂载 stream parser，worker 的 enqueue 落在 parser 本次读取的清零和结算之间 |
| work 获取/释放的 psock 引用 | 存在获取新 psock、释放旧 psock 的检查窗口；引用操作本身是原子的，但对象不匹配 | 旧 work 已通过 TX 检查、尚未 get，此时删除最后一条 map 关联再加入同一 S |
| 拆除时 purge/cork free 的 socket 记账 | 旧 psock 的延迟销毁不持 socket 锁；等待旧 work 不会停止仍存活 S 的正常收发 | 删除 map 关联，fd 仍打开，旧 psock 有待清理的已记账数据 |
| `skb->users` | 本次未发现其引用加减本身的错误；最后一个引用的释放上下文仍是记账问题 | 不应把原子 refcount 与 socket 状态互斥混为一谈 |
| 消息链、`msg_tot_len`、SG 消费、work state | 下文列出的正常路径已有队列锁、socket 消费串行化或 work 生命周期保护，未证明额外功能性竞态 | 指同一正常存活 psock；不包含前述引用对象错配的后续影响 |

需要区分范围：**没有 parser、map 也不变化**时，不能把上表 parser 和重新挂载
问题说成该配置必然触发的问题。该窄场景已明确的主要缺口仍是 socket 内存记账
及 skb 最终释放边界。无 parser self-ingress 的 `copied_seq` 重复推进另见
[主分析第 8 节](analysis.md#8-当前源码的-self-ingress-序号问题)，它不需要并发，
也不会因加锁自动消失。

## 2. 审计的调用边界

worker 路径从 [sk_psock_backlog()](../net/core/skmsg.c#L671) 开始：

```text
TX_ENABLED 检查
sk_psock_get(psock->sk)                 按 sk 当前关联查找 psock
mutex_lock(work_mutex)
  peek ingress_skb
  sk_psock_handle_skb()
    ingress:
      sk_rmem_schedule()               通用分支检查/申请额度
      skb_set_owner_r()                orphan、改 owner、charge
      sk_psock_skb_ingress_enqueue()
        构造 SG
        ingress_bytes += len           parser 配置开启时
        skb_get()                      work 给消息增加引用
        sk_psock_queue_msg()           ingress_lock 保护发布
        sk_psock_data_ready()          callback_lock 保护回调
    egress:
      skb_send_sock()                  下层发送自己处理 socket 锁
  dequeue ingress_skb
  kfree_skb()                          可能是最后引用，执行 sock_rfree
mutex_unlock(work_mutex)
sk_psock_put(psock->sk, psock)          释放 work 所属的 psock
```

`work_mutex` 只有 psock worker 使用。TCP RX 和应用 recv 不拿它。
`ingress_lock` 保护队列及相关状态，不能覆盖它之前的 charge、之后的最终释放，
TCP 原生记账路径也不拿它。RCU 保护查找期间的对象生存期，不提供字段互斥。

## 3. `ingress_bytes`：独立于 forward allocation 的具体问题

### 3.1 两个访问端不在同一同步协议中

[tcp_bpf_strp_read_sock()](../net/ipv4/tcp_bpf.c#L690)：

```c
psock->ingress_bytes = 0;
copied = tcp_read_sock_noack(sk, desc, recv_actor, true,
                            &psock->copied_seq);
/* 错误分支略 */
tp->copied_seq = psock->copied_seq - psock->ingress_bytes;
tcp_rcv_space_adjust(sk);
__tcp_cleanup_rbuf(sk, copied - psock->ingress_bytes);
```

这里的计数是**本次 parser 读取中同步进入本 socket 应用队列的字节数**，
用于区分立即消费的 redirect 与留到应用读取时消费的 SK_PASS。
它不是所有时间、所有来源 enqueue 字节的累计量。

但是 [sk_psock_skb_ingress_enqueue()](../net/core/skmsg.c#L545) 无条件执行：

```c
#if IS_ENABLED(CONFIG_BPF_STREAM_PARSER)
psock->ingress_bytes += len;
#endif
```

该函数既被同步 SK_PASS 调用，也被异步 redirect worker 调用；这段更新在
`ingress_lock` 之外。parser 接收及 parser work 遵循 socket 锁协议，见
[strp_data_ready() / do_strp_work()](../net/strparser/strparser.c#L380)，
但 psock ingress worker 没有参加这个协议。

### 3.2 一个消息就足以构造的 self-redirect 时序

条件：TCP stream parser 把 N 字节解析成一个完整消息，verdict 将其 ingress
redirect 回 S；不发生分配/SG 映射失败，没有 FIN，没有其他应用读取。
初始 `tp->copied_seq = psock->copied_seq = C`。

| 顺序 | CPU 0：parser，遵循 socket 锁协议 | CPU 1：psock worker，仅 work mutex |
|---|---|---|
| 1 | `ingress_bytes = 0`；进入 `tcp_read_sock_noack()` | |
| 2 | `sk_psock_strp_read()` 产生 N 字节消息；verdict 将其排入 S 的 ingress_skb 并调度 work | |
| 3 | 尚未完成本次 parser 结算 | 通用 ingress 成功，`ingress_bytes += N`；发布消息 |
| 4 | 读取返回 N，`psock->copied_seq = C + N` | |
| 5 | `tp->copied_seq = C + N - N = C`；cleanup 的参数为 0 | |

[sk_psock_strp_read()](../net/core/skmsg.c#L1075) 在运行 verdict 后把
`skb->sk` 清为 NULL；`tcp_eat_skb()` 对 parser 标记的 skb 不推进序号。
因此这个首次成功的通用 ingress 不走 self helper，创建的 `msg->sk` 保持 NULL。
之后应用读取 N 字节，`copied_from_self` 为 0，也不会替该消息补上 N 的消费序号。
证据见 [通用 ingress](../net/core/skmsg.c#L591)、
[__sk_msg_recvmsg()](../net/core/skmsg.c#L413) 和
[tcp_bpf_recvmsg_parser()](../net/ipv4/tcp_bpf.c#L221)。

如果 CPU 1 等 CPU 0 完成结算后才 enqueue，则 CPU 0 读取到的 counter 为 0，
`tp->copied_seq` 为 C+N。**同一报文的序号结果取决于 work 与 parser 的交错。**
这里证明的是消费序号与清理输入错误；具体窗口停滞、吞吐下降等外部表现仍需运行验证。

还存在跨批次情况：上一次 parser 产生的 A 字节在本次 B 字节读取期间才由 work
enqueue，本次就会减去 A。若 A>B，`copied - ingress_bytes` 甚至发生无符号下溢
后传给接收 int 的 cleanup。无需假定两个 `+=` 恰好丢失一次更新，已能证明批次归属错误。

### 3.3 为什么 atomic 不能解决

即使清零、加法、读取全部改为原子操作，上一表仍是完全合法的原子操作顺序。
问题是异步 enqueue 混入了另一段执行中的 parser 结算。

可行方向是让 ingress 与 parser 参加同一 socket 串行化，或把 parser 临时计数
明确限定在同步解析事务中，不由异步目标入队更新。后者仍需处理内存记账和最终释放，
不能作为整个并发问题的替代修复。两种方向都需要验证 SK_PASS、redirect、
不同 socket、部分消息和失败重试的序号语义。

## 4. psock 引用：原子操作也可能操作了不同对象

### 4.1 代码上的身份差异

worker 的 `psock` 来自 work 的 `container_of()`，身份在本次执行中固定。
但 [sk_psock_get()](../include/linux/skmsg.h#L495) 根据 `sk_user_data` 查找
**当前** psock 并加引用。worker 丢弃了其返回的指针：

```c
/* psock 是该 work 所属的对象 */
if (!sk_psock_test_state(psock, SK_PSOCK_TX_ENABLED))
        return;
if (!sk_psock_get(psock->sk))
        return;
/* 使用 psock */
sk_psock_put(psock->sk, psock);
```

### 4.2 删除再加入的可达窗口

条件：S 的 fd 保持打开，仍是可加入 map 的已连接 socket；旧 psock A 仅剩 map
关联的引用，已排 work，但 work 尚未获取自己的引用。

```text
CPU 0：A 的 work                       CPU 1：map 操作
读到 A.TX_ENABLED == 1
暂停在 sk_psock_get() 之前
                                      删除 S 的最后一条关联
                                      A.refcnt: 1 -> 0
                                      sk_psock_drop(A):
                                        恢复 S 的 proto/callback
                                        S.sk_user_data = NULL
                                        stop A
                                        queue_rcu_work(A.destroy)
                                      把仍打开的 S 再加入 map
                                      创建 B，S.sk_user_data = B
sk_psock_get(S) 获取 B 的引用
返回指针被丢弃，仍使用 A
退出时 sk_psock_put(S, A)
  A 的零引用被再次减一
  B 的这次引用没有对应 put
```

[__sock_map_delete()](../net/core/sock_map.c#L415) 只有 map 自旋锁，随后经
[sock_map_unref()](../net/core/sock_map.c#L179) 释放 psock 引用，没有
`lock_sock()`，也不等待旧 work。系统调用入口
[map_delete_elem()](../kernel/bpf/syscall.c#L1900) 的 RCU 读临界区不会补上 socket 锁。
其 `maybe_wait_bpf_programs()` 对 sockmap 不执行同步等待。

[sk_psock_drop()](../net/core/skmsg.c#L889) 先 detach，再排延迟销毁；
[sock_map_link()](../net/core/sock_map.c#L217) 在没有当前 psock 时允许初始化 B。
旧析构尚未运行，或者在 `cancel_delayed_work_sync()` 等 CPU 0，均不阻止这个顺序。

入口的 TX 检查能拒绝“开始执行时已经 stop”的 work，不能覆盖“检查后、get 前”
的窗口。本地提交 `76be5fae32febb1fdb848ba09f78c4b2c76cb337`
引入的正是这个预检查；当前代码没有把检查与引用对象绑定合成一个操作。

在上述时序下，源码可推导出**引用获取/释放错配、旧 refcount 下溢告警条件以及新引用泄漏**。
`refcount_t` 的下溢处理见 [refcount_dec_and_test()](../include/linux/refcount.h#L436)。
不能仅凭这个顺序进一步声称已经证明 worker 访问已释放内存：旧析构同步取消 work
之后才释放 A 及其持有的 S 引用，该机制仍保护其物理内存生存期。

历史 [2025-05-22 讨论](https://lists.openwall.net/linux-kernel/2025/05/22/1577)
说明了 cancel work 与最终 sock_put 的生存期保障；这里检查的是更具体的
“get 和 put 是否指向同一 psock”，不是否定该生存期保障。

### 4.3 修复性质

应围绕**work 所属对象的引用、stop 状态和 detach 顺序**建立协议。
可以评估直接对 work 所属 psock 做 `refcount_inc_not_zero()`，并保持其与 close
等待的配合；不能只保存新查找到的 B 后继续处理 A，也不能仅再加一次无共同同步的
TX 检查就声称完成修复。

仅把 `lock_sock()` 放进 skb ingress helper 不覆盖 work 入口。即便更早拿 socket
锁，map 删除本身也不拿这把锁；必须证明引用绑定，而不是假定它隐式互斥了全部 map 操作。

## 5. 拆除清理：停止旧 work 不等于停止 S 的收发

[sk_psock_destroy()](../net/core/skmsg.c#L864) 的关键顺序是：

```text
sk_psock_done_strp()
cancel_delayed_work_sync(psock->work)
__sk_psock_zap_ingress()
  清空 ingress_skb -> sock_drop -> kfree_skb
  清空 ingress_msg -> sk_msg_free -> consume_skb / sk_mem_uncharge
...
sock_put(psock->sk)
```

这段清理不持 socket 锁。考虑 worker 已把一个 self-ingress skb 发布为消息并退出，
应用未读，此时删除 S 的最后一个 map 关联。A 被 detach，S 恢复原生 TCP，但 fd
仍在使用；之后 A 的 RCU work 清理该消息，最终执行 S 的 `sock_rfree()`。
与此同时 S 可以正常接收新包或执行原生 send/recv。

因此 `cancel_delayed_work_sync()` 能让 A 的队列清理避开 A 的 worker；RCU 宽限期
能等待 detach 前的查找者；**两者都不能排除之后仍在使用同一个 S 的协议记账操作**。
旧 psock 与新 psock 即使各有独立锁，最终仍写同一份 S 的记账字段。

这是 forward allocation 同一问题类别的另一个入口，不能只修 worker 成功路径。
此外 [sk_psock_stop()](../net/core/skmsg.c#L854) 中的 cork free 也需要纳入审计：
它可能经最后一次 map unref 调用，只有 ingress_lock，释放暂存的 SK_MSG 数据时也会
uncharge。纯 SK_SKB self-redirect 不要求存在 cork；此项是启用 SK_MSG cork 后的扩展。

给清理补 socket 同步时，应在等待 parser/work **之后**再进入需要记账的临界区。
不能拿着 S 的 socket 锁等待一个可能正在请求该锁的 worker 退出。

## 6. 已检查但没有据此认定额外故障的状态

| 对象 | 代码依据与边界 |
|---|---|
| `skb->users` | worker 在发布 msg 前调用 skb_get；worker 与 msg 各持一份引用，`consume_skb`/`kfree_skb` 原子减引用。最后一份归谁决定 destructor 上下文，不能把它直接称为引用竞争/UAF。 |
| skb 的 SG/数据 | `skb_to_sgvec()` 与必要的 linearize 在 msg 发布前完成；正常成功后 worker 不再修改该 msg 的 SG。recv 修改的是 msg 的 SG offset/length，worker 保留自己的 skb 引用。此路径未证明存在发布后边读边 linearize。 |
| `ingress_msg` 链和 `msg_tot_len` | 发布、移除及长度修改使用 ingress_lock；消费者由 lock_sock 串行化；recv 持有 psock 引用，正常析构不能同时释放其正在读取的 msg。ioctl 的 READ_ONCE 长度快照不是无锁加减。 |
| `ingress_skb` | 入队/出队有 skb 队列锁；worker 的 peek 在唯一消费者语境下使用，销毁前 cancel work；生产者只尾部追加。 |
| `work_state.len/off` | 工作线程通过 work_mutex 串行处理；设置时还有 ingress_lock；析构先 cancel work。未发现其他正常路径无锁修改这两个字段。 |
| TX state | 位操作为原子操作，入队在 ingress_lock 内再次检查。它不保证检查后的整个 work 永远不会被 stop，因此不能用于第 4 节的引用身份推理。 |
| `sk_rmem_alloc` | 加减本身使用 atomic；这不代表它与 forward allocation、内存额度的一组变化具有事务性。 |
| `sk_rcvbuf`、`sk_reserved_mem`、`sk_wmem_queued` | worker 的限额/回收计算读到它们，正常 socket 路径可能改变它们。它们参与内存记账审计，但 rcvbuf/压力检查允许近似，不能仅凭检查与增加分离就断言严格容量上限被破坏。 |
| `sk_err` | worker 的 sk_psock_report_error 写入，SO_ERROR 等读取可异步发生；sock_error 本身使用 data_race 快速检查和 xchg 清除。不能仅据无 socket 锁就认定另一个确定的错误丢失或唤醒故障。 |
| data_ready 等回调 | 正常当前 psock 的回调变更与调用使用 sk_callback_lock；该锁保护回调指针，不保护此前的内存记账和 ingress_bytes。 |

主要证据：[消息队列辅助函数](../include/linux/skmsg.h#L339)、
[消息消费](../net/core/skmsg.c#L413)、[skb 引用实现](../include/linux/skbuff.h#L1286)、
[sock_error()](../include/net/sock.h#L2555)。这个表的“未证明”不等于完整子系统无其他 bug。

## 7. 为什么仅改 `sk_forward_alloc` 为 atomic 仍不够

当前问题不仅是 `F = F + n` 丢失更新。
[sk_mem_reclaim()](../include/net/sock.h#L1618) 先计算可回收额，再调用
[__sk_mem_reclaim()](../net/core/sock.c#L3490) 扣 F，并归还协议/memcg 额度。
这一组合必须避免重复消费同一份可回收额。

作一个反例：**假定已经把 F 的每次加减和读取都改为原子操作**，并设无 reserved
memory、初始 F=0。P 表示 PAGE_SIZE；两个上下文各退还 P 字节额度：

```text
A：atomic_add(P, F)                     F = P
A：计算可回收 P，暂停
B：atomic_add(P, F)                     F = 2P
B：计算可回收 2P，回收 2P              F = 0；归还 2 页
A：按此前的结果回收 P                  F = -P；又归还 1 页
```

两次实际退还共 2 页，却归还了 3 页。全部单次原子操作都成功，组合仍然错误。
若选择无锁记账，必须重做额度预留/领取、回滚、回收以及其他字段的一致性协议，
不能把类型替换当作局部修复。单独用 READ_ONCE/WRITE_ONCE 更不能提供这种保证。

## 8. 建议的修复方向与必须保留的边界

### 8.1 TCP ingress 优先复用 socket 串行化

对本次 TCP 路径，更自然的方向是让异步 ingress 参加现有 socket 锁协议：

- 内存 schedule、owner 转换、charge、发布时的 parser 计数和必要的回滚在有序范围内完成。
- **同一保护范围必须考虑 worker 的 `kfree_skb()`。** 只锁 ingress helper 然后解锁，
  msg 仍可被 recv 先读完，worker 随后在锁外释放最后引用，旧问题仍在。
- purge、队列拒收、失败重试、cork 清理等可能执行 uncharge 的路径一并处理。
- 保留 ingress_lock 来保护队列/stop 原子性；socket 锁与队列锁承担不同职责。
- 与 socket RX 互斥要遵循 `lock_sock()` / owned 协议；在进程上下文只拿
  `bh_lock_sock()` 而不检查 owned，不能排除正在持有进程 socket 锁的 recv。

也可评估在正确锁上下文提前解除 skb 的 socket 记账，之后只异步释放无 owner 的
存储；原生 [tcp_eat_recv_skb()](../net/ipv4/tcp.c#L1614) 有这种职责划分。
但对 psock 不能直接照抄 orphan：消息尚未读取时，接收内存归谁记账、何时释放额度，
需要另行设计并守住接收缓冲限制。

### 8.2 不宜直接给整个 worker 或公共 helper 套锁

1. **egress 入口要求未锁。** [skb_send_sock()](../net/core/skbuff.c#L3431)
   进入 sendmsg_unlocked，再经 TCP 或 BPF sendmsg 自行加锁。
   外层持同一 socket 锁再调用它会形成重复获取。不能机械恢复旧锁而保留当前发送入口。
2. **同步 ingress 已有锁上下文。** SK_PASS 从 TCP 接收或 parser 路径直接调用公共
   self helper，在那里无条件 lock_sock 既可能处于 softirq，也可能重复获取同一锁。
   应在异步入口设计同步，或区分明确的已锁/未锁调用契约。
3. **可能睡眠。** 通用 ingress 有 GFP_KERNEL 分配，不能把整个过程塞进
   ingress_lock/sk_lock.slock 自旋临界区来解决问题。
4. **取消 work 的顺序不能倒置。** close 当前先 release_sock 再 cancel work，
   正是避免持 socket 锁等待 work。parser work 的同步取消也需保持同样约束。
5. **引用身份独立修复。** 第 4 节发生在 ingress 之前，且 map 删除不拿 socket 锁。
6. **公共 skmsg 服务多个协议。** 即使先解决 TCP，也要明确锁分支及协议适用范围，
   不把 TCP 的方案未经审计推广到 UDP、UNIX、vsock。

### 8.3 代码改动前应明确的三个不变量

1. 同一个 S 上，所有要求 socket 串行化的记账变更，包括最终 destructor，
   必须参加同一个协议，或已提前从该记账协议中解除。
2. parser 本次的消费结算只能使用属于本次解析语义的数据，不能被异步目标 enqueue
   无锁改变。
3. worker 获取、使用、释放的是同一个 psock 实例，且与 detach/close 等待配合。

这三个不变量比“给某个变量加 atomic”或“函数开头放一把锁”更适合作为修复验收条件。

## 9. 后续验证范围（本次未执行）

- 无 parser self-ingress：持续 RX + 并发 recv，尤其消息发布后 recv 先完成、worker
  最后释放 skb 的时序；跟踪 owner/destructor、F、rmem 和回收页数。
- 有 parser self-ingress：固定完整消息 N，在 enqueue 与 parser 清零/结算处观察
  时序，对照 C、psock copied_seq、ingress_bytes、msg->sk 及 recv 后 tp copied_seq。
- 保持 fd 打开，删除再加入 map：同时记录 work 所属 psock、sk_psock_get 的返回值、
  引用数、TX 状态、detach 和销毁时刻。
- 删除 map 后保留未读消息并继续原生 TCP 收发：覆盖旧 psock 的 purge。
- 分配失败、SG linearize 失败、EAGAIN 重试、close 与取消、SK_PASS 快路径、
  egress 及 SK_MSG 回归；锁方案要检查 lockdep，记账/引用时序要用对应观测点验证。

本次证据来自实际源码路径与可达交错推导。没有把文中时序作为已运行的复现结果，
也没有将所有无锁读取都归类为独立的功能性缺陷。
