# CVE-2026-72252 / OLK-5.10：18 补丁收敛版

基线：`b8095978261c8f5739d74a5b1ef5e117b863420f`。issue：17278。
主线取证仓库：`/home/sisyphus/code/linux`，本次沿用取证快照 `9b87fdc9af2fbfcdb5c24a64139685ef80f6573f`。

纯 CVE 系列分支：`backport/CVE-2026-72252-OLK-5.10-maybe-unused-series`。
纯系列 HEAD：`8805c7a55dd02ac8d80822a040e96a08b7857c78`。
集成分支：`backport/CVE-2026-72252-OLK-5.10-maybe-unused`。
集成 HEAD：`a8fb09599f4ed39d3c2d2e3cd680b26ae7768741`。
集成分支在 18 个本 CVE 补丁之后保留原有 7 个 CVE-2026-80668 补丁；
这 7 个提交的代码 patch-id、正文均保持不变，不计入本目录的 18 个补丁。

## 本轮变更

- 25 → 18：撤掉 `65e9eb1ccfe5` 全局 RCU 宏修改及其 6 个前置：
  `9f14cb030d98`、`891cd1f99dd9`、`d97f3bdf7a1c`、`a72e9d547205`、
  `f505d4346f61`、`cd539cff9470`。
- 第 2 个补丁（`b9f052dc68f6`）引入 helper 时添加 `__maybe_unused`；
  第 14 个补丁（`3f1d886cc7c3`）移动函数时保留该标记。
  这是 OLK-5.10 的局部编译适配，commit message 中已明确说明。
  未开启 PROVE_RCU 时旧宏会移除 helper 调用；开启时锁检查照常保留。
- 第 8 个补丁继续补齐 `e79b47a8615d`：在 `nft_flush_set()` 和
  `nft_lookup_validate_setelem()` 中过滤 next generation 非活动元素。
  前者阻止重复 flush 的重复删除事务，后者补齐 verdict-map 校验过滤。
- 相对原始 17 补丁版，最终源码差异仅为以上 3 个 netfilter 文件中的
  8 行新增、1 行删除；全局 RCU、lockdep、SRCU、socket/TC 文件均恢复基线。
- 保留原上游顺序、作者、日期、正文和来源字段。Conflicts 的来源 SHA
  仍使用 12 位；没有添加 nft kABI 补丁。

## 提交对应关系

| 序号 | 本地提交 | 主线提交 | 标题 |
|---|---|---|---|
| 1 | `fa2382737f48` | `a2a0ffb08468` | netfilter: nft_set_pipapo: Use struct_size() |
| 2 | `ac94bc96be3a` | `b9f052dc68f6` | netfilter: nf_tables: fix false-positive lockdep splat |
| 3 | `9b3a68992fda` | `26cec9d4144e` | netfilter: nft_set_pipapo: no need to call pipapo_deactivate() from flush |
| 4 | `5b6e9d53420a` | `9dad402b89e8` | netfilter: nf_tables: expose opaque set element as struct nft_elem_priv |
| 5 | `bdbaedf35639` | `ab0beafd52b9` | netfilter: nft_set_pipapo: remove static in nft_pipapo_get() |
| 6 | `e95bc6179c1a` | `f04df573faf9` | netfilter: nft_set_pipapo: constify lookup fn args where possible |
| 7 | `faa3bd3a10cc` | `5b651783d80b` | netfilter: nft_set_pipapo: use GFP_KERNEL for insertions |
| 8 | `2106d8d41690` | `e79b47a8615d` | netfilter: nf_tables: restore set elements when delete set fails |
| 9 | `6dfb21b2c4da` | `80efd2997fb9` | netfilter: nft_set_pipapo: make pipapo_clone helper return NULL |
| 10 | `60bfc5982a09` | `8b8a2417558c` | netfilter: nft_set_pipapo: prepare destroy function for on-demand clone |
| 11 | `15be991cbfdf` | `6c108d9bee44` | netfilter: nft_set_pipapo: prepare walk function for on-demand clone |
| 12 | `b7c8fa3dff6f` | `c5444786d0ea` | netfilter: nft_set_pipapo: merge deactivate helper into caller |
| 13 | `3f2fa719bcb4` | `a238106703ab` | netfilter: nft_set_pipapo: prepare pipapo_get helper for on-demand clone |
| 14 | `e056a5eb0769` | `3f1d886cc7c3` | netfilter: nft_set_pipapo: move cloning of match info to insert/removal path |
| 15 | `271cbc4f4b66` | `532aec7e878b` | netfilter: nft_set_pipapo: remove dirty flag |
| 16 | `873705b013b1` | `c9526aeb4998` | netfilter: nf_tables: do not remove elements if set backend implements .abort |
| 17 | `0b4eb9d9d828` | `fb7fb4016300` | netfilter: nf_tables: clone set on flush only |
| 18 | `8805c7a55dd0` | `47e65eff5069` | netfilter: nft_set_pipapo: don't leak bad clone into future transaction |

每个补丁内保留完整主线 SHA、来源版本和 Reference 链接。
`series` 给出应用顺序。旧 17 / 25 补丁分支与产物均保留。

## 验证

- 18 个补丁逐个应用到临时索引，与对应提交的 tree 全部一致。
- 每个提交 `git diff --check` 通过；上游祖先顺序通过。
- 除第 2、14 个补丁的 helper 注解外，其余 16 个与 25 补丁版保留项
  的 patch-id 相同；去掉该注解后，第 2、14 项也相同。
- `CONFIG_PROVE_RCU=n/y` 两个 x86_64 配置，各编译 10 个 netfilter
  对象并使用 `KCFLAGS=-Werror=unused-function`，均无编译警告或错误。
  使用独立 O= 目录。NF_TABLES 分别为模块和内建。
- 原始 checkpatch 日志完整保留。按仓库格式豁免 GIT_COMMIT_ID 和
  COMMIT_LOG_LONG_LINE 后通过；第 17 项另保留已审阅的上游堆栈重复词
  REPEATED_WORD 例外。源码检查没有残留告警。
- 代码复核未发现本轮新增的功能问题，详见
  [review.txt](verification/review/review.txt)。沿用原 17 项审查，重点复核
  本轮注解、移除全局宏修改后的配置语义，以及保留的 e79 回调检查。

未执行复现、QEMU、故障注入或其他运行时测试；未进行完整内核链接或
其他架构编译。编译后仅规范了提交正文的尾部换行，源码 tree 完全一致；
编译日志同时记录实际编译提交与最终等价提交，未将其表述为再次编译。

核验日志在 `verification/`。`research/delta-vs-17.diff` 和
`research/delta-vs-25.diff` 展示两版差异（均位于 verification 下）。
