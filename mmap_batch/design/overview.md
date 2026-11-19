# mmap_batch: fork 文件 VMA 插入批处理

## 当前版本

当前以 v6 为基础新增多 mapping 批次提交 9ed28899f15d，形成三个补丁
（本地 v8）。v6 原有两个提交保持不变；v7 继续挂起，其设计仍为
copy-batch.md。本轮实现和测试见 multiple-file-batches.md 与 dev/review-v8/。

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
- 同时保存 4 个 mapping 的批次，每槽容量为 8；优先匹配已有槽，再用
  空槽，满槽组遇到新 mapping 时刷新并复用首槽。final 处理全部非空槽。
- 单批获取一次 i_mmap 写锁和架构 dcache mmap 锁；沿用当前
  `vma_is_shared_maywrite()` 计数规则及 `mapping_rmap_tree_insert_after()`。
- 文件 VMA 入队后，若非 WIPEONFORK 且 vma_needs_copy(dst, src) 为真，
  在 copy_page_range 前刷新队列，使实际页表复制前目标已进入文件 rmap 树。
  无需复制的文件 VMA 可继续排队；没有 vm_file 的 VMA 不触发该刷新。
- vma_needs_copy 在 mm/internal.h 中共享，参数为 dst_vma、src_vma。
  该条件只控制文件批次刷新，copy_page_range 保持原有调用条件和内部判断。
- vm_ops->open 保持在当前 VMA 入队之前；arch_dup_mmap 的位置不变。
  loop_out 的 final 处理成功和失败路径的剩余项，然后进入原有清理流程。

## 错误路径与 final（沿用 v6）

显式 final 有两处：需要复制的文件 VMA 在复制前刷新，以及 loop_out
无条件完成非空队列。批处理仅负责插入，本身不会分配页表或返回错误。
失败路径在删除子 VMA 前补齐已创建项的文件树链接，使插入、可写计数
和原有 teardown 配对；未安装的 VMA 仍按原有 mpnt 边界排除。

当前系列保持 v6 的同步页表复制，因此 arch_dup_mmap 前不额外刷新。
相关回调、架构与清理风险核对在 dev/review-v6/risk-audit/risk-review.md；
没有把 v7 的错误边界或测试结果套用于 v6。

## v6 UnixBench spawn 的合批边界（2026-09-22）

QEMU 上静态和动态链接的 spawn，各运行 1、4 个 worker，3328 次 fork
均显示每个文件的 5 个 VMA 合为 4 + 1 两批：前三个无需复制，第四个
已有 anon_vma，入队后触发 final；第五个也需复制，形成单项批次。
动态版的可执行文件、libc、ld 均采用这一形态。平均批大小 2.5，插入侧
i_mmap 写锁次数节省 60%，但本轮没有整体吞吐收益证明。
因此复制前 final 是合批边界，不能据其触发频繁推断没有合批。
详情及原始证据见 dev/spawn-batch-20260922/report.md。

## 多 mapping 批次验证

64 段交错 2/4 文件实测 8 批，5 文件 65 段实测 32 批。当前 spawn 的
192 次 fork 仍保持每文件 4 + 1：静态每次 2 批，动态 6 批。优化针对
交错布局，未消除复制前边界。16 项文件自测和 128 次 ENOMEM 清理通过。
完整结果与限制见 dev/review-v8/report.md。

## 测试设计

扩展现有 mm/rmap.c：33 段不合并的文件映射，每段两页，覆盖共享、
私有、混合 COW、交错 2/4/5 个文件、只读 fd，以及同 inode 的不同 struct file。
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
