# Sashiko Patchset Review Report v2

## Metadata
- **Repository**: `/home/sisyphus/code/fork/kernel`
- **Patch Directory**: `/home/sisyphus/code/fork/kernel/unixbench-opt`
- **Review Date**: 2026-08-31
- **Total Patches**: 26 (excluding cover-letter)
- **Base Commit**: `cece3eeed8c9692d37c9b398eb91f7c69e93a101`
- **Final Tree Commit**: `8b936c9b6503193e0650bb11ded13410dacf12d5`
- **Reviewer**: Sashiko Agent (OpenCode)
- **Model**: kimi-for-coding/kimi-for-coding

## Summary

This patchset is an optimized/follow-up revision of the earlier `unixbench`
series.  A tree-level comparison against the previous series top commit
(`c6f1e25fdcd5`) shows that only **two files changed** in the final tree:
`fs/inode.c` and `mm/rmap.c`.  Both changes are fixes for the two blocking
issues identified in the earlier Sashiko review
(`unixbench/sashiko-review-report-v2.md`):

1. `i_mmap_lock_contention` and the dynamic shard pointer are now reset on
   every inode allocation in `inode_init_always()` (Patch 4, later made
   conditional in Patch 19), so slab-object reuse can no longer inherit stale
   shard-activation state.
2. The shard-aware `rmap_walk_file_vma()` callback has been restored to use
   `VM_BUG_ON_VMA(address == -EFAULT, vma)`, matching the baseline diagnostic
   invariant.

A re-check of the final tree (`review-ana.md`) confirms that **no unfixed
correctness defects remain**.  Several items previously marked Medium are
actually future-maintenance assumptions or documented design trade-offs rather
than current bugs.  The report has therefore been revised to downgrade those
items to informational/maintenance notes.

**Overall verdict: Approved (LGTM)** — the blocking correctness defects from
the previous review are resolved.  The remaining notes are minor maintenance
suggestions or measurement items and do not block merging.

---

## Per-Patch Reviews

### Patch 01: mm: batch file VMA insertions in dup_mmap
**Commit**: `929238e61409`
**Author**: Dong Chenchen
**Files Changed**: `kernel/fork.c`, `mm/Kconfig`

#### Analysis
Adds batched insertion of child file VMAs during `dup_mmap()` for up to 8 VMAs
per active `address_space`, flushing the batch before `copy_page_range()`
boundaries.  Identical to the original series.

#### Findings
- No new correctness issues.

#### Verdict
LGTM

---

### Patch 02: mm: batch file VMA unlink by address_space
**Commit**: `b78799384dd6`
**Author**: Dong Chenchen
**Files Changed**: `mm/internal.h`, `mm/mmap.c`

#### Analysis
Regroups file VMA unlink in `exit_mmap()` by `address_space` instead of by
`struct file`.  Identical to the original series.

#### Findings
- No new correctness issues.

#### Verdict
LGTM

---

### Patch 03: mm: keep multiple file VMA batches in dup_mmap
**Commit**: `8d7e05eb4d56`
**Author**: Dong Chenchen
**Files Changed**: `kernel/fork.c`

#### Analysis
Extends `dup_mmap()` to keep up to 4 concurrent file-VMA batches indexed by
`address_space`.  Identical to the original series.

#### 🔵 Low / Policy note
1. **Batch eviction flushes slot 0 unconditionally**
   - **Location**: `kernel/fork.c:855-881`
   - **Description**: When all four batch slots are occupied and no exact match
     or empty slot exists, slot 0 is always flushed.  The policy is safe and
     avoids recency state in the fork fast path, but may flush more often than
     LRU for interleaved workloads with more than four active mappings.  No
     benchmark data is available to quantify the effect.
   - **Impact**: Performance only; not a correctness defect.
   - **Fix**: Document the deliberate policy; revisit only if fork-heavy
     workloads show regression.

#### Verdict
LGTM

---

### Patch 04: mm: add optional NUMA-local i_mmap shard state
**Commit**: `291661804f19`
**Author**: Dong Chenchen
**Files Changed**: `fs/inode.c`, `include/linux/fs.h`, `include/linux/mm_types.h`, `kernel/fork.c`, `mm/Kconfig`

#### Analysis
Introduces `struct i_mmap_shards`, `i_mmap_nr_vmas`, `i_mmap_lock_contention`,
and per-mm NUMA home node / shard index.  This patch also adds the initial
reset of the dynamic shard fields in `inode_init_always()` (later made
conditional on `i_mmap_opt_enabled()` in Patch 19).

#### 🟢 Earlier stale-contention finding: FIXED
The previous `unixbench` review found that `i_mmap_lock_contention` survived
inode slab-object reuse, causing a newly allocated inode to inherit stale
shard-activation state.  This patch adds the reset in `inode_init_always()`:

```c
#ifdef CONFIG_I_MMAP_SHARDS
	mapping->i_mmap_shards = NULL;
	atomic_set(&mapping->i_mmap_nr_vmas, 0);
	atomic_set(&mapping->i_mmap_lock_contention, 0);
#endif
```

(The reset was later gated by `i_mmap_opt_enabled()` in Patch 19, which is safe
because there are no readers of these fields when the key is off.)

#### 🔵 Low / Maintenance note
1. **`hash_ptr(..., ilog2(I_MMAP_SHARDS_PER_DOMAIN))` assumes power of two**
   - **Location**: `kernel/fork.c:1510`
   - **Description**: `I_MMAP_SHARDS_PER_DOMAIN` is defined as 4, so the shift
     value is exactly 2 and the hash result is always in `0..3`.  Changing the
     constant to a non-power-of-two would underuse some shards but would not
     overflow the array.
   - **Impact**: Maintenance constraint; current code is correct.
   - **Fix**: Add `BUILD_BUG_ON(!is_power_of_2(I_MMAP_SHARDS_PER_DOMAIN))` to
     document the design constraint.

#### Verdict
LGTM

---

### Patch 05: mm: allocate i_mmap shards on their NUMA nodes
**Commit**: `a8c265fe4cec`
**Author**: Dong Chenchen
**Files Changed**: `fs/inode.c`, `include/linux/fs.h`, `mm/mmap.c`

#### Analysis
Adds `i_mmap_shards_alloc/free()` and frees shards in `__destroy_inode()`.
Allocation and cleanup paths are symmetric and use the correct NUMA node.
Identical to the original series.

#### Findings
- No new correctness issues.

#### Verdict
LGTM

---

### Patch 06: mm: account file VMAs independently of i_mmap layout
**Commit**: `147ab96294cd`
**Author**: Dong Chenchen
**Files Changed**: `include/linux/fs.h`, `kernel/fork.c`, `mm/mmap.c`

#### Analysis
Adds `i_mmap_nr_vmas` counter and updates it on every insert/remove.
Identical to the original series.

#### 🔵 Low / Maintenance note
1. **`i_mmap_nr_vmas` counter invariant is enforced by current code but not by helpers**
   - **Location**: `include/linux/fs.h`, `mm/mmap.c`, `kernel/fork.c`
   - **Description**: A complete scan of the final tree shows that every
     permanent file VMA insert/remove path pairs the interval-tree mutation
     with `i_mmap_vma_count_add/sub()`.  Shard installation only migrates
     existing VMAs, so the count is unchanged.  Split/merge temporarily removes
     and re-inserts the same VMAs, and new split VMAs are accounted separately.
   - **Impact**: No current defect.  A future caller that mutates a shard root
     directly could silently desynchronize `mapping_mapped()`.
   - **Fix**: Centralize permanent tree mutations in helpers, or add
     `VM_WARN_ON_ONCE` assertions in the raw interval-tree helpers when
     `CONFIG_I_MMAP_SHARDS` is enabled.

#### Verdict
LGTM

---

### Patch 07: mm: add NUMA-aware i_mmap shard installation
**Commit**: `02e04b6da3fd`
**Author**: Dong Chenchen
**Files Changed**: `include/linux/fs.h`, `mm/mmap.c`

#### Analysis
Implements `i_mmap_shards_install_locked()` and the migration of VMAs from the
central tree to shard trees.  Identical to the original series.

#### Findings
- No new correctness issues.  Shard domains are allocated for `N_POSSIBLE`,
  and `num_possible_nodes()` does not shrink at runtime.  The deterministic
  hash fallback ensures migration and later lookups agree.

#### Verdict
LGTM

---

### Patch 08: mm: route file VMA updates through shard-aware locks
**Commit**: `efcafe13f9df`
**Author**: Dong Chenchen
**Files Changed**: `include/linux/fs.h`, `kernel/fork.c`, `mm/internal.h`, `mm/mmap.c`, `mm/mremap.c`

#### Analysis
Wraps file VMA insert/remove with `i_mmap_lock_write_vma()` / unlock.
Identical to the original series.

#### 🔵 Low / Maintenance note
1. **`vma_prepare()` assumes `vp->insert` shares mapping/shard with `vp->vma`**
   - **Location**: `mm/mmap.c`
   - **Description**: The only current assignment to `vp->insert` is in
     `__split_vma()` via `vm_area_dup(vma)`.  The clone preserves `vm_file`
     and `vm_mm`, so the insert necessarily shares the same mapping, NUMA home
     node, and shard as the original VMA.  No other caller sets `vp->insert`.
   - **Impact**: No current defect; the invariant is implicit and could be
     violated by a future caller of `init_multi_vma_prep()`.
   - **Fix**: Add a debug assertion such as
     `VM_WARN_ON_ONCE(vp->insert->vm_file->f_mapping != vp->mapping)`.

#### Verdict
LGTM

---

### Patch 09: mm: add all-shard i_mmap read iteration
**Commit**: `1609cf90166b`
**Author**: Dong Chenchen
**Files Changed**: `include/linux/fs.h`, `mm/mmap.c`

#### Analysis
Adds `i_mmap_read_walk()`, `i_mmap_walk_locked()`, `i_mmap_lock_read_all()`,
etc.  Identical to the original series.

#### Findings
- No duplicate-callback concern: `i_mmap_lock_shards_read()` acquires all shard
  locks before any callback runs.  If a trylock fails, all already-acquired
  locks are released and `-EAGAIN` is returned with zero callback side effects,
  so retry cannot duplicate work.

#### Verdict
LGTM

---

### Patch 10: mm: make core file rmap walkers shard-aware
**Commit**: `fd98e9713d64`
**Author**: Dong Chenchen
**Files Changed**: `mm/memory.c`, `mm/rmap.c`

#### Analysis
Routes `rmap_walk_file()` and `unmap_mapping_pages()` through the common shard
iterator.

#### 🟢 Earlier diagnostic regression: FIXED
The previous `unixbench` review noted that the original series weakened the
baseline `VM_BUG_ON_VMA(address == -EFAULT, vma)` to a warning-and-skip in the
shard-aware callback.  This patch restores the baseline behavior:

```c
	address = vma_address(&walk->folio->page, vma);
	VM_BUG_ON_VMA(address == -EFAULT, vma);
	cond_resched();
```

This matches the non-sharded path and preserves the invalid-address diagnostic
invariant.

#### Findings
- No new correctness issues introduced by the restoration.

#### Verdict
LGTM

---

### Patch 11: mm/memory-failure: walk sharded file mappings
**Commit**: `8d2fbf18e14a`
**Author**: Dong Chenchen
**Files Changed**: `mm/memory-failure.c`

#### Analysis
`collect_procs_file()` walks all shards.  Identical to the original series.

#### Findings
- Holds the same set of mapping read locks as the baseline, so serialization
  on writers is unchanged.  Per-VMA RCU entry/exit adds latency on the rare
  memory-failure path, but this is a performance trade-off.

#### Verdict
LGTM

---

### Patch 12: mm/khugepaged: find retractable mappings in all shards
**Commit**: `ee11d8c6d797`
**Author**: Dong Chenchen
**Files Changed**: `mm/khugepaged.c`

#### Analysis
`retract_page_tables()` walks every shard.  Identical to the original series.

#### Findings
- No new issues.

#### Verdict
LGTM

---

### Patch 13: mm/pagewalk: traverse every i_mmap shard
**Commit**: `8ad306c665d5`
**Author**: Dong Chenchen
**Files Changed**: `mm/mapping_dirty_helpers.c`, `mm/pagewalk.c`

#### Analysis
`walk_page_mapping()` takes shard locks internally.  Identical to the original
series.

#### Findings
- Kerneldoc documents the locking contract for both sharded and non-sharded
  builds.

#### Verdict
LGTM

---

### Patch 14: uprobes: build registration maps from all i_mmap shards
**Commit**: `12fd5863bd1c`
**Author**: Dong Chenchen
**Files Changed**: `kernel/events/uprobes.c`

#### Analysis
`build_map_info()` uses `i_mmap_read_walk()`.  Identical to the original
series.

#### Findings
- No new issues.

#### Verdict
LGTM

---

### Patch 15: mm: make architecture i_mmap cache walkers shard-aware
**Commit**: `db6df2a961b9`
**Author**: Dong Chenchen
**Files Changed**: `arch/arm/mm/fault-armv.c`, `arch/arm/mm/flush.c`,
`arch/nios2/mm/cacheflush.c`, `arch/parisc/kernel/cache.c`, `include/linux/fs.h`,
`mm/mmap.c`

#### Analysis
Adds `i_mmap_walk_dcache_locked()` for cache-flush walkers.  Identical to the
original series.

#### 🔵 Low / Maintenance note (current architecture support)
1. **Dcache walkers rely on every shard-tree writer taking the dcache lock**
   - **Location**: `mm/mmap.c:491-501`, `arch/arm/mm/fault-armv.c:178`,
     `arch/arm/mm/flush.c:291`, `arch/nios2/mm/cacheflush.c:218`,
     `arch/parisc/kernel/cache.c:547`
   - **Description**: `i_mmap_walk_dcache_locked()` walks shard trees without
     holding the shard rwsems.  Current correctness is guaranteed because every
     file interval-tree mutation also holds `flush_dcache_mmap_lock()`.  A
     complete scan of writers confirms this coverage.
   - **Important caveat**: `I_MMAP_SHARDS` depends on `CONFIG_NUMA`
     (`mm/Kconfig:5-7`), but `arch/arm`, `arch/nios2`, and `arch/parisc` do not
     define `CONFIG_NUMA`.  The new `#ifdef CONFIG_I_MMAP_SHARDS` branches in
     these architecture files are therefore dead code under current Kconfig and
     cannot be exercised.
   - **Impact**: No current correctness hazard in the supported architecture
     matrix; future NUMA support on these architectures would need to preserve
     the dcache-lock invariant.
   - **Fix**: Add a lockdep assertion in `__vma_link_file()` /
     `__remove_shared_vm_struct()` that the dcache lock is held when a shard
     root is modified, where the architecture provides a real lock.

#### Verdict
LGTM

---

### Patch 16: mm: preserve all-lock semantics for sharded i_mmap
**Commit**: `4faa9c9f463c`
**Author**: Dong Chenchen
**Files Changed**: `mm/mmap.c`

#### Analysis
`vm_lock_mapping()` and `mm_take_all_locks()` preserve the all-lock semantics.
Identical to the original series.

#### Findings
- No new issues.

#### Verdict
LGTM

---

### Patch 17: mm: hold all i_mmap shards for locked file rmap
**Commit**: `a9423cbb0ac8`
**Author**: Dong Chenchen
**Files Changed**: `include/linux/fs.h`, `mm/huge_memory.c`, `mm/mmap.c`

#### Analysis
`split_huge_page_to_list_to_order()` uses `TTU_RMAP_LOCKED` semantics and
acquires all shard read locks.  Identical to the original series.

#### 🔵 Low / Trade-off note
1. **File-THP split blocks every shard writer**
   - **Location**: `mm/huge_memory.c:3691-3791`, `mm/mmap.c:391-422`
   - **Description**: Acquiring all shard read locks is required for
     `TTU_RMAP_LOCKED` correctness: `unmap_folio()` calls `try_to_unmap()`
     with `TTU_RMAP_LOCKED`, and `rmap_walk_locked()` must see a stable view of
     every shard.  The baseline code held the central `i_mmap_rwsem` read lock
     over the same interval, blocking the same set of writers.  The sharded
     implementation therefore does not broaden the set of blocked writers, but
     it pays the cost of acquiring multiple read locks.
   - **Impact**: Necessary synchronization overhead; potential latency change
     compared to baseline for workloads that repeatedly split file THPs while
     concurrently mapping/unmapping the same sharded file.  No benchmark data
     is available to quantify the effect.
   - **Fix**: Document the trade-off; measure `THP_SPLIT_PAGE` latency with
     `i_mmap_opt=on` if performance claims are made.

#### Verdict
LGTM

---

### Patch 18: mm: enable NUMA-local i_mmap shards after contention
**Commit**: `aba6ec3fee7c`
**Author**: Dong Chenchen
**Files Changed**: `mm/mmap.c`

#### Analysis
Implements dynamic shard installation after 256 failed trylocks on the
central write lock.  Identical to the original series.

**Note**: This patch only touches `mm/mmap.c`.  The shard-state reset that
fixes the earlier inode-reuse issue was added in Patch 4 and conditionalized in
Patch 19, not in this commit.

#### Findings
- No new correctness issues.  Allocation failure resets contention to 0,
  which throttles retry under memory pressure rather than permanently
  disabling installation.

#### Verdict
LGTM

---

### Patch 19: mm: control i_mmap optimizations from the command line
**Commit**: `f3269756da3d`
**Author**: Dong Chenchen
**Files Changed**: `Documentation/admin-guide/kernel-parameters.txt`,
`arch/arm64/configs/openeuler_defconfig`,
`arch/x86/configs/openeuler_defconfig`, `fs/inode.c`, `include/linux/fs.h`,
`kernel/fork.c`, `mm/Kconfig`, `mm/internal.h`, `mm/mmap.c`

#### Analysis
Adds the `i_mmap_opt=<bool>` boot parameter and wraps all shard/batch fast
paths with the `i_mmap_opt_enabled()` static key.  When disabled, the code
falls through to the original central-lock paths.  This patch also makes the
shard-field reset in `inode_init_always()` conditional on
`i_mmap_opt_enabled()`.

#### Findings
- `static_branch_disable()` on an already-false static key is safe and gives
  sensible last-value behavior if the parameter appears multiple times.
- The conditional reset is safe because there are no readers of the dynamic
  shard fields when `i_mmap_opt_enabled()` is false.

#### Verdict
LGTM

---

### Patch 20: riscv: entry: Convert ret_from_fork() to C
**Commit**: `9d30f8d721f6`
**Author**: Charlie Jenkins
**Files Changed**: `arch/riscv/kernel/entry.S`, `arch/riscv/kernel/process.c`

#### Findings
- No new issues.

#### Verdict
LGTM

---

### Patch 21: riscv: entry: Split ret_from_fork() into user and kernel
**Commit**: `0d28753d2bdd`
**Author**: Charlie Jenkins
**Files Changed**: `arch/riscv/kernel/entry.S`, `arch/riscv/kernel/process.c`

#### Findings
- No new issues.

#### Verdict
LGTM

---

### Patch 22: LoongArch: entry: Migrate ret_from_fork() to C
**Commit**: `782a4d3939dd`
**Author**: Charlie Jenkins
**Files Changed**: `arch/loongarch/kernel/entry.S`, `arch/loongarch/kernel/process.c`

#### Findings
- No new issues.

#### Verdict
LGTM

---

### Patch 23: entry: Inline syscall_exit_to_user_mode()
**Commit**: `105b25e09af1`
**Author**: Charlie Jenkins
**Files Changed**: `include/linux/entry-common.h`, `kernel/entry/common.c`

#### Findings
- No new issues.

#### Verdict
LGTM

---

### Patch 24: fs: optimize acl_permission_check()
**Commit**: `5b6a56fe539f`
**Author**: Linus Torvalds
**Files Changed**: `fs/namei.c`

#### Findings
- The `mask & 7` optimization is documented in the source comment.

#### Verdict
LGTM

---

### Patch 25: fs: speed up path lookup with cheaper handling of MAY_EXEC
**Commit**: `b4c60fb80ee6`
**Author**: Mateusz Guzik
**Files Changed**: `fs/namei.c`, `include/linux/fs.h`

#### Analysis
Adds `IOP_FASTPERM_MAY_EXEC` opt-in flag and a helper for faster exec
permission checks.  `lookup_inode_permission_may_exec()` is static to
`fs/namei.c` and only called from `may_lookup()`, where the inode is
guaranteed to be a directory.

#### 🔵 Low / Performance-scope note
1. **`IOP_FASTPERM_MAY_EXEC` flag is not set by any in-tree filesystem**
   - **Location**: `include/linux/fs.h:796-802`, `fs/namei.c:600-608`
   - **Description**: No in-tree filesystem explicitly sets
     `IOP_FASTPERM_MAY_EXEC`, so the opt-in route for filesystems with custom
     `->permission` hooks is currently unused.  Filesystems without a custom
     hook still benefit via `do_inode_permission()` and the no-ACL cache path.
     The commit message already notes that the quoted ext4 benchmark benefit
     depends on a companion patch that is not in this series.
   - **Impact**: Performance-scope clarification only; not a correctness bug.
   - **Fix**: Ensure cover letter / commit message accurately describes the
     current coverage and dependency on the companion patch.

#### Verdict
LGTM

---

### Patch 26: LoongArch: entry: Fix include order
**Commit**: `8b936c9b6503`
**Author**: Charlie Jenkins
**Files Changed**: `arch/loongarch/kernel/process.c`

#### Analysis
Reorders introduced include headers to keep alphabetical order.  Upstream
backport.

#### Findings
- No new issues.

#### Verdict
LGTM

---

## Overall Assessment

### Strengths
- The two blocking issues from the previous review are directly and correctly
  fixed.
- The sharding feature remains opt-in at build time and boot time, preserving
  default behavior.
- All write paths route through a single helper that selects the correct root
  and shard.
- All-shard read iteration is atomic with respect to callbacks, preventing
  partial side effects.
- The architecture entry and VFS changes are well-isolated and traceable to
  upstream commits.

### Concerns (all maintenance or measurement, not blocking)
- The conditional shard-state reset in `inode_init_always()` is correct under
  the current boot-only static-key model but would be more robust if made
  unconditional inside the `#ifdef CONFIG_I_MMAP_SHARDS` block.
- Several subtle invariants (same mapping for `vp->insert`, dcache lock on all
  shard mutations, manual `i_mmap_nr_vmas` accounting) are not asserted.
- The file-THP split path re-serializes all shard writers; this is necessary for
  `TTU_RMAP_LOCKED` correctness but should be measured if performance claims are
  made.
- The VFS `IOP_FASTPERM_MAY_EXEC` optimization is currently unused by any
  in-tree filesystem with a custom `->permission` hook.

### Recommendations
1. **(Optional follow-up)** Make the shard-field reset in `inode_init_always()`
   unconditional inside the `#ifdef CONFIG_I_MMAP_SHARDS` block.
2. Add `BUILD_BUG_ON(!is_power_of_2(I_MMAP_SHARDS_PER_DOMAIN))` to guard the
   `hash_ptr()` shift.
3. Add `VM_WARN_ON_ONCE` or lockdep assertions for the `i_mmap_nr_vmas`,
   `vp->insert` mapping, and dcache-lock coverage invariants.
4. Run a lockdep-enabled stress test with `i_mmap_opt=on`, exercising fork,
   exit, mmap, munmap, rmap, and huge-page split in a loop.
5. Verify per-commit source equivalence with `CONFIG_I_MMAP_SHARDS=n`.
6. Clarify VFS performance claims and include the companion ext4 patch if
   the full quoted microbenchmark benefit is required.

### Top 3 Items
1. **Conditional shard-state reset**
   - Severity: 🔵 Low / Maintenance note
   - Location: `fs/inode.c:208-213`
   - Correct today because the static key is boot-only; unconditional reset
     would be more defensive against future readers.

2. **Unasserted invariants in shard-tree mutation**
   - Severity: 🔵 Low / Maintenance note
   - Location: `mm/mmap.c`, `kernel/fork.c`
   - `i_mmap_nr_vmas`, `vp->insert` mapping/shard consistency, and dcache-lock
     coverage are all correct in the current tree but not enforced by helpers
     or assertions.

3. **File-THP split all-shard serialization**
   - Severity: 🔵 Low / Trade-off note
   - Location: `mm/huge_memory.c:3691-3791`, `mm/mmap.c:391-422`
   - Necessary for `TTU_RMAP_LOCKED` correctness; quantify with benchmarks
     before claiming a latency improvement.

---

*Report generated by Sashiko Agent for OpenCode (v2 based on review-ana.md)*
