# 验证方案与本次验证记录

## 1. 本次实际完成的检查

- 检查提交、Makefile 版本和相关配置；源码基线为
  `45c13f3f9e3bb15fd89ff2864c6f627a3b4b4229`。
- 阅读实际 helper、socket hook、TCP 收发、strparser、psock work、SG 接收和释放代码。
- 对无 parser 的 `SK_SKB` self-ingress 重复推进 `copied_seq`，逐步核对入口、
  owner 设置、内部 verdict、异步入队和最终 recv；结论属于静态源码分析。
- 检查已有 selftests 的实际拓扑，确认不能把不同 socket 的转发测试视为 self-redirect 覆盖。
- 生成源码索引和 SHA-256 manifest，并检查归档链接、行号和文件一致性。

首轮归档检查结果：4 份 Markdown 的 137 个本地链接均有效，代码围栏配对、无行尾空白；
源码索引定位到 21 个文件中的 122 个定义；manifest 中 25 份文件校验全部通过。
`git diff --no-index --check` 检查归档 Markdown 未报告空白错误，相关内核源码无工作区修改。

后续针对 backlog 锁的补充，已核对四处 work 调度点、`sk_lock.owned` 协议、
`sk_forward_alloc` 的非原子更新及最终 skb destructor 的执行上下文，见
[backlog-locking.md](backlog-locking.md)。该部分同样属于静态分析，未运行 KCSAN。

继续核对了 `__release_sock()` 保留 owned 的批处理、普通 backlog 的独立长度记账、
psock 的两种最终引用释放顺序，以及原生 `tcp_eat_recv_skb()` 在延迟释放前撤销
socket 记账的实现，见 [TCP backlog 对照](tcp-backlog-vs-psock.md)。未新增运行测试结果。

**没有完成的运行验证：** 本次没有编译/启动该提交内核，没有运行下面的 selftests
或最小场景，也没有采集动态 TCP 序号、抓包或性能结果。仅新增文档，无内核实现变更。
宿主机是 `6.6.87.2-microsoft-standard-WSL2`，其 BPF 行为不能证明本分析版本的行为。

## 2. 当前源码树相关配置

本次读取的 `.config` 中：

```text
CONFIG_BPF=y
CONFIG_BPF_SYSCALL=y
CONFIG_BPF_JIT=y
CONFIG_BPF_STREAM_PARSER=y
CONFIG_NET_SOCK_MSG=y
CONFIG_INET=y
CONFIG_IPV6=y
```

`.config` 说明源码树中的构建选择，不证明现有 `bzImage` 或当前运行内核由这些配置构建。
真实复现应在明确从该提交构建的测试内核/虚拟机上完成。

## 3. 最小拓扑和 BPF 程序示意

仅建立一条 TCP 连接：客户端 P 与 accept 得到的 S。
只把 **S** 插入 key 0，P 保持普通 TCP socket。不要把监听 socket 当作 S。

```c
/* BPF 侧公共 map；示意代码，需要通常的 linux/bpf.h 和 bpf_helpers.h。 */
struct {
	__uint(type, BPF_MAP_TYPE_SOCKMAP);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, __u64);
} sock_map SEC(".maps");

/* 场景 A/B：无 stream parser 的接收侧 verdict。二选一方向。 */
SEC("sk_skb/stream_verdict")
int rx_self(struct __sk_buff *skb)
{
	return bpf_sk_redirect_map(skb, &sock_map, 0, BPF_F_INGRESS);
	/* 测 egress 时，把最后一个参数改成 0。 */
}

/* 场景 C/D：发送侧 verdict，附到 BPF_SK_MSG_VERDICT。 */
SEC("sk_msg")
int tx_self(struct sk_msg_md *msg)
{
	return bpf_msg_redirect_map(msg, &sock_map, 0, BPF_F_INGRESS);
}

/* parser 对照组可另加；此例以当前可见长度为消息长度。 */
SEC("sk_skb/stream_parser")
int parser(struct __sk_buff *skb)
{
	return skb->len;
}

char LICENSE[] SEC("license") = "GPL";
```

这些是配置片段，不是本次已构建或运行的复现程序。
载入多个程序不等于全部 attach；各个场景要显式选择所需 attach type。
若要验证稳定的消息边界，应让 parser 根据自定义头里的长度解析，
不能假定 `return skb->len` 等于一次 `send()` 的长度。

用户态设置顺序示意：

```c
/* create/load map and program */
bpf_prog_attach(rx_prog_fd, map_fd, BPF_SK_SKB_STREAM_VERDICT, 0);
/* optional: attach parser before inserting S */
/* establish TCP connection P <-> S, S is the accepted connected socket */
__u32 key = 0;
__u64 value = (__u64)s_fd;
bpf_map_update_elem(map_fd, &key, &value, BPF_ANY);
```

实际程序必须检查创建、load、attach、update、send、recv 的返回值，并处理短写/短读。
使用 poll 或有界超时，避免错误配置造成测试进程无限等待。

## 4. 验证矩阵

除特别注明，均是新建连接、空队列、无 FIN、无 payload 修改、只挂一个 verdict。

| 场景 | 动作 | 数据应到达 | 关键检查 |
|---|---|---|---|
| A：无 parser，SK_SKB self ingress | `send(P, N)`，`recv(S)` | S | 当前代码预期出现 `copied_seq` 重复推进，不能只检查内容 |
| B：无 parser，SK_SKB self egress | `send(P, N)`，`recv(P)` | P | S 不调用用户态 recv 也应完成回显；观察 work→发送入口 |
| C：仅 SK_MSG self ingress | `send(S, N)`，`recv(S)` | S | 本次 payload 不出现在 TCP 线上；TCP 收发序号不因该本地交付增加 |
| D：仅 SK_MSG self egress | `send(S, N)`，`recv(P)` | P | 目标直接到 tcp_sendmsg_locked，不再次进入 SK_MSG |
| E：parser + STREAM_VERDICT self ingress | P 发，S 读 | S | 记录 parser offset/full_len、skb owner 与 msg self 标记 |
| F：SK_SKB self egress + SK_MSG self ingress | P 发，S 读 | S | work 经 sock_sendmsg 触发 SK_MSG，并改变最终方向 |
| G：直接 SK_PASS 对照 | P 发，S 读 | S | 无 redirect 元数据，正常直接入队路径不先 tcp_eat_skb |
| H：无效 key / listener 目标 / 不支持的 flags | 触发相应 verdict | helper 返回 SK_DROP | helper 失败与异步 work 错误分开统计 |

扩展验证按需要增加：

- N 字节先读 k 字节再读剩余数据；前后各做一次 `MSG_PEEK`。
- 一次大写、多个小写、多个 skb/SG、parser 跨 skb 消息。
- 缩小接收/发送缓冲，验证 `-EAGAIN`、短写和 work 的 offset/len 续传。
- 分别在 ingress_skb 尚有数据、ingress_msg 尚有数据、SK_MSG cork 未完成时关闭 S。
- 接收队列原生部分读取后再加入 map；这是另外的序号/offset 边界，不是场景 A 的必要条件。
- IPv6 和 sockhash 对照。

## 5. 场景 A 的关键断言

设本次收包前读取 `C = tcp_sk(S)->copied_seq`，`rcv_nxt = C`：

1. P 发送 N > 0 字节；期间不发 FIN 或其他数据。
2. 等待 S 的 `msg_tot_len == N`，确认 redirect work 已入应用队列。
3. 此时当前代码已由 `tcp_eat_skb` 推进到 `copied_seq=C+N`。
4. S 读取 k 字节，0 < k ≤ N。
5. 跟踪当前代码应显示 `copied_seq=C+N+k`、`rcv_nxt=C+N`；正常消费关系不应如此。
6. 读完之后 `msg_tot_len=0`，但 `copied_seq=C+2N`，说明仅检查 FIONREAD 和内容会漏检。
7. 继续验证下一次接收，并在移除 socket 的 map 关联后验证原生 TCP 收发。

TCP 序号是 32 位循环空间，动态比较应使用 TCP 的序号比较规则；以上代数示例假定
所选 C、N 不跨回绕点。获取内部序号需在匹配内核上使用 BTF/fentry、kprobe、
调试器或临时测试插桩；普通 FIONREAD/TCP_INFO 不能替代所有这些内部字段。

## 6. 建议的跟踪点和字段

| 跟踪点 | 应记录的信息 |
|---|---|
| `tcp_read_skb` / `sk_psock_verdict_recv` | S 指针、skb 指针、skb len、TCP seq/end_seq、skb owner |
| BPF verdict | 当前 socket 身份、目标 key/socket、flags、helper 返回值和执行计数 |
| `tcp_eat_skb` | 前后 copied_seq、rcv_nxt、是否带 STRPARSER 标记 |
| `sk_psock_skb_redirect` | 源/目标 psock、TX_ENABLED、入队 skb |
| `sk_psock_backlog` / `sk_psock_handle_skb` | ingress、off、len、work_state、返回值 |
| `sk_psock_skb_ingress_self` | 是否命中 self owner 分支、take_ref |
| `sk_psock_skb_ingress_enqueue` | msg->sk、msg->skb、SG size、msg_tot_len |
| `tcp_bpf_send_verdict` / `tcp_bpf_sendmsg_redir` | 方向、apply/cork、源/目标、实际转移字节 |
| `tcp_bpf_push` / `tcp_sendmsg_locked` | 是否真的进入 TCP 发送，write_seq/snd_nxt |
| `__sk_msg_recvmsg` / `tcp_bpf_recvmsg_parser` | copied、copied_from_self、MSG_PEEK、最终 copied_seq |
| `sock_rfree` / `sock_efree` | owner 转换、sk_rmem_alloc 和引用释放 |

部分函数是 static/inline，实际能否 fentry/kprobe 取决于该构建的 BTF、优化和符号。
先检查实际内核符号，再选择其非内联上下游入口，不应假定所有列出的函数都能直接挂探针。

抓包用于区分本地注入与 TCP 回显：B/D 应有发往 P 的 TCP payload；C 的该次 payload
不应出现在网络接口上。捕获 loopback 流量时按序号和方向理解记录，不用抓包条目数
直接代替 BPF 执行次数。抓包本身不能证明 `copied_seq` 正确。

## 7. 已有 selftests 能覆盖什么

相关代码：
[sockmap_basic.c](../tools/testing/selftests/bpf/prog_tests/sockmap_basic.c)、
[test_sockmap_pass_prog.c](../tools/testing/selftests/bpf/progs/test_sockmap_pass_prog.c)。

- `sockmap recover` / `sockmap recover with strp` 对应
  `test_sockmap_copied_seq(false/true)`。其中测试发送拓扑是 `c0 → p0 → p1`，
  redirect 目标 key 1 对应 p1；这是跨 socket redirect，并非 p0 redirect 给 p0。
- `sockmap same socket replace` 检查同一 socket 的 map 替换操作，不是同一 socket
  作为 verdict redirect 源和目标时的完整收发/序号验证。
- FIONREAD、PEEK、multi channels 测试可验证相关接收行为，但通过这些测试仍不能
  自动证明第 5 节的 self-ingress 序号断言成立。

在匹配测试内核启动后，可先运行已有定向测试：

```sh
cd tools/testing/selftests/bpf
sudo ./test_progs -t sockmap_basic
```

上面是后续运行命令，**本次未执行**。新增复现程序时应单独覆盖场景 A，
不要只用已有跨 socket 测试的通过结果替代。

## 8. 归档一致性复核

在仓库根目录校验分析用到的源码快照：

```sh
sha256sum -c sockmap_redirect/source-manifest.sha256
```

若后续切换提交或修改相关实现，校验值或行号会变化，应重新核对路径结论。

2026-09-15 共享状态审计后复核：7 份 Markdown、240 个本地链接的目标/行号范围/
章节锚点、30 份源码校验值均通过；没有行尾空白。相关 `net/`、`include/`、
`kernel/`、`lib/` 下没有内核实现改动，也没有暂存提交内容。
这是归档一致性检查，不是运行时并发验证；新增审计中的 parser 和引用错配时序仍属
静态源码推导。
