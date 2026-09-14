# nft_pipapo_destroy 告警静态分析

## 结论与范围

告警对应 [net/netfilter/nft_set_pipapo.c:2389](/home/sisyphus/code/lts/linux/net/netfilter/nft_set_pipapo.c:2389) 的 `WARN_ON_ONCE(!list_empty(&priv->gc_head))`：集合销毁时仍持有未排空的 GC 批次。当前代码存在与该断言吻合的静态生命周期缺陷：扫描先分配并挂接 GC 对象，无过期普通元素时 `dirty` 可保持 false，随后 commit 提前返回，绕过唯一的 GC 出队及引用释放路径。

该机制在当前 Linux 6.6.156 HEAD 仍存在。输入列出的八个补丁已经全部合入，不能将此告警归为尚未修复的历史 bad clone 问题。另发现按需 clone 的销毁路径缺少从 live match 释放元素的配套逻辑，属于独立的元素所有权遗漏。

本分析只读取源码、提交 diff 与公开报告。未复现、未构造请求或 PoC、未编译或运行测试、未应用补丁、未修改内核源码；原始 `stack`、`analyse.md` 保持原样。静态控制流与引用失衡已确认，但原始报告没有实际操作序列及运行时 `dirty`、`clone`、GC `count`，不能断言下面的可达路径就是原始事故的唯一来源。

## 报告、版本及堆栈

| 对象 | 已核对结果 |
|---|---|
| 原始告警版本 | Linux 6.6.151，`d27334b2888c` |
| 当前分支 | `linux-6.6.y`，Linux 6.6.156，`8b73de7da85fde281a385e0b26eda9bffd3ca477` |
| 主线比较基准 | 本地 `master`：`704340f1cd0dcef829eb62f5b48ae95a2ce17bdf` |
| 关键源码一致性 | 报告版本与 HEAD 的 `nft_set_pipapo.c`、`nf_tables_api.c` 无差异 |
| 平台 | Google Compute Engine，x86_64；CPU 0，PID 8，`kworker/0:0` |

清理后的相关堆栈如下；输入没有附带能够确定队列内容的诊断信息：

```text
Workqueue: events nf_tables_trans_destroy_work
nft_pipapo_destroy+0x84b/0x8b0
nft_set_destroy+0x489/0xa20
nft_commit_release [inline]
nf_tables_trans_destroy_work+0xb2d/0x11b0
process_one_work [inline]
process_scheduled_works
worker_thread
```

[原始 syzbot 条目](https://syzkaller.appspot.com/bug?extid=b338ce808595248a409a)与[邮件线程](https://groups.google.com/g/syzkaller-lts-bugs/c/THgivXwy8iU/m/5LdDQynRDAAJ)关联的原始报告为 2026-08-10，邮件给出了上述 6.6.151 修订。公开报告的堆栈与本地输入在忽略空白后匹配。资料于 2026-09-14 查询；网页索引快照可能早于查询日期。

## 根因：GC 所有权建立后被 dirty 早退截断

### 扫描即建立所有权，空批次仍需释放

[pipapo_gc_scan()](/home/sisyphus/code/lts/linux/net/netfilter/nft_set_pipapo.c:1695) 在扫描普通元素前执行：

```c
	gc = nft_trans_gc_alloc(set, 0, GFP_KERNEL);
	if (!gc)
		return;

	list_add(&gc->list, &priv->gc_head);
```

`nft_trans_gc_alloc()` 使用 `kzalloc()`，因此新对象的 `count` 为 0；分配成功还通过 `maybe_get_net()` 和 `refcount_inc(&set->refs)` 持有 net/set 引用，见 [nf_tables_api.c:9852](/home/sisyphus/code/lts/linux/net/netfilter/nf_tables_api.c:9852)。`gc_head` 非空不等于其中存在待回收元素。

只有遇到过期普通元素，扫描才设置 `priv->dirty = true`，见 [nft_set_pipapo.c:1736](/home/sisyphus/code/lts/linux/net/netfilter/nft_set_pipapo.c:1736)。因此完成一次未发现过期元素的扫描，也可能留下 `count == 0` 的已挂接对象。

### commit 的两个状态判定并不等价

当前 [nft_pipapo_commit()](/home/sisyphus/code/lts/linux/net/netfilter/nft_set_pipapo.c:1841)：

```c
static void nft_pipapo_commit(struct nft_set *set)
{
	struct nft_pipapo *priv = nft_set_priv(set);
	struct nft_pipapo_match *old;

	if (!priv->clone)
		return;

	if (time_after_eq(jiffies, priv->last_gc + nft_set_gc_interval(set)))
		pipapo_gc_scan(set, priv->clone);

	if (!priv->dirty)
		return;

	old = rcu_replace_pointer(priv->match, priv->clone,
				  nft_pipapo_transaction_mutex_held(set));
	priv->clone = NULL;
	priv->dirty = false;

	if (old)
		call_rcu(&old->rcu, pipapo_reclaim_match);

	pipapo_gc_queue(set);
}
```

`clone != NULL` 不能证明 `dirty == true`。[nft_pipapo_init()](/home/sisyphus/code/lts/linux/net/netfilter/nft_set_pipapo.c:2327) 仍预建 clone，然后显式将 dirty 清零。扫描之后的 `!dirty` 返回可以跳过发布以及 `pipapo_gc_queue()`。

“进入后端 commit 必定已设置 dirty”的反驳也不成立。通用 [nft_setelem_insert()](/home/sisyphus/code/lts/linux/net/netfilter/nf_tables_api.c:6580) 的 catchall 分支绕过后端 insert：

```c
	if (flags & NFT_SET_ELEM_CATCHALL)
		ret = nft_setelem_catchall_insert(net, set, elem, ext);
	else
		ret = set->ops->insert(net, set, elem, ext);
```

成功的通用元素事务仍由 [nft_add_set_elem()](/home/sisyphus/code/lts/linux/net/netfilter/nf_tables_api.c:7098) 加入提交链表，[nf_tables_commit() 的 NEWSETELEM 分支](/home/sisyphus/code/lts/linux/net/netfilter/nf_tables_api.c:10383) 将集合登记到 `set_update_list`，随后 [nft_set_commit_update()](/home/sisyphus/code/lts/linux/net/netfilter/nf_tables_api.c:10143) 调用后端 commit。该调用关系证明 dirty=false 的 commit 在源码中可达；不表示原始报告实际使用了 catchall。

### 唯一排空点及后果

[pipapo_gc_queue()](/home/sisyphus/code/lts/linux/net/netfilter/nft_set_pipapo.c:1766) 的私有队列排空逻辑为：

```c
	list_for_each_entry_safe(gc, next, &priv->gc_head, list) {
		list_del(&gc->list);
		nft_trans_gc_queue_sync_done(gc);
	}
```

[nft_trans_gc_queue_sync_done()](/home/sisyphus/code/lts/linux/net/netfilter/nf_tables_api.c:9929) 对空批次直接调用 `nft_trans_gc_destroy()`；非空批次交由 RCU 延迟销毁。[nft_trans_gc_destroy()](/home/sisyphus/code/lts/linux/net/netfilter/nf_tables_api.c:9777) 执行 `nft_set_put()`、`put_net()`、`kfree()`。跳过该链条会遗留 GC 对象和 set/net 引用；集合最终释放一次基础引用无法抵消额外 GC 引用，set/name 分配也被保留。

全部 `gc_head` 引用只有初始化、扫描入队、queue 出队和析构检查，没有其他清理线程接管该私有队列。已证明的空批次路径支持“告警及引用/内存泄漏”结论，不能据此扩大为元素 UAF 或任意写。

## 控制流时间线与排除项

这里是串行生命周期遗漏，无需构造 CPU 竞争窗口。`nf_tables_valid_genid()` 持有 `commit_mutex`，事务结束才释放，见 [nf_tables_api.c:10787](/home/sisyphus/code/lts/linux/net/netfilter/nf_tables_api.c:10787)。

| 阶段 | 源码状态变化及职责 |
|---|---|
| 集合状态建立 | init 可留下 clone 非空、dirty=false；通用事务登记不保证修改后端表。 |
| 后端提交中的扫描 | GC 分配成功即挂接到 gc_head；没有过期普通元素时不置 dirty。 |
| 同一 commit 的提前返回 | `!dirty` 返回；批次未交给 queue，相关引用仍归私有链表所有。 |
| 后续集合销毁 | 删除事务撤销集合的 RCU 可见性，异步销毁工作等待 RCU 后调用 backend destroy。 |
| 断言与泄漏 | destroy 发现 gc_head 非空并 WARN；没有排空该队列，额外引用未释放。 |

删除链条是 `nf_tables_commit()` → `nf_tables_commit_release()` → `nf_tables_trans_destroy_work()` → `nft_commit_release()` → `nft_set_destroy()` → `nft_pipapo_destroy()`。工作线程的 `synchronize_rcu()` 见 [nf_tables_api.c:9583](/home/sisyphus/code/lts/linux/net/netfilter/nf_tables_api.c:9583)；它不能回收从未提交给 RCU 的 GC 对象。dead set 跳过 commit 是现有销毁策略，只暴露先前遗留状态，不会自行创建 GC 对象。

排除与限定：

- 历史坏 clone：当前已有 NEW/MOD/ERR 隔离；本断言检查 gc_head。错误路径和 abort 本身不执行 GC 扫描，不能据函数名相同直接归因。
- abort 补救：[nft_pipapo_abort()](/home/sisyphus/code/lts/linux/net/netfilter/nft_set_pipapo.c:1866) 没有 GC 排空，无论早退还是释放 clone，都不能代替漏掉的 queue。
- 分配失败：首个 GC 分配失败不会新增节点；扩容失败发生在设置 dirty 之后，commit 会继续排空。已确认路径不依赖 OOM。
- catchall GC 分配失败：queue 中 catchall 处理失败不会跳过后面的 gc_head 遍历，因此不是遗漏来源。
- RCU：旧 match 的 RCU 回调只释放匹配表存储，不持有该私有 GC 链表的清理职责。

## 引入提交与版本边界

6.6 回移提交为 [`7864c667aed01a58b87ca518a631322cd0ac34c0`](https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/commit/?id=7864c667aed01a58b87ca518a631322cd0ac34c0)，作者 Florian Westphal `<fw@strlen.de>`，主题 `netfilter: nft_set_pipapo: split gc into unlink and reclaim phase`，主线来源为 `9df95785d3d8302f7c066050117b04cd3c2048c2`。提交自身没有 `Link:` trailer。

该回移把 GC 拆成扫描摘除和发布后回收，但保留了旧 dirty gate；实际 diff 同时展示新增入队与既有早退：

```diff
@@ -1632,6 +1632,8 @@ static void pipapo_gc(struct nft_set *set, struct nft_pipapo_match *m)
 	if (!gc)
 		return;
 
+	list_add(&gc->list, &priv->gc_head);
+
 	while ((rules_f0 = pipapo_rules_same_key(m->f, first_rule))) {
 		union nft_pipapo_map_bucket rulemap[NFT_PIPAPO_MAX_FIELDS];
 		const struct nft_pipapo_field *f;
```

```diff
@@ -1741,7 +1771,7 @@ static void nft_pipapo_commit(struct nft_set *set)
 	struct nft_pipapo_match *new_clone, *old;
 
 	if (time_after_eq(jiffies, priv->last_gc + nft_set_gc_interval(set)))
-		pipapo_gc(set, priv->clone);
+		pipapo_gc_scan(set, priv->clone);
 
 	if (!priv->dirty)
 		return;
```

对该提交自身的 init、catchall 分支和通用事务提交路径核对后，确认静态缺陷已自此存在。首次包含它的正式 6.6 tag 为 **v6.6.130**，不是八个候选补丁全部合入的 v6.6.148。旧实现还存在 commit 克隆失败后不排空 GC 的另一条路径；后来的 `744dc9a47a84` 消除了该分配失败路径，却保留了 `!dirty` 早退。

| 版本/历史阶段 | 与此告警相关的状态 |
|---|---|
| 主线 v6.10-rc1，2024 年 | 已包含按需 clone 配套销毁及 `532aec7e878b` 移除 dirty。 |
| 主线 v6.11 | 无 dirty；尚未引入本次 gc_head 及该断言。 |
| GC split，2026 年 | 主线 `9df95785d3d8` 在已有上述生命周期语义的基础上拆分 GC。 |
| stable v6.6.130 | `7864c667aed0` 引入 gc_head，但保留 dirty gate，静态遗漏开始可达。 |
| stable v6.6.148 | 下列八个候选补丁全部存在，dirty gate 仍在。 |
| stable v6.6.151 / 当前 v6.6.156 | 同一 gc_head 检查和静态缺陷仍在。 |
| 本地当前 master | 有 gc_head，但扫描后无 dirty 早退，正常完成发布及排空。 |

因此 v6.11 与当前主线没有同一缺陷的源码依据不同，不能仅用“未出现告警”证明所有 PIPAPO 问题均不存在。公开线程也有 5.15、6.1 报告，但本次未逐版本核对这两条分支的全部历史，不将 6.6 边界直接外推。

## 输入八个候选补丁：stable / upstream 完整映射

八个输入 SHA 是连续的 stable 回移提交，全部为当前 HEAD 祖先；对应原始主线 SHA 全部为 master 祖先。原始主线 SHA 不在 HEAD 祖先链，不能据此断言补丁缺失。以下按 stable 应用顺序列出：

| 序号 | 6.6 stable SHA | 原始 upstream SHA | 内容及本问题关系 |
|---|---|---|---|
| 1 | `385e2a9360cc6edf9562cf8abcc4a0f18c199890` | [`5b651783d80b97167ecd27dc6a4408c694873902`](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/commit/?id=5b651783d80b97167ecd27dc6a4408c694873902) | use GFP_KERNEL for insertions；为事务插入/删除使用 GFP_KERNEL 做准备；不清理 GC。 |
| 2 | `dde6b54848a5169baac365ef6ac1166899ac2396` | [`a590f4760922acaa2d2b55a88004a38eecdd6412`](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/commit/?id=a590f4760922acaa2d2b55a88004a38eecdd6412) | move prove_locking helper around；前移锁验证辅助函数；不改变 GC 行为。 |
| 3 | `7583b0d84ca272dc2e183f83b5395f5061444efa` | [`80efd2997fb9343a0283cf3cac5524a4595c8ff4`](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/commit/?id=80efd2997fb9343a0283cf3cac5524a4595c8ff4) | make pipapo_clone helper return NULL；统一 clone 分配失败为 NULL；不清理 GC。 |
| 4 | `8e264996d95f9b897e95c92ba0d71ff0e3246482` | [`6c108d9bee448a850b03e682836bfe91fca645cb`](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/commit/?id=6c108d9bee448a850b03e682836bfe91fca645cb) | prepare walk function for on-demand clone；区分遍历的读/更新上下文，为按需克隆准备。 |
| 5 | `840daa6bc4d12a57533d9bbcf348469bc15aa0b4` | [`c5444786d0ea2417a5e2cee7bd67137fc8bad687`](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/commit/?id=c5444786d0ea2417a5e2cee7bd67137fc8bad687) | merge deactivate helper into caller；合并 deactivate 辅助函数；不是此告警修复。 |
| 6 | `fad1685df350faed5927c33e991df8c9d948ae20` | [`a238106703ab4ae1090b86eba128815b8626d8f1`](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/commit/?id=a238106703ab4ae1090b86eba128815b8626d8f1) | prepare pipapo_get helper for on-demand clone；显式传递 match/clone，为按需克隆准备。 |
| 7 | `744dc9a47a8458ed49becd6123a092c7dae82b8b` | [`3f1d886cc7c3525d4dbeee24bfa9bb3fe0d48ddc`](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/commit/?id=3f1d886cc7c3525d4dbeee24bfa9bb3fe0d48ddc) | move cloning of match info to insert/removal path；将克隆前置到可返回失败的修改路径；仍保留 dirty 早退。 |
| 8 | `047e813324eac2ac60cddfb58bcdbd0144eadb09` | [`47e65eff50691f0a5b79d325e28d83ec1da43bcf`](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/commit/?id=47e65eff50691f0a5b79d325e28d83ec1da43bcf) | don't leak bad clone into future transaction；用 NEW/MOD/ERR 隔离坏 clone；已合入，不能解决空 GC 遗留。 |

[官方 CVE-2026-72252 公告](https://kernel.googlesource.com/pub/scm/linux/security/vulns/+/999ba78cd51962f21bcabc54b2093c12759db34d/cve/published/2026/CVE-2026-72252.mbox)将 `47e65eff5069` 与 `047e813324ea` 对应，6.6 首个修复版本为 6.6.148。公告针对 bad clone 状态，不能据此给此后出现的 gc_head 告警定性为同一缺陷。

## 主线相关语义改动及适配建议

### 1. 补全 clone 可空时的析构所有权

[`8b8a2417558c632f249abebde97adf8c46540de2`](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/commit/?id=8b8a2417558c632f249abebde97adf8c46540de2)，`netfilter: nft_set_pipapo: prepare destroy function for on-demand clone`。当前 6.6 缺少其核心语义：有 clone 时销毁 clone 中的元素，没有 clone 时从 live match 销毁元素，再释放表存储。

当前 [nft_pipapo_destroy()](/home/sisyphus/code/lts/linux/net/netfilter/nft_set_pipapo.c:2391) 先释放 live match 元数据，仅在 clone 存在时调用 `nft_set_pipapo_match_destroy()`。然而成功 commit 已将 clone 置 NULL，因此集合中实际元素可能无人销毁。表存储释放并不等于 `nf_tables_set_elem_destroy()`；这是与本次 gc_head WARN 区分的独立所有权缺口，单独补它也不会排空 gc_head。

### 2. 移除 dirty 的完整生命周期

[`532aec7e878b527fcee8877350ab5c5341789626`](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/commit/?id=532aec7e878b527fcee8877350ab5c5341789626)，`netfilter: nft_set_pipapo: remove dirty flag`。它发表于 2024 年，早于 GC split 和本次报告；属于主线已具备、能消除此回移缺口的语义改动，不是社区专门针对此次 syzbot 告警发布的修复。其实际 diff：

```diff
@@ -1823,13 +1818,9 @@ static void nft_pipapo_commit(struct nft_set *set)
 	if (time_after_eq(jiffies, priv->last_gc + nft_set_gc_interval(set)))
 		pipapo_gc(set, priv->clone);
 
-	if (!priv->dirty)
-		return;
-
 	old = rcu_replace_pointer(priv->match, priv->clone,
 				  nft_pipapo_transaction_mutex_held(set));
 	priv->clone = NULL;
-	priv->dirty = false;
 
 	if (old)
 		call_rcu(&old->rcu, pipapo_reclaim_match);
```

该补丁还删除 init 预建 clone、abort 的 dirty gate、dirty 字段及各处写入。结合当前 GC split，clone 存在时 commit 将完成 **scan → 发布新 match → queue/reclaim**；clone 不存在时在扫描前返回，不新增 GC 所有权。

建议当前 6.6 先适配 `8b8a2417558c` 的析构语义，再适配 `532aec7e878b` 的完整 dirty 移除。适配时保留现有 gc_head 初始化与断言、`pipapo_gc_queue()`、6.6 的 `void *` 元素接口及已回移的 GC 行为；dirty 的 GC 写入位置已从历史 `pipapo_gc()` 变为 `pipapo_gc_scan()`，init 错误标签也须按当前上下文处理。

不得将 GC 回收简单前移到 match 发布之前：`9df95785d3d8` 正是为了保证旧元素不再对新读者可见后，才启动 RCU 回收。只删除 WARN 或只清空链表也不能释放 GC/set/net 引用。

另一个相关主线提交 `fb7fb4016300ac622c964069e286dc83166a5d52`（`netfilter: nf_tables: clone set on flush only`）已回移为 `e38f054f0af98224003e600545726fbd96379cfb`。它限制必须 clone 的遍历类别，处理另一条 `nft_map_deactivate` WARN，未移除这里的 dirty gate。

本地 refs 的相关符号/主题历史及已执行的公开检索未发现更直接、已发布的专门 empty-GC 修复；该结论仅限已核对范围。

## 交付与验证边界

[原始主线补丁目录说明](upstream-patches/README.md)列出 12 个参考 patch，其中 01–08 对应候选系列，09/10 对应两项缺失语义，11 是 GC split，12 是 flush 配套。文件为 `git show --format=email` 原始输出，归档编号不是应用顺序；它们不是已适配到 6.6 的补丁系列。

已核对源码控制流、引用所有权、实际提交 diff、stable/upstream 映射及 tag 边界，并逐字节校验参考 patch。原始输入未变，已跟踪内核源码无修改。未执行补丁适用性检查、cherry-pick、编译、启动或运行时验证；没有将静态修复建议表述为测试通过。
