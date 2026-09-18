# 建议修改方案：引用、记账同步、消费序号

日期：2026-09-15。基线：`45c13f3f9e3bb15fd89ff2864c6f627a3b4b4229`。
这是基于 [共享状态审计](concurrency-audit.md) 的设计建议，没有修改内核实现。
文中的 diff 和调用结构用于说明修改位置，不是已经编译、测试的完整补丁。

## 1. 总体选择

建议优先复用 TCP 的 socket 锁协议，保留现有 skb 引用及队列同步，按三个问题组组织修改：

1. worker 必须获取和释放自身所属 psock 的引用。
2. TCP ingress 的 socket 记账和最终释放串行化，包含拆除清理。
3. parser 临时计数与 TCP 消费序号的语义独立修复。

不建议将 `sk_forward_alloc` 全局改为 atomic，也不建议新建一把只有 psock 使用的
记账锁：前者没有解决检查/预留/回收组合的交错，后者不能与原生 TCP 记账互斥。

## 2. 引用修复：直接获取 work 所属实例

修改 [sk_psock_backlog()](../net/core/skmsg.c#L671) 的引用获取：

```diff
- if (!sk_psock_get(psock->sk))
+ if (!refcount_inc_not_zero(&psock->refcnt))
        return;
```

保留退出处 `sk_psock_put(psock->sk, psock)`。这样加减操作必然作用于同一个对象。

依据：已进入执行的 work，其所属 psock 的物理内存由析构中的同步取消保护；
`refcount_inc_not_zero()` 决定本次执行能否取得一个有效引用。若 detach 已使引用
归零，则获取失败；若成功，引用在本次 put 前阻止其最终 drop。不能使用普通 inc
复活零引用，也不能改成获取新 psock 后继续处理旧队列。

入口 TX 检查可作为提前退出优化；在取得引用、进入处理临界区后还应检查 stop/dead，
特别是 worker 等待 socket 锁期间 close 已停止 psock 的情况。这个状态检查不替代
引用获取，也不取消现有队列在 ingress_lock 下的入队检查。

历史 [worker 与 close 的讨论](https://lists.openwall.net/linux-kernel/2025/05/22/1577)
说明了同步取消与最终 sock_put 的生存期配合；修改仍需保留该配合。

## 3. 记账修复：在异步 TCP ingress 的外边界持锁

### 3.1 推荐的锁范围

对已知目标为 TCP 的 ingress，以一个 skb 的处理为初始锁粒度，结构如下：

```text
取得 work 所属 psock 引用
mutex_lock(work_mutex)
  取得队首 skb，识别 ingress/egress
  TCP ingress:
    lock_sock(target)
      检查 stop/dead
      schedule、owner 转换、charge、构造并发布 msg
      成功：清理 work_state，dequeue，释放 worker 的 skb 引用
      EAGAIN：保存重试状态，保留队列 skb
      硬错误/拒收：凡本次发生的 uncharge 均在此锁范围内
    release_sock(target)
    重试等待期间不持有 socket 锁
  egress:
    按当前未持 socket 锁的发送入口执行
mutex_unlock(work_mutex)
释放取得的 psock 引用
```

这是一份控制流约束，实际代码需要统一成功、失败、重试各出口的解锁和引用处理。
保留现有 work_mutex 与 ingress_lock；初版不同时改动消息引用转移方式。

**必须把 dequeue 后的 `kfree_skb()` 包括进去。** 当前 ingress 为 msg 增加 skb
引用，用户 recv 也持同一个 socket 锁；worker 在解锁前释放自己的引用后，recv
才可消费消息。这样最终 sock_rfree 要么在 worker 的锁范围内发生，要么在之后
持锁的 recv 中发生。只锁 ingress helper 而保留锁外 kfree，不能建立这个保证。

### 3.2 锁的位置与锁顺序

- 公共 self ingress helper 保持调用者负责同步；同步 SK_PASS 可能已处于 socket
  接收临界区或 softirq，不能在公共 helper 内无条件调用 lock_sock。
- egress 继续使用当前 skb_send_sock 的未锁入口。不能在外层持锁再进入会自行
  lock_sock 的 TCP/BPF sendmsg，也不能为了恢复旧锁而绕开 SK_MSG verdict。
- 进程上下文用完整 socket 锁协议，不能仅使用 bh_lock_sock 取代它。
- 建议顺序为 work_mutex → socket 锁 → 必要的短队列/回调锁；不允许反向拿着
  ingress_lock/sk_callback_lock 再等待 socket 锁。SG 映射/分配不能置于长自旋临界区。
- close 与析构不能持 socket 锁执行 cancel_delayed_work_sync；释放 socket 锁会
  处理原生 TCP backlog，需验证其中回调不会反向等待 work_mutex。
- 先按每个 skb 获取/释放 socket 锁验证正确性；若需要减少加锁成本，再评估有限批次，
  同时测量 recv 延迟和 native backlog 排空成本。

共享 skmsg 还服务 UDP、UNIX、vsock。上述锁方案首先针对当前 TCP 路径；实现时
必须明确协议分支，或完成各协议的锁顺序及 skb 原 owner/destructor 审计后才推广。

## 4. 拆除清理必须属于同一组记账修复

### 4.1 析构中的顺序

[sk_psock_destroy()](../net/core/skmsg.c#L864) 对 TCP 的目标结构：

```text
停止并同步结束 parser work
cancel_delayed_work_sync(psock->work)
lock_sock(psock->sk)
  purge ingress_skb / ingress_msg
  释放本 psock 尚存的 cork 等需要 socket 记账的资源
release_sock(psock->sk)
释放其余资源及 socket 引用
```

理由是删除 map 后 socket 仍可能继续原生 TCP 收发，或者已挂载新 psock。
旧 psock 自身静止不能保证其 socket 的记账静止。

### 4.2 stop 不能无条件获取可睡眠锁

[sk_psock_stop()](../net/core/skmsg.c#L854) 当前在 ingress_lock 内执行 cork free。
它可从 map 删除的最后一次 unref 经 sk_psock_drop 调用，该路径可能处于 BPF、
RCU 和自旋锁上下文。因此不能直接在 stop 中补 lock_sock，也不能在那里同步等待 work。

建议把停止入队与资源释放明确分开：stop 保留原子状态切换；对不能睡眠的 detach
路径，将剩余 cork 的释放交给上述已同步、持 socket 锁的析构阶段。若 close 需要
立即释放 cork，应在已明确持 socket 锁的位置调用专用清理，而不依赖通用 stop
隐含这一动作。具体拆分需检查 active send 引用、close、destroy 和已 detach 情况，
保证只释放一次且额度最终归还。

因此，修改 worker 的一个 lock/unlock 对不应被描述成已经修复所有 psock 记账入口。

## 5. 序号修复独立完成

### 5.1 `ingress_bytes`

TCP ingress 加锁可以排除 worker 插入 parser 清零和结算之间；在此基础上，建议
进一步把该 counter 的更新从通用目标 enqueue 移回负责本次解析消费结算的同步路径。
该字段只应表示本次解析中需要留待应用消费的本源数据，不是目标队列的流量统计。

不能只删除通用 enqueue 中的 `+=`：同步 SK_PASS 仍需要正确扣除延迟消费的字节。
也不能仅把更新移到所有 SK_PASS 分支就结束：SK_PASS 排 work、分配失败、linearize
失败后的重试，必须与消费标记的决定配套处理。

### 5.2 `msg->sk` 与是否推进 copied_seq

明确区分“skb 当前由哪个 socket 记内存”与“这段源 TCP 数据是否仍需应用推进
消费序号”。owner 可以在重试前改变，不能作为唯一的消费语义来源。

推荐以“每段本源 TCP 字节只结算一次”为不变量，在源 verdict/parser 决定消费责任，
需要跨 work 保存时使用明确的元数据。具体字段布局在实现阶段确定。至少覆盖：

- 无 parser self-redirect：已经由 tcp_eat_skb 推进，不再由 recv 重复推进。
- parser 同步 SK_PASS：parser 延迟的消费，需由应用读取逐步推进。
- parser redirect 和异步 SK_PASS：排队时的结算决定与 recv 标记一致。
- 不同 socket、EAGAIN、部分读取、MSG_PEEK、FIN：重试或内存 owner 改变不改变消费责任。

不建议把 copied_seq 的行为调整与首次补锁混成无法独立定位回归的大改动。

## 6. 实现顺序和验证

建议按“引用身份 → worker 与拆除记账 → 消费语义”逐组实现和验证。
第二组可拆成几个窄补丁，但其覆盖完整性应按整组评估。

- 引用：旧 work 与 map 删除重加并行，检查 work 所属实例、获取实例和最终 put。
- 记账：持续 RX/recv，覆盖 msg 发布后的引用交错、EAGAIN 和删除 map 后继续收发。
- 锁：lockdep 检查 ingress、egress、SK_MSG redirect、close/cancel；核对 error 出口。
- 序号：有/无 parser、self/不同 socket、SK_PASS/redirect 的数据和 copied_seq 双重断言。
- 性能：验证正确后比较吞吐、recv 延迟和高接收压力下的 socket 锁持有时间。

本次只完成源码设计建议，没有执行这些运行测试，也没有声称示意代码已经可合入。
