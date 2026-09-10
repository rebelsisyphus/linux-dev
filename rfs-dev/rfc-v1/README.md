# RFS BPF RFC v1 讨论稿

邮件主题：`[RFC net-next 0/4] net: rfs: Add programmable CPU selection with BPF`。

- [完整 cover letter 邮件](0000-cover-letter.patch)：包含 From、To/Cc、
  Subject、Message-ID、正文、四个提交的 shortlog、diffstat 和 base-commit。
- [可单独编辑的正文](cover-letter.txt)：英文纯文本，按邮件列宽排版。
- [中文架构说明及 Mermaid](../native-bpf-architecture.md)、
  [SVG](../native-bpf-architecture.svg)、[PNG](../native-bpf-architecture.png)。
- `0001`–`0004` 是现有四个提交的 RFC 邮件导出，包含已有提交的签名标记。

## 这份 RFC 希望讨论什么

正文先说明“允许接收 CPU 策略独立于内核迭代”的动机，再解释原生 RFS
为何仍然负责迁移边界、active CPU 和 backlog。最后给出实际验证范围，
向维护者提出四个具体问题：

1. `get_rps_cpu()` 中的期望 CPU 决策是否是合适的扩展位置。
2. 每设备 struct_ops/link 是否合适，是否需要每 RX 队列的独立绑定。
3. 设备注销后保留失效 link 的语义，以及是否需要额外的绑定状态查询。
4. 进入非 RFC 阶段前应完成哪些性能、迁移和乱序压力验证。

正文记录了 22/22 自测通过、KASAN/lockdep/RCU 验证，以及关闭调试后的 Redis VM 对比。
性能数据来自实际三轮测试；宿主机只有一个物理 NUMA 节点，真实 NIC、多队列、
实际 SMT、重组前乱序和物理 NUMA 收益仍待验证。详见 [性能报告](../test-results/redis-numa/report.md)。

当前代码来自主线快照 `45c13f3f9e3bb15fd89ff2864c6f627a3b4b4229`，
四个提交终点是 `9f3cd6a8fd249686dd175723ee7412dfb81160c6`。
`RFC net-next` 是这份讨论稿的目标方向，base-commit 保留实际测试基线。
邮件收件人按当前源码树的网络、BPF、自测和文档维护者信息整理；文件目前是
本地草稿。本次性能测试已更新前两个提交的说明并重新导出系列；四个提交的源码树均未改变。

## 参考的社区写法

- [Networking subsystem 文档](https://docs.kernel.org/process/maintainer-netdev.html)：
  RFC 用于讨论，注明目标树；用户态示例和自测可以与内核功能放在同一系列。
- [BPF 提交流程](https://docs.kernel.org/bpf/bpf_devel_QA.html)：
  多补丁系列需要高层概述，跨子系统变更同时覆盖相关列表；`net-next` 和
  `bpf-next` 的最终接收路径由维护者协调。
- [Amery Hung 的 BPF qdisc RFC cover letter](https://lwn.net/Articles/973292/)：
  参考其按设计概述、关键接口、验证情况和待讨论事项组织内容的方式。
- [Matthew Cover 的 XPS BPF hook RFC](https://lists.openwall.net/linux-kernel/2019/09/19/1438)：
  参考其明确区分已实现功能和仍需完成事项的表达方式。

技术内容依据这四个本地提交与实际测试日志；社区邮件用于组织方式参考。
英文邮件正文保留 ASCII 示意图，SVG/PNG 和 Mermaid 供本地讨论、展示使用。

## 导出与核对

补丁使用以下基线范围导出，并以当前 `cover-letter.txt` 填充 cover letter：

```sh
git format-patch --cover-letter --thread=shallow \
  --subject-prefix='RFC net-next' \
  --base=45c13f3f9e3bb15fd89ff2864c6f627a3b4b4229 \
  --output-directory=/tmp/rfs-rfc-export \
  45c13f3f9e3bb15fd89ff2864c6f627a3b4b4229..9f3cd6a8fd249686dd175723ee7412dfb81160c6
```

导出命令会产生新的 cover letter 占位符和线程头；本目录中的邮件已经填好。
若修改正文，需要同步更新 `0000-cover-letter.patch` 中的正文。
邮件和原提交的 patch-id、线程关系及基线核对结果见
[validation.txt](validation.txt)。
