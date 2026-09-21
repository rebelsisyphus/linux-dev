# 与 2026-06-16 RFC 的比较

## 结论

两个方案都将同一文件映射的 VMA 插入合并到一次 i_mmap 写锁临界区。
Pedro Falcato 指出的竞态存在于 RFC 的插入时序：它可能先复制页表，
再把子 VMA 加入文件 rmap 树。当前实现保留“复制页表前完成插树”的
顺序，没有发现同一类延迟登记窗口。

这一结论针对当前源码中的页表复制和批处理边界，不代表所有架构、
驱动回调及配置均已运行验证。CONFIG_USERFAULTFD 当前未开启。

## 讨论来源

使用 lei 从 lore 的 all 索引按 Message-ID 获取线程，并按相同主题
检索版本。检索返回原始 RFC 和 Pedro 的回复两封邮件，没有检索到
该主题的后续版本。原始 mbox、逐封正文和查询结果在 dev/review-v3/。

- [Yibin Liu 的 RFC](https://lore.kernel.org/all/20260616091302.2725675-1-liuyibin@hygon.cn/)
- [Pedro Falcato 的回复](https://lore.kernel.org/all/ajEfJyYRbhVo4dFK@pedro-suse.lan/)

Pedro 描述的是以下时序：先创建子 VMA、复制其页表，此时另一线程
按文件的 rmap 树遍历映射，而子 VMA 尚未插入该树。最后才执行的
批量插入无法弥补这次漏查。rmap 并不依赖新任务已开始运行。

## 实现差异

| 项目 | RFC | 当前实现 |
| --- | --- | --- |
| 何时允许延迟 | 文件 VMA 普遍入批，包括随后要复制页表的 VMA | 复制页表前强制处理批次，仅无须复制页表的 VMA 可继续延迟 |
| 分组条件 | struct file 指针相等 | address_space 相等，可合并同 inode 的不同打开实例 |
| 回调和收尾位置 | 未单独处理 open；在 arch_dup_mmap 之后执行尾批 | 同样保留 open、arch_dup_mmap 的基线位置，尾批在 loop_out 处理 |
| 出错回收 | loop_out 处理尾批 | 同样在 loop_out、回收 VMA 前处理尾批 |
| 数据结构 | 单独维护 new_vmas/old_vmas 两个数组 | 嵌入共用 vma_file_batch，另存 src_vmas，容量共用且仍为 8 |
| dcache 锁 | 每个插入项各加锁一次 | 每批加锁一次 |
| 接口范围 | 在 vma.h 声明全局函数 | 插入辅助函数限定在 mmap.c 内部 |

## 对当前实现的核对

1. `copy_mm()` 在不共享 mm 时调用 `dup_mm()`，随后才把成功创建的 mm
   赋给新任务。`__mt_dup()` 只复制 VMA 索引，不复制用户页表。
2. `vma_needs_copy(dst_vma, src_vma)` 与 `copy_page_range()` 使用同一个
   函数：检查目标的 VM_COPY_ON_FORK，以及源的 anon_vma。页表复制函数
   对“不需要复制”的情况直接返回，没有被绕过的其他操作。
   v6 在 dup_mmap 中仅以 `file && vma_needs_copy(tmp, mpnt)` 控制刷新；
   copy_page_range 的外层调用仍只排除 WIPEONFORK，其内部判断保持原样。
3. 在文件 VMA 需要复制的路径中，`dup_mmap()` 先调用
   `link_file_vma_batch_final()`，再调用 `copy_page_range()`。
   `mapping_rmap_tree_insert_after()` 和可写计数更新已在 i_mmap 写锁
   下完成，因此不存在“已复制文件 PTE、尚未登记子 VMA”的批处理窗口。
4. `__rmap_walk_file()` 获取 i_mmap 读锁并遍历 mapping 的树。
   父子 mmap 写锁负责 VMA 的生命周期；插树与页表复制的先后顺序负责
   本次审查关注的 rmap 可见性。
5. 真正延迟的子 VMA 没有从父进程复制的文件页表。v5 删除 has_open
   对刷新时机的额外限制，仍先执行原有 open 回调，再取得文件引用、
   入队。需要复制文件页表时刷新，单纯存在 open 回调不再刷新。
   无 vm_file 的 VMA 复制也不再打断文件批次。
6. 初始锁失败和 __mt_dup 失败时尚无入队项；循环中的分配、策略、
   userfaultfd、anon_vma 等失败都到 loop_out 处理已有批次。
   文件 VMA 页表复制失败之前，当前 VMA 已完成插入；无文件的 VMA
   复制失败时，之前的文件批次可继续待处理，到 loop_out 再完成插入。
   后续解除映射和可写计数释放仍遵循原有回收路径。
   v5 恢复 arch_dup_mmap 在循环结束后、loop_out 之前的原位置。当前
   generic/arm64/powerpc/x86（包括 Xen）架构钩子不依赖文件 rmap 树。
   正常、循环错误和架构错误均经过尾批处理，再完成注册或回收。
7. 队列只在 count 非零时读取首项，满 8 项时先处理再继续入队。
   父 VMA 留在原树中且受旧 mm 的写锁保护，子 VMA 入队前已持有文件
   引用。按 address_space 合批不改变 insert_after 所需的父子对应关系。

对应源码：mm/mmap.c 的批处理辅助函数与 dup_mmap；mm/internal.h 的
vma_needs_copy；mm/memory.c 的 copy_page_range/free_pgtables；
mm/interval_tree.c 的 mapping_rmap_tree_insert_after；mm/rmap.c 的
__rmap_walk_file；kernel/fork.c 的 dup_mm/copy_mm。

## 验证方法与限制

已有 truncate 和 write-seal 自测被增强为每个文件 VMA 映射两页。
private_cow 场景对第一页写入，第二页预先读入，从而覆盖同一 VMA
内同时复制 COW 数据和文件 PTE 的情况。

项目诊断通过 kprobe/kretprobe 跟踪 dup_mmap、rmap 插入的进入/返回、
copy_page_range。按进程和每次 fork 分组，根据目标 vm_flags 和源 anon_vma
识别需要复制的调用，检查这些文件 VMA 的插入返回先于复制调用，并检查
trace 没有覆盖丢失。v6 恢复调用函数后由其内部返回的路径，此类调用
无需提前插入，诊断单独计数，不能仅凭 copy_page_range 入口判定发生复制。
这验证已运行场景的顺序，不能替代源码层面的并发论证或覆盖所有配置。
运行结果及日志路径见 dev/validation.md。v3 原始验证保存在
dev/review-v3/，后续简化验证保存在 dev/review-v4/ 至 dev/review-v6/。
v5 另用 SysV SHM 诊断检查带 open 回调的 VMA 合批及映射计数生命周期；
该场景不能替代对所有驱动回调的验证。

## 性能结论的区别

另外检查了 RFC 引用的 [doexec.c 源码](https://apollo.backplane.com/DFlyMisc/doexec.c)。
该程序在创建工作进程时执行 fork，稳定运行阶段由工作进程反复
execl 自身。这适合测量 exec 释放旧映射时的 unlink 优化，但不能
直接把稳定阶段的吞吐差异归因于 dup_mmap 插入优化。这个判断来自
基准源码，未在本机重跑 RFC 的硬件性能实验。

本项目的 fork-bench 每轮确实执行 fork/wait，能触及目标路径。
不过现有 TCG、KASAN 和 lockdep 环境下的耗时仍不足以证明生产收益，
补丁不沿用 RFC 的性能收益数字。
