# mmap_batch: fork 文件 VMA 插入批处理

## 输入与基线

用户提供 `patch/mm_batch_file_vma_insert_v1.patch`，原作者 Dong Chenchen，
目标是在 `dup_mmap()` 中批量插入同一 address_space 的文件 VMA。
本项目基于 `9b87fdc9af2fbfcdb5c24a64139685ef80f6573f`，分支
`develop/mmap_batch`，源码在 `/home/sisyphus/code/worktree/mmap_batch`。
原补丁不能直接应用：本基线的实现已从 kernel/fork.c 移至 mm/mmap.c。

## 实现

- 保持容量 8，按 `file->f_mapping` 分组，因此同一 inode 的不同打开
  文件对象也可合批；不合并 VMA，不改变用户接口。
- 队列存储子 VMA 与对应父 VMA。父、子 mmap 写锁保护其生存期，
  子 VMA 在入队之前已取得独立文件引用。
- 参考 `3577dbb19241`（批量 unlink_file_vma）的命名和接口：将
  `unlink_vma_file_batch` 更名为 `vma_file_batch`，保留 count 和 VMA
  数组布局；共用 `VMA_FILE_BATCH_SIZE`。插入专用的
  `link_vma_file_batch` 嵌入该结构，并增加对应父 VMA 的 src_vmas 数组。
  这保留 insert_after 的顺序，不增加 unlink 批次的存储开销。
- 插入辅助函数采用 `link_file_vma_batch_init/add/process/final`。
  process 从首个 VMA 获取 mapping，处理后调用 init；final 处理非空
  批次。add 与已有 unlink 接口一致，在接收下一项前处理满批。
  插入仍按 address_space 合批；unlink 的分组和执行逻辑保持原样。
- 单批获取一次 i_mmap 写锁和架构 dcache mmap 锁；沿用当前
  `vma_is_shared_maywrite()` 计数规则及 `mapping_rmap_tree_insert_after()`。
- 将现有 `vma_needs_copy()` 原样移至 mm/internal.h 作为共享 inline，
  与批处理实现放在同一份补丁中。插入 API 参数统一为 dst_vma、src_vma，
  顺序与 vma_needs_copy()、copy_page_range() 一致。
  dup_mmap 与 copy_page_range 共用判断，覆盖完整 VM_COPY_ON_FORK
  （含 VM_MAYBE_GUARD、VM_UFFD_RWP）以及父 VMA 的 anon_vma。
  v6 将该判断限定在 `if (file && vma_needs_copy(tmp, mpnt))`，仅控制
  批次刷新。copy_page_range 的外层调用条件恢复为原来的非 WIPEONFORK；
  不需要复制的情况仍由 copy_page_range 内部原有判断直接返回。
- 需要复制文件 VMA 的页表之前刷新队列。可延迟插入的子 VMA 尚无复制的页表，
  且子 mm 尚未交给新任务执行；truncate/rmap 在此期间不会漏掉其 PTE。
- vm_ops->open 按基线的原有时机执行，然后为当前文件 VMA 取得引用并
  入队；不再因为存在回调就强制刷新。不关联文件的 VMA 复制也不刷新
  已积攒的文件批次。
- 地址空间变更、满批需要继续入队时由 add 调用 process。正常结束和
  失败统一在 loop_out 处理尾批，之后再注册 KSM/khugepaged 或错误回收。
  arch_dup_mmap 恢复在循环结束后、loop_out 之前的原位置；其返回错误
  时仍先处理尾批，再进入原清理路径。
  __mt_dup 成功后才初始化批次，失败直接到 out 时没有批次需要处理。

## 错误路径核对

在当前 VMA 尚未入队时发生 vma_start_write_killable、DONTCOPY tree
修改、内存记账、vm_area_dup、policy、userfaultfd 或 anon_vma_fork
失败，loop_out 先插入先前已成功创建的子 VMA，再按现有 end 边界回收。
文件 VMA 的页表复制失败之前，队列已经刷新；不关联文件的 VMA 复制
失败时，可能仍有之前的文件批次，由 loop_out 处理。后续 teardown
沿用原实现；文件引用、可写计数、interval-tree 删除仍由既有生命周期负责。

## final 刷新点（v6）

dup_mmap 中只保留两处显式 final 调用：

1. 当前 VMA 有 vm_file 且 vma_needs_copy 为真时：保证复制文件页表
   之前，目标 VMA 已在 rmap 树中。
2. 公共 loop_out：处理正常结束、循环错误或架构初始化失败时的剩余批次。

v3 有五个调用点，v4 减少为三个。v5 去掉剩余 has_open 特殊处理，
并恢复 arch_dup_mmap 的原位置。当前 generic、arm64、powerpc、x86
（含 Xen enter_mmap）实现不依赖文件 rmap 树，也不为待入队文件 VMA
建立页表；不要求 arch_dup_mmap 前刷新。初始锁和 __mt_dup 错误绕过
loop_out，因为此时没有入队对象。

final 在队列为空时直接返回，不会重复插入，也不会再次获取 i_mmap 锁。

## 测试设计

扩展现有 mm/rmap.c：33 段不合并的文件映射，每段两页，覆盖共享、
私有、混合 COW、交替不同文件、只读 fd，以及同 inode 的不同 struct file。
COW 场景写第一页并预先读入第二页，覆盖同一 VMA 内的私有页和文件 PTE。
验证 fork 内容、子进程已缺页后的 truncate 失效、重新扩展后数据、
父进程解除映射后子映射阻止 F_SEAL_WRITE、子退出后计数恢复。
复用 guard-regions 的 fork、fork_cow、fork_wipeonfork 用例。

项目专用 fork-failure 在隔离 guest 中限制 overcommit，安排七个
文件 VMA 后接不能复制的 256 MiB 记账映射，重复触发 32 次 ENOMEM，
并检查文件封印成功。trace-batch.sh 在 x86_64 上探测批次大小，
验证满批和失败时剩余批次确实被刷新。

v5 新增项目诊断 fork-shm / trace-shm-batch.sh：同一 SysV SHM 段创建
33 个带 shm_open 回调的映射，检查 fork 后共享数据及 shm_nattch 在
33、66、33、0 之间转换；同时跟踪 shm_open 和批次处理，确认回调
实际执行且允许形成满批。此诊断不加入社区补丁，自测补丁保持原样。

trace-rmap-order.sh 采集 copy_page_range 入口的目标 vm_flags 和源 anon_vma。
宿主分析脚本按构建配置的 VM_COPY_ON_FORK 判断是否需要复制，仅在需要
复制时要求文件 VMA 的 rmap 插入已返回；不需要复制的调用会在函数内部
直接返回，可以早于插入。掩码和成员偏移从本项目编译配置提取，偏移再
由最终 vmlinux 核对。与 Yibin Liu RFC 及 Pedro
Falcato 回复的逐项比较见 design/lore-comparison.md。

fork-bench 对同一文件与交替不同文件分别测量 1/8/64/256 个 VMA，
每组 3 轮、每轮 20 次 fork+wait。这是 TCG、KASAN、lockdep 环境的
功能与初步耗时对比，不能外推为生产工作负载的性能提升。

## 环境

基线和修改内核使用同一 config.seed（x86_64、PER_VMA_LOCK、KASAN、
PROVE_LOCKING）。CONFIG_USERFAULTFD 未开启，其运行时路径尚未覆盖。
测试依赖仅下载解包至 dev/deps，不改变宿主系统软件。
原 rootfs 存在 ext4 位图校验错误；复制为 dev/rootfs.img 并在副本上
执行 e2fsck 修复，后续基线与补丁测试共用该修复副本的独立 overlay。
