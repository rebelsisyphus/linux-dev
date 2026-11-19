# mmap_batch v7

- 基线：9b87fdc9af2fbfcdb5c24a64139685ef80f6573f。
- 实现：aeb884f315c6d69a5ba9995bafb9ce8604357efc。
- 自测 / HEAD：668fa34e8896ac58334ef040ac81ec0f9f32a540。
- v6 备份：refs/mmap_batch/revisions/v6。
- 两份正式补丁，按 series 顺序应用；没有未提交源码差异。
- 0000 是 cover 草稿；没有发送邮件。

每批先插入全部文件 VMA，解锁后调用 copy_page_range_batch。
内部保留逐 VMA 的复制逻辑；没有合并 MMU notifier 或写保护序列。
旧批复制失败时仍把当前已创建项纳入链接和回收，尾批复制先于架构钩子。

QEMU spawn：静态 2→1 批，动态 6→3 批，每批 5 项，3481 次 fork。
这是相对 v6 的插入侧锁次数改善；没有实机吞吐 A/B 数据。
14 项 fork_file 自测、guard、SysV、64 次记账失败，以及 84 组页分配
故障注入中的 54 次 ENOMEM 与恢复检查通过。完整统计在
../../dev/review-v7/results.json，逐事件分析在 trace-results.json。

首轮在内核回归通过后因诊断程序对 fail-nth 使用 lseek 而中止。
诊断改为 pread/pwrite，内核源码未变；第二轮完成故障与 spawn 验证。
两台 VM 均已停止，失败运行状态和原始日志没有改写。

设计：../../design/copy-batch.md。
测试记录及限制：../../dev/review-v7/report.md。
复现完整验证：

```sh
ROOTFS=/home/sisyphus/code/kernel-dev/mmap_batch/dev/rootfs.img develop mmap_batch test
```

测试输入及跟踪偏移绑定当前构建；更换配置/架构时需重新核对。
