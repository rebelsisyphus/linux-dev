# CVE-2026-72252 / OLK-5.10 / 四前置重排版

已加入指定的四个前置及必要后续修复，共 **17 个主线补丁**。按用户指示，
本版本不包含 nft kABI 修复，旧版 iterator kABI 补丁也已从新版系列中移除。

## 分支与基线

- 当前集成分支：`backport/CVE-2026-72252-OLK-5.10-four-preps`。
- 集成 HEAD：`ed7491a7c5f0cc7d551c240654140f40763a4a1d`。
- 纯 CVE 系列分支：`backport/CVE-2026-72252-OLK-5.10-four-preps-series`。
- 纯系列末端：`3618a1a7f94d4f2690ae4aef0095ce49229f20e6`。
- 补丁应用基线：`b8095978261c8f5739d74a5b1ef5e117b863420f`。
- 原 `OLK-5.10` 保留在 `7c030816a7c58940b14ed821cd91e2c2cf2706f0`。

集成分支在这 17 个提交之后重放原有 CVE-2026-80668 的七个提交，包含原 conntrack
kABI 修复；其消息和 patch-id 均与旧版相同。本目录只导出 CVE-2026-72252 系列。
旧 `OLK-5.10/` 目录保留为 13 补丁历史版本，不能与本目录连续重复应用。

## 补丁顺序

全部相邻上游提交均校验为祖先顺序。来源、作者、日期、正文及原 trailers 保留；
沿用 issue 17278、CVE 字段和原仓库格式，Conflicts 中的原因提交为 12 位 SHA。

| # | 本地提交 | 上游 | 首次版本 | 用途 |
|---|---|---|---|---|
| 1 | `7f492c9c32df` | `a2a0ffb08468` | v6.5-rc1 | 新增前置：struct_size 分配大小 |
| 2 | `86475099d1d1` | `b9f052dc68f6` | v6.5-rc7 | 新增前置：事务锁 lockdep 判断 |
| 3 | `caf9cfa04c51` | `26cec9d4144e` | v6.7-rc1 | 前置：flush 直接停用元素 |
| 4 | `5cbc64424075` | `9dad402b89e8` | v6.7-rc1 | 新增前置：opaque 元素类型 |
| 5 | `d1ebcba612d6` | `ab0beafd52b9` | v6.8-rc4 | 新增 newbugfix：去掉 GET 的静态指针 |
| 6 | `1cf216dbe57d` | `f04df573faf9` | v6.9-rc1 | 新增前置：lookup const 参数 |
| 7 | `1391bff5c36d` | `5b651783d80b` | v6.9-rc1 | 前置：插入使用 GFP_KERNEL |
| 8 | `7cf0a4ad7938` | `80efd2997fb9` | v6.10-rc1 | 前置：克隆失败返回 NULL |
| 9 | `2f1efdf84638` | `8b8a2417558c` | v6.10-rc1 | 前置：按需克隆的销毁路径 |
| 10 | `52923552e0e6` | `6c108d9bee44` | v6.10-rc1 | 前置：按需克隆的遍历路径 |
| 11 | `a68c8c6a2c77` | `c5444786d0ea` | v6.10-rc1 | 前置：合并 deactivate helper |
| 12 | `6b2feef39472` | `a238106703ab` | v6.10-rc1 | 前置：GET 明确选择匹配表 |
| 13 | `1565151a320a` | `3f1d886cc7c3` | v6.10-rc1 | 前置：按需克隆 |
| 14 | `445da5645499` | `532aec7e878b` | v6.10-rc1 | 前置：移除 dirty 状态 |
| 15 | `b409bfc1aa43` | `c9526aeb4998` | v6.12-rc1 | 前置：abort 由后端丢弃克隆 |
| 16 | `3d00fb40b2c8` | `fb7fb4016300` | v7.0-rc3 | newbugfix：仅 flush 遍历创建克隆 |
| 17 | `3618a1a7f94d` | `47e65eff5069` | v7.2-rc2 | CVE 主修复：隔离失败克隆状态 |

## 依赖与后续修复

确认 `9dad402b89e8` 引入了 `nft_pipapo_get()` 的共享静态指针，已带入
[`ab0beafd52b9`](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/commit/?id=ab0beafd52b98dfb8b8244b2c6794efbc87478db)
作为独立 newbugfix。递归检索该修复及其余三个新增提交，结合相关函数历史，
截至本地主线 `9b87fdc9af2f` 未发现还需加入的后续修复。

`6509a2e410c3`、`0e1ea651c971`、`078996fcd657` 属于接口简化/压缩的上下文差异，
不构成此次类型转换的逻辑依赖。目标的 bool flush、NULL 分配失败、单表达式扩展、
原事务容器和 GC API 均保持原语义。`d8d871a35ca9` 未纳入此次四前置重排。
详见 [依赖说明](verification/research/dependency-analysis.md) 和 [提交核对](verification/logs/metadata-check.json)。

## 验证与复核

- 17 个补丁逐条应用并与各提交 tree 比较通过；逐提交 `git diff --check` 通过。
- 严格 checkpatch 原始日志已保留。仅豁免仓库元数据的 `GIT_COMMIT_ID`、
  `COMMIT_LOG_LONG_LINE`，以及第 16 补丁原始堆栈的 `REPEATED_WORD` 后通过，代码无诊断。
- 发行版 `NF_TABLES=m` 和精简 `NF_TABLES=y` 配置下，API/core、
  PIPAPO/AVX2、hash、rbtree、bitmap、dynset 八个对象编译通过。
  两种关闭 PROVE_RCU 的配置有一条上游 lockdep helper 未使用警告；原始日志保留。
  开启 KASAN、PROVE_LOCKING/RCU 的完整 QEMU 内核、模块及测试探针编译通过，无编译警告。
- QEMU 中六种 backend 的 insert/get/miss/delete/abort/flush/destroy 通过；
  96 次并发 PIPAPO GET、32 次 dynset 包路径 update 通过，kprobe 确认各 backend 均执行。
- map/table/匿名 map/netns 销毁、回滚反向激活、已有 clone 复用均通过；
  flush 克隆 ENOMEM 注入和后续恢复通过。
- 主 CVE 的 8 次 NEW、4 次 MOD 定点分配失败通过，验证后续 GET、包匹配、
  删除、flush、timeout GC 和下一事务恢复；scratch 探针 68 次命中、12 次实际注入。
- 无新增 KASAN、RCU、锁、引用计数或原子上下文睡眠异常；taint=4096 仅来自测试模块。
  测试规则与模块已清理，虚拟机已正常关机。

首次测试命令在新用例全部通过后，因 `pipapo_clone` 被编译器内联而无法装入旧探针。
已根据反汇编改用实际分配路径 `pipapo_maybe_clone.part.0`，续跑剩余回归全部通过；
内核补丁未因测试修改。保留首次失败及后续成功日志，见
[运行汇总](verification/logs/runtime-summary.json)、[续跑日志](verification/logs/runtime-remainder.log)。

运行范围限于 x86_64 QEMU；AVX2 对象已编译，未强制执行 AVX2 路径，未运行原始攻击 PoC
或长期压力测试。rootfs 启动/安装工具时有与旧测试相同的 EXT4 bitmap checksum 错误。
复核未发现新增功能回归，见 [复核报告](verification/review/review.txt)。

## 应用与复现

在上述基线的新分支上按 `series` 应用：

```sh
CVE_PATCH_DIR=/home/sisyphus/code/kernel/cve_patches/CVE-2026-72252/OLK-5.10-four-preps
while IFS= read -r patch; do
    git am "$CVE_PATCH_DIR/$patch" || break
done < "$CVE_PATCH_DIR/series"
```

完整复现命令见 [commands.txt](verification/commands.txt)，配置和测试源码位于 `verification/`。
源码构建在隔离 checkout 的提交 `70842a4d1c0c0df37f12af5917eed074a448d93f` 执行。
交付提交仅进一步整理消息，两者源码 tree 一致：`2c73db0e777bc757c4416d2d9d0724c2a11e3ad7`。
