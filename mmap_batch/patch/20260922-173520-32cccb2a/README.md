# mmap_batch：v6 + 多 mapping 批次（本地 v8）

当前 HEAD 为 9ed28899f15dd4538d94e48e0526176731a320a5。
v6 的两个提交保持不变，在 series 中追加第三个补丁；v7 继续挂起。

1. mm: batch file VMA insertions in dup_mmap
2. selftests/mm: cover file VMA lifetime across fork
3. mm: keep multiple file VMA batches in dup_mmap

第三个补丁参考用户提供的同名补丁，适配为 4 个 mapping 槽、每槽 8 项；
保留 v6 的同步 copy_page_range 和两个 final，增加对应自测布局。
保留原输入 Signed-off-by 并标注 Assisted-by，没有新增人工签署身份。
cover 是草稿，未发送邮件。

构建、三个补丁的严格 checkpatch 及顺序应用检查均通过。
QEMU 20260922-173702-cab84bbe passed，VM stopped；16 项文件自测、
SysV、guard（5 pass / 4 skip）、128 次 ENOMEM 清理检查通过。

交错 2/4 文件的 64 段布局实测 8 批，v6 代码对应 64 批；5 文件 65 段
实测 32 批。spawn 静态/动态仍分别为 2/6 批，没有实机吞吐收益结论。

设计：../../design/multiple-file-batches.md。
验证：../../dev/review-v8/report.md、results.json、multiple-results.json。
旧 v7：refs/mmap_batch/revisions/v7；当前：refs/mmap_batch/revisions/v8。
