# sockmap 第二轮拆分补丁

## 状态

已先归档 [CPU0 / CPU1 并发模型](../concurrency-models.md)，再撤回旧的两提交方案，
重新实现；按功能关联收拢为以下三个修复提交。

**这不是全部并发问题的完整修复。** `inet_csk_listen_stop()` 对 owned child
继续执行 native TCP 销毁的问题尚未修复，见并发模型 F1/F2/F3。
补丁以 RFC 格式导出，未向社区发送。

| 顺序 | 提交 | 修复 |
|---|---|---|
| 1 | [78713dd14046](0001-bpf-sockmap-Pin-the-psock-owning-the-backlog-work.patch) | 固定 backlog work 所属 psock 的引用 |
| 2 | [fc379f91201c](0002-bpf-sockmap-Fix-strparser-self-delivery-accounting-a.patch) | strparser self helper 的 owner/额度处理，以及 EAGAIN 重试恢复完整 `_sk_redir` |
| 3 | [cb790fcb3216](0003-bpf-sockmap-Serialize-TCP-psock-memory-accounting.patch) | TCP ingress、最终 skb 释放、旧 psock 队列及 cork 回收统一使用 socket 锁 |

补丁 1 解决 psock 引用对象身份错配，独立于报文记账。
补丁 2 将 strparser 自投递及其重试元数据放在一起；补丁 3 将工作处理和
资源释放的 socket 同步作为一个整体，不再分别提交两段锁保护。

原五提交方案中的 2+3 合并成当前补丁 2，4+5 合并成当前补丁 3。
仅调整提交边界和说明；当前已跟踪文件树与合并前 `b127878ad23b` 完全相同。
旧的五份导出补丁已替换为三份，避免混用旧编号。

本轮源码相对原始基线只修改 `net/core/skmsg.c` 和 `net/core/sock_map.c`，
合计 43 行增加、12 行删除；没有新增 self-backlog helper、参数传递链或公共接口。

## 相比旧方案的关键变化

- 旧方案由 `61c044e00cf8` 显式 revert，保留历史，不 reset 用户的提交。
  对应补丁保存在 [reverts](../reverts/)。
- `sock_map_destroy()` 不再直接释放 TCP cork，避免依赖已被反例否定的
  “调用方必然排除 socket owner”假设。剩余 cork 交给持锁的 destroy work。
- `sock_map_close()` 仍在原有 socket 锁内即时释放 TCP cork，随后释放锁并
  cancel backlog；不能让已有 cork 在关闭等待期间被发送者继续使用。
- 非 TCP 的 stop 保留原有即时 cork 清理；新增 socket 锁只用于 TCP。
- 保留 work_mutex、ingress_lock；不添加与本次记账修复无关的 TX 状态复查。
- strparser 额度补丁只修改原有 self helper。分配/调度失败且尚无 owner 时，
  backlog 沿用普通 ingress 的 full-truesize 调度；已有 owner 时保留记账。
- `schedule(..., 0)` 只补足已有负额度，不保证下一次扣账后 F 始终非负。
- strparser 补丁中的元数据修复恢复 parser 标志；现有 `work_state` 本来就会保存重试的剩余
  长度和偏移，不能把标志丢失直接等同于已经证明的数据截断。
- `copied_seq` 语义修复、UDP/UNIX/vsock 并发审计、TCP 未 accept 销毁方案
  均不包含在这三个补丁中。

## 基线和应用

原始基线：`45c13f3f9e3bb15fd89ff2864c6f627a3b4b4229`。
本地 revert 后的已跟踪文件树与该基线完全相同，因此三个 RFC 补丁可以直接
按编号应用到原始基线，无须先应用旧方案和本地 revert。

从已有旧方案 `25c619569844` 开始，则先应用 reverts 下的撤回补丁，再应用
这里的三个补丁。当前工作分支已经完成这些操作，不需要再次应用。

回移补丁 3 时，需要 `8bbabb3fddcd`（把 cancel 移出 socket 锁）或等价顺序。
本轮未审计其他旧内核基线，不能直接宣称任意版本可安全回移。

## 本轮验证

- `make -j4 net/core/skmsg.o net/core/sock_map.o`：成功，两对象编译无警告。
  使用现有 x86_64 配置，包含 `CONFIG_BPF_STREAM_PARSER=y`、INET、IPv6。
  编译在提交合并前执行；合并后的源码完全相同，本轮未重复编译。
- `scripts/checkpatch.pl --strict --git 61c044e00cf8..HEAD`：合并后三个提交均为
  0 errors、0 warnings、0 checks。
- `git diff --check 61c044e00cf8..HEAD`：通过。
- 用独立临时 Git index 验证撤回补丁恢复原始树；三个导出补丁按序应用后，
  得到的树与最终 HEAD 完全相同，没有修改工作区 index 来执行该验证。
- 静态核对 backlog 成功、EAGAIN、硬错误出口；队列拒收后的 skb 引用；
  stop 的三个调用位置；cork 和 ingress purge；两个同步 cancel 的锁顺序。
- 没有创建或运行复现，没有 QEMU、压力测试、运行时 lockdep/KASAN/KCSAN 验证。
  旧提交中记录的测试不作为这组补丁的验证结果。

## 社区依据与接收方

额度策略依据 [Junseo Lim 的 v3](https://lists.openwall.net/linux-kernel/2026/08/17/1597)，
其简化反馈见 [Emil Tsalapatis 的回复](https://lkml.iu.edu/2609.0/03674.html)。
未照搬 v3 的 helper 和 self-pass 路由改造，也不声称与其全部行为等价。

本地 `scripts/get_maintainer.pl --nogit --nogit-fallback` 给出的 sockmap
维护者包括 John Fastabend、Jakub Sitnicki、Jiayuan Chen；相关列表为
`bpf@vger.kernel.org`、`netdev@vger.kernel.org`、`linux-kernel@vger.kernel.org`。
这些是准备供人工审阅的本地补丁，没有发信或推送。
