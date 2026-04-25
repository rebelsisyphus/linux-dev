
1.目标场景
execl测试用例在4numa, 320核机器上并发执行时存在锁竞争，测试模型：execl二进制文件使用execl libc接口循环调用自己
2.锁竞争
dup_mmap, vma_prepare等函数钟持有vma对应file的address_space锁（mapping->i_mmap_rwsem)，导致严重锁竞争
3.社区做的尝试性优化
3.1 commit 3577dbb19241("mm: batch unlink_file_vma calls in free_pgd_range")
对mmput流程的vma遍历做了批量处理，减少持锁次数，但效果有限
3.2 numa级别的immap红黑树
参见目录下numa_immap_tree.mbox。将immap树拆分成numa级别，减少临界区。但社区maintainer提出了反对意见
3.3 跳过so的rmap操作
参见目录下skip_rmap.mbox. 增加flag，跳过so的rmap操作，社区maintainer坚决反对，但Mateusz Guzik提出了一些优化建议，很值得参考
4.优化方向
参考章节3钟社区做的优化和社区的反馈意见优化mapping lock, 目前看批量处理vma和分片锁值得考虑，着重考虑下Mateusz Guzik的建议
5.开发原则
以优化execl并发性能为第一原则，激进开发，不要回退
修改拆分成合理的commit, 符合linux社区规范
