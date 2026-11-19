# 同时保留多个文件 VMA 批次

## 来源与移植范围

参考 /home/sisyphus/code/fork/kernel/mmap_batch/
0003-mm-keep-multiple-file-VMA-batches-in-dup_mmap.patch，原提交
 e11d3ddf3b408f53495a5be1be1cdeef7e4acc07，作者 Dong Chenchen。

当前实现基于 v6 的两个提交，在其后追加 9ed28899f15d 作为第三个补丁。
v7 的延迟页表复制方案继续挂起。该功能只调整文件 VMA 的待插入队列。

## 优化的作用

v6 仅保存一个 mapping 的批次，遇到不同 mapping 就刷新。因此 A/B/A/B
交错布局即使完全无需复制页表，也退化成逐项获取 i_mmap_rwsem。

现在同时保存 4 个 mapping，每个批次容量仍为 8。查找时优先匹配已有
mapping，再使用空槽；均不可用时刷新并复用第一个槽。按 v6 风格，
满 8 项的批次在接收下一项之前处理；最终由 final 补齐所有非空槽。

例如 64 段 A/B 交错映射，A、B 各 32 段，各组成 4 个满批，共 8 批。
相对 v6 每项一批的 64 次加锁，插入侧只需 8 次，减少 87.5%。
这里比较的是插入锁次数，rmap 树插入仍执行 64 次。

容量只有 4 个 mapping，不保证任意交错序列都能完全合批。5 文件轮转
时，第一个槽会在其中两个 mapping 间反复替换，另三个槽仍能积累。
64/65 段的实际诊断结果见 dev/review-v8/report.md。

## 与原始第三个补丁的差异

- 适配当前 mm/mmap.c 的 dup_mmap，沿用 v6 的 link_vma_file_batch，
  内嵌公共 vma_file_batch，不改 unlink 的结构布局和策略。
- 保留 mapping_rmap_tree_insert_after 和 vma_is_shared_maywrite，
  不引入旧内核的 interval-tree API 或 VM_SHARED 判定。
- 不增加 CONFIG_I_MMAP_SHARDS；当前树中的 fork/MMU 路径直接使用该逻辑。
- mapping 从首项 vm_file 推导，无需增加显式缓存字段。
- 优先匹配/空槽查找合在一次最多 4 项的扫描中，复用第一个槽的策略与
  参考补丁一致。满批处理时机沿用 v6 的“加入下一项前处理”。
- 仍仅在 file && vma_needs_copy 时，于 copy_page_range 前 final；
  loop_out final 处理所有剩余项。arch_dup_mmap 的原位置不变。

## 正确性条件

父、子 mmap 写锁仍覆盖整个复制过程，父 VMA 的 vma_start_write 已完成，
每个已入队子 VMA 都有自己的文件引用。每槽只接受同一 address_space；
同一 mapping 的入队顺序保持原 VMA 遍历顺序，src/dst 配对没有重排。

每次 process 只获取一个 mapping 的 i_mmap/dcache 锁，处理后释放，
再清空该槽 count。final 依次处理各槽，不嵌套不同 mapping 的锁。
其他槽的 count、VMA 指针不会被单槽处理改写。

需要复制的文件 VMA 在页表复制之前入队，再刷新所有待处理槽；无需复制
的调用仍可先返回。无 vm_file 的 VMA 复制不触发文件队列刷新。所有页表
复制仍同步发生，不引入 v7 的延迟复制和创建/复制进度分离。

初始化只在 __mt_dup 成功后执行；之前的失败跳转到 out，不访问队列。
之后的 VMA 创建或复制失败仍走 loop_out，先链接已安装的全部待处理项，
再按原 mpnt 边界 unmap_region/tear_down_vmas。批次处理不新增失败返回。

## 成本与适用边界

x86_64 单槽 136 字节，4 槽缓冲为 544 字节，增加 408 字节；没有新内存
分配。当前 KASAN 构建 dup_mmap 的栈预留为 0x4c0 字节，另有 6 个保存
寄存器，不把这个调试构建数字外推到其他配置或架构。

每个文件 VMA 增加最多 4 项的匹配扫描；连续布局的合批数量不变。
x86 的 dcache mmap lock/unlock 为空函数，实际减少的是 i_mmap_rwsem
加解锁。未测量实机吞吐，也未确认额外扫描成本对具体负载的影响。

当前 spawn 的 5 段文件布局仍被复制前 final 分成 4 + 1，静态版每 fork
2 批，动态版 6 批；本轮 192 次 fork 跟踪确认没有改变。该功能没有绕过
复制前的安全边界，因此不能据其合入推断用户 spawn 测试会出现收益。
