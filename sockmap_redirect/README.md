# sockmap verdict 重定向到自身：收发流程分析

分析日期：2026-09-15。源码基线：
`45c13f3f9e3bb15fd89ff2864c6f627a3b4b4229`。
当前 Makefile 为 `VERSION=7, PATCHLEVEL=2, SUBLEVEL=0`；结论以提交和实际源码为准。

## 先看结论

设 `S` 是加入 sockmap 的已连接 TCP socket，`P` 是连接对端，
`map[0] = S`。“重定向给自己”指 helper 找到的目标 `struct sock *`
与运行 verdict 的 socket 相同，不是指同一个进程或同一台机器。

| verdict 入口 | helper 的 flags | 数据路径 | 谁读到数据 |
|---|---|---|---|
| 接收侧 `SK_SKB` | `BPF_F_INGRESS` | `P → S 的 TCP 接收 → verdict → S 的 psock 接收队列` | `recv(S)` |
| 接收侧 `SK_SKB` | `0` | `P → S 的 TCP 接收 → verdict → S 的发送路径 → P` | `recv(P)`，表现为回显 |
| 发送侧 `SK_MSG` | `BPF_F_INGRESS` | `send(S) → verdict → S 的 psock 接收队列` | `recv(S)`，该数据不进入 TCP 发包路径 |
| 发送侧 `SK_MSG` | `0` | `send(S) → verdict → S 的 TCP 发送路径 → P` | `recv(P)` |

表中接收侧 egress 一行假设没有额外的 `SK_MSG` 程序改变发送方向。
TCP socket 的发送方向始终指向它的连接对端；目标 socket 相同不会自动变成本地接收。
flags 的接口定义也可参见 [内核官方 sockmap 文档](https://docs.kernel.org/bpf/map_sockmap.html)。

**当前源码的关键问题：** 无 stream parser 的 `SK_SKB` self-ingress redirect
会在 `tcp_eat_skb()` 和应用读取时重复推进 `copied_seq`。
因此，“数据最后由 `recv(S)` 读到”不代表这条路径的 TCP 接收序号记账正确。
详细的可达时序见 [主分析第 8 节](analysis.md#8-当前源码的-self-ingress-序号问题)。
这是源码路径推导，本次没有进行内核运行复现。

## 归档内容

- [concurrency-models.md](concurrency-models.md)：本轮 CPU0 / CPU1 并发调用栈，涵盖 cross-socket、最终 skb 释放、cork、未 accept 子连接和旧锁方案的边界；修复状态以该文及新补丁说明为准。
- [patches-v2/README.md](patches-v2/README.md)：按引用、strparser 自投递/重试、TCP 记账同步合并后的三个提交、补丁文件和验证结果，以及尚未修复的 TCP ownership 缺口。
- [analysis.md](analysis.md)：挂载、完整收发调用链、parser 差异、队列、锁、内存和序号分析。
- [backlog-locking.md](backlog-locking.md)：work 调度点、socket 锁协议，以及 ingress work 无锁修改 `sk_forward_alloc` 的并发缺口。
- [tcp-backlog-vs-psock.md](tcp-backlog-vs-psock.md)：展开 `owned/slock` 协议、两种 skb 最终释放时序，以及普通 TCP backlog 如何避免同类缺口。
- [concurrency-audit.md](concurrency-audit.md)：进一步审计 parser 的 `ingress_bytes`、旧/新 psock 引用错配、拆除清理，以及为什么需要修同步边界而非仅将变量改为 atomic。
- [change-proposal.md](change-proposal.md)：修改位置、worker/析构锁范围、stop 与 cork 清理拆分、序号语义及建议验证顺序。
- [validation.md](validation.md)：最小配置示例、分支验证矩阵、跟踪点和已完成的检查。
- [source-index.md](source-index.md)：函数定义位置和源码导航。
- [source-manifest.sha256](source-manifest.sha256)：分析所用源码的校验值，路径相对仓库根目录。

阅读建议：先看主分析第 1、4、5、6 节理解数据去向，再看第 7、8 节理解接收记账。

补充结论：当前 ingress work 的队列锁不能保护 TCP 内存记账；`sk_forward_alloc`
是非原子读改写，work 与同一 socket 收包可并发更新。上述补充分析给出具体时序，
并说明最后一份 skb 引用也可能在 worker 中释放。这与 `copied_seq` 问题独立。

进一步审计发现：挂载 stream parser 时，异步 enqueue 可干扰 parser 本次
`ingress_bytes` 结算；删除再加入 map 时，旧 work 还存在获取新 psock 引用却释放旧
psock 的窗口。后者需要 map 生命周期变化，不能混入固定 map 的普通数据路径结论。
详细时序、未认定为故障的字段，以及持锁范围建议均见新增审计文档。

## 范围与证据

主线是普通、已连接、无 TLS ULP 的 TCP/IPv4 socket，IPv6 共用的 BPF 路径也作说明；
同时解释 `STREAM_VERDICT` 有/无 parser、`SK_SKB_VERDICT`、`SK_MSG_VERDICT`。
UDP 等协议的差异在主分析第 12 节列出。

本 README 的主体记录原始基线分析；后续已进行本地修复迭代，本轮已撤回旧方案并
重新拆分实现。已有 `sockmapsyz/`、`sockingress/` 下的研究内容不作为新补丁
通过运行验证的依据。各篇早期文档的“当前源码”应按其标注的基线理解。
宿主机运行 `6.6.87.2-microsoft-standard-WSL2`，不是所分析的源码版本。
