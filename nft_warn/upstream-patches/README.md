# 原始主线补丁参考

这些文件通过 `git show --format=email <upstream SHA>` 导出，仅供静态审阅。
未应用到源码，也未做适配、编译或运行测试。编号仅为归档编号，不是应用顺序。

- `01`–`08`：输入八个 stable SHA 对应的原始主线补丁。stable 对应版本已在当前 6.6 分支。完整映射见 `../debug-context/commit-mapping.json`。
- `09-532aec7e878b.patch`：移除 dirty 及 commit 早退，本次 WARN 的直接相关主线改动。
- `10-8b8a2417558c.patch`：销毁路径支持 clone 为空，生命周期配套。当前分支若补全，建议先适配此补丁，再适配 `09`。
- `11-9df95785d3d8.patch`：GC 拆分的主线来源；已回移为 `7864c667aed0`，用于解释回移前提。
- `12-fb7fb4016300.patch`：只在 flush 时克隆，已回移为 `e38f054f0af9`，修复另一条 WARN。

应用顺序及适配要求以分析报告为准，不能将这些历史原始 diff 直接视为适用于当前 6.6 的补丁系列。
