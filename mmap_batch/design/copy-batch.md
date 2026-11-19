# v7：按批延迟文件 VMA 页表复制

2026-09-22。实现提交 aeb884f315c6d69a5ba9995bafb9ce8604357efc，
HEAD 668fa34e8896ac58334ef040ac81ec0f9f32a540。
v6 保存在 refs/mmap_batch/revisions/v6。

## 目标与范围

v6 在当前文件 VMA 需要复制时立即 final。实测 spawn 每文件 5 个 VMA
形成 4 + 1 两批。v7 将复制推迟到整批文件 VMA 已插入 rmap 后，允许
所有成员都需要复制的批次继续积攒。

每批调用一次本地 copy_page_range_batch()；内部仍逐 VMA 调用现有
copy_page_range()。没有合并 MMU notifier、write_protect_seq、页表锁
或不同 VMA 的复制语义，也不将不同 VMA 假装成一个连续映射。

## 正常路径

1. 继续按原顺序完成 VMA 分配、策略/UFFD/anon_vma 初始化、子树安装、
   vm_ops->open 和 get_file，然后将父子 VMA 对加入队列。
2. 下一项的 mapping 改变或已有 8 项时，先处理旧批。一次获取 i_mmap
   写锁及架构 dcache mmap 锁，将所有子 VMA 按 insert_after 链接到
   各自父 VMA 之后，完成 shared-writable 计数，然后释放两种锁。
3. link_file_vma_batch_process 清零 count，但配对数组内容仍然有效。
   dup_file_vma_batch 保存原 count，调用 copy_page_range_batch，按原
   WIPEONFORK 规则跳过相应项，其余交给 copy_page_range。
4. 文件 VMA 入队之后不再即时复制或按 vma_needs_copy 刷新。
   没有 vm_file 的 VMA 保持原位置即时复制，不打断尚未处理的文件批次。
5. 成功遍历完毕后，先处理尾批，再调用 arch_dup_mmap。因为本版延迟
   了实际复制，架构后续初始化仍应看到完整复制结果。

vma_needs_copy 已回到 memory.c 的原有位置及 static bool 定义；
dup_mmap 不再需要共享这个谓词，内部的复制选择逻辑保持不变。

## 数据结构及接口

继续复用 vma_file_batch 的 count 和容量 8 的数组，插入专用
link_vma_file_batch 增加对应的 src_vmas。x86_64 大小仍为 136 字节，
unlink 的大小、分组条件、函数行为保持不变。

- link_file_vma_batch_process：只链接非空批次，完成后清零 count。
- copy_page_range_batch：复制已链接批次，返回第一个复制错误。
- dup_file_vma_batch：组合上述两阶段，空批直接成功。
- link_file_vma_batch_add：按需处理旧批，然后入队当前项，返回旧批
  的复制结果。即使旧批失败，当前项也必须入队，原因如下。
- link_file_vma_batch_final：只链接剩余项，供 loop_out 清理使用；
  正常路径以及尾批复制失败时队列已为空。

## 失败边界

复制进度不再代表 VMA 创建进度。保持 mpnt/vma_next 表示首个尚未安装
到子树的父 VMA，回收范围覆盖所有已创建子 VMA，包括未复制页表的项。

| 失败位置 | 批次处理 | 回收边界 |
| --- | --- | --- |
| 当前 VMA 安装前，分配/初始化等失败 | 剩余旧批只链接，不再复制 | 当前 mpnt |
| add 刷新旧批时复制失败 | 旧批已全部链接；当前新项仍入队，loop_out 将其链接 | vma_next，包含当前已安装项 |
| 非文件 VMA 即时复制失败 | 队列中的文件 VMA 只链接，不再复制 | vma_next，包含该非文件 VMA |
| 尾批复制失败 | 已全部链接，count 已清零 | mpnt 为 NULL，覆盖整个子树 |
| arch_dup_mmap 失败 | 所有批次均已完成 | 覆盖整个子树 |

当前新 VMA 在 add 之前就已装入 maple tree、执行 open 并取得文件引用。
因此 add 不能因旧批错误直接返回而遗漏它；即便返回错误也必须先把
当前项加入已清空的队列。loop_out 只补齐链接，然后交给现有 teardown
释放文件引用、共享可写计数、anon_vma、策略、页表和内存记账。

批次在复制之前清零，保证复制部分成功后出错不会再次插入同一批。
copy_page_range 自身继续闭合 notifier/write_protect_seq，dup_mmap
保留原有父 mm TLB 刷新与 UFFD 完成/失败通知。

## 不变量与验证

- 批内任意文件 VMA 获得复制 PTE 前，整批已在对应 rmap 树中。
- 父、子 mmap 写锁覆盖队列生存期；所有父 VMA 已取得 vma 写锁。
- i_mmap/dcache 临界区仅完成树插入和计数，不包含页表复制及分配。
- loop_out 回收前，全部已创建文件 VMA 都已链接且各链接一次。
- 正常尾批复制先于 arch、KSM/khugepaged 注册和释放 mmap 写锁。

验证包括扩展至 14 项的 fork_file 自测、guard、SysV、原有内存记账
失败用例，以及针对 copy_page_range 内页分配的故障注入。
故障注入逐一检查整批中未复制后缀、切换文件时当前已创建项、尾批和
匿名复制失败时的待处理文件批次，随后验证恢复 fork、封印和 truncate。
最终结果及实测 spawn 批次在 dev/review-v7/ 和 dev/validation.md。

设备回调和其他架构仍有运行时覆盖限制，UFFD/hugetlb 未在本轮运行。
不把源码不变量分析或本轮自测等同于所有环境均验证通过。
