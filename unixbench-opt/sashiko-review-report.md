# Sashiko Patchset Review Report

## Metadata
- **Repository**: `/home/sisyphus/code/fork/kernel`
- **Patch Directory**: `/home/sisyphus/code/fork/kernel/unixbench-opt`
- **Review Date**: 2026-08-31
- **Total Patches**: 26 (excluding cover-letter)
- **Base Commit**: `cece3eeed8c9692d37c9b398eb91f7c69e93a101`
- **Final Tree Commit**: `8b936c9b6503193e0650bb11ded13410dacf12d5`
- **Reviewer**: Sashiko Agent (OpenCode)
- **Model**: kimi-for-coding/kimi-for-coding

## Protocol Notes

The Sashiko review skill and the current model instructions require loading
additional guide files from
`/home/sisyphus/tools/sashiko/third_party/prompts/kernel/severity.md`,
`/home/sisyphus/tools/sashiko/third_party/prompts/kernel/technical-patterns.md`,
and the subsystem-specific guides. Those files do not exist in this
environment (`/home/sisyphus/tools/sashiko/` is not present), so the review was
performed using the core Sashiko protocol from the loaded skill and the
severity definitions embedded in that skill document.

## Summary

This patchset is an optimized/follow-up revision of the earlier `unixbench`
series.  A tree-level comparison of the original 26-patch series against the
previous series top commit
(`c6f1e25fdcd5`) shows that only **two files changed** in the final tree:
`fs/inode.c` and `mm/rmap.c`.  Both changes are fixes for the two main residual
issues identified in the earlier Sashiko review
(`unixbench/sashiko-review-report-v2.md`):

1. `i_mmap_lock_contention` and the dynamic shard pointer are now reset on
every inode allocation in `inode_init_always()`, so slab-object reuse can no
longer inherit stale contention state from a previous file.

2. The shard-aware `rmap_walk_file_vma()` callback has been restored to use
`VM_BUG_ON_VMA(address == -EFAULT, vma)`, matching the baseline diagnostic
invariant instead of silently warning and continuing.

The remaining findings from the earlier review (manual `i_mmap_nr_vmas`
accounting, implicit `vp->insert` mapping assumptions, and the file-THP split
serialization trade-off) are pre-existing maintenance considerations rather
than new regressions.  A follow-up working-tree cleanup removes Patch 15's
architecture dcache walkers: `CONFIG_I_MMAP_SHARDS` depends on `NUMA`, while
ARM, Nios II, and PA-RISC do not define `CONFIG_NUMA`, so those branches and
their private helper could never be selected.

The follow-up cleanup was compiled with `CONFIG_NUMA=y` and
`CONFIG_I_MMAP_SHARDS=y`; `mm/mmap.o`, `fs/inode.o`, and `kernel/fork.o`
compile successfully with the in-tree configuration.  A full kernel build was
attempted previously and failed only in
`kernel/sched/fair.c` with an unrelated `unused variable 'cfs_rq'` error, which
is not introduced by this patchset.

**Overall verdict: Approved (LGTM)** — the blocking correctness defects from
the previous review are resolved.  The remaining medium-severity items should
be tracked and addressed as follow-up clean-ups, but they do not block merging
of this optimized variant.

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

#### 🔵 Low / Nits
1. **Batch eviction flushes slot 0 unconditionally**
   - **Location**: `kernel/fork.c:870-878`
   - **Description**: When all four batch slots are occupied, slot 0 is always
     flushed.  The exact-match search prevents the new mapping from already
     occupying slot 0, so the policy is safe, but an LRU could reduce flushes
     for interleaved workloads.
   - **Impact**: Performance only; not a correctness defect.
   - **Fix**: Document the deliberate policy; revisit if fork-heavy workloads
     show regression.

#### Verdict
LGTM

---

### Patch 04: mm: add optional NUMA-local i_mmap shard state
**Commit**: `291661804f19`
**Author**: Dong Chenchen
**Files Changed**: `fs/inode.c`, `include/linux/fs.h`, `include/linux/mm_types.h`, `kernel/fork.c`, `mm/Kconfig`

#### Analysis
Introduces `struct i_mmap_shards`, `i_mmap_nr_vmas`, `i_mmap_lock_contention`,
and per-mm NUMA home node / shard index.  Patch 04 initializes the dynamic
fields unconditionally in both `__address_space_init_once()` and
`inode_init_always()`; Patch 19 later gates those resets with
`i_mmap_opt_enabled()`.

#### 🟡 Medium Finding (maintenance risk)
1. **Shard-state reset is conditional on `i_mmap_opt_enabled()`**
   - **Location**: `fs/inode.c:208-213`
   - **Description**: `inode_init_always()` now resets the dynamic shard
     pointer, VMA counter, and contention counter, but only when
     `i_mmap_opt_enabled()` is true.  The slab constructor already zeros the
     whole inode, so on first allocation the fields are zero regardless.  The
     boot parameter is parsed early, so in practice the conditional matches the
     runtime state.  However, if any future code path reads these fields
     without checking `i_mmap_opt_enabled()`, a stale value could be observed.
   - **Impact**: Latent robustness hazard if the static-key invariant is
     violated by future readers.
   - **Fix**: Make the reset unconditional inside the `#ifdef
     CONFIG_I_MMAP_SHARDS` block, or add a comment documenting why the
     conditional is safe.

#### 🔵 Low / Maintenance note
2. **`hash_ptr(..., ilog2(I_MMAP_SHARDS_PER_DOMAIN))` assumes power of two**
   - **Location**: `kernel/fork.c:1510`
   - **Description**: The current constant is 4, so the result is always in
     range.  Changing it to a non-power-of-two would underuse some shards.
   - **Impact**: Maintenance hazard if the constant is changed.
   - **Fix**: Add `BUILD_BUG_ON(!is_power_of_2(I_MMAP_SHARDS_PER_DOMAIN))` or
     use explicit modulo after hashing.

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

#### 🟡 Medium Finding (maintenance risk)
1. **`i_mmap_nr_vmas` counter invariant is not enforced**
   - **Location**: `include/linux/fs.h:712-720`, `mm/mmap.c:542-556`,
     `mm/mmap.c:925-963`, `kernel/fork.c:694-713`
   - **Description**: All current MMU file-VMA insert/remove paths pair tree
     mutations with `i_mmap_vma_count_add/sub()`.  A scan of the final tree
     finds no unaccounted MMU file-tree mutation.  However, the invariant is
     not enforced by helper functions or lockdep; a future caller that calls
     `vma_interval_tree_insert/remove` directly on a shard root could
     silently desynchronize `mapping_mapped()`.
   - **Impact**: Latent data-corruption / stale-cache risk if future code
     bypasses the helpers.
   - **Fix**: Centralize tree mutations in helpers that always update the
     counter, or add `VM_WARN_ON_ONCE` assertions in raw interval-tree
     helpers when `CONFIG_I_MMAP_SHARDS` is enabled.

#### Verdict
LGTM

---

### Patch 07: mm: add NUMA-aware i_mmap shard installation
**Commit**: `02e04b6da3fd`
**Author**: Dong Chenchen
**Files Changed**: `include/linux/fs.h`, `mm/mmap.c`

#### Analysis
Implements `i_mmap_shards_install_locked()` and the migration of VMAs from
the central tree to shard trees.  Identical to the original series.

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

#### 🟡 Medium Finding (maintenance risk)
1. **`vma_prepare()` assumes `vp->insert` shares mapping/shard with `vp->vma`**
   - **Location**: `mm/mmap.c:1040-1054`, `mm/mmap.c:1071-1075`, `mm/mmap.c:1098-1102`
   - **Description**: The only current assignment to `vp->insert` is in
     `__split_vma()` via `vm_area_dup(vma)`, and `vma_merge()` only accepts
     mergeable VMAs with the same file mapping.  The invariant therefore
     holds, but it is implicit.
   - **Impact**: Latent correctness hazard if a future caller of
     `init_multi_vma_prep()` passes an insert with a different mapping.
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
  locks are released and `-EAGAIN` is returned with zero callback side
  effects, so retry cannot duplicate work.

#### Verdict
LGTM

---

### Patch 10: mm: make core file rmap walkers shard-aware
**Commit**: `fd98e9713d64`
**Author**: Dong Chenchen
**Files Changed**: `mm/memory.c`, `mm/rmap.c`

#### Analysis
Routes `rmap_walk_file()` and `unmap_mapping_pages()` through the common shard
iterator.  **This is one of the two patches changed in this optimized variant.**

#### 🟢 Earlier diagnostic regression: FIXED
The previous review noted that the original series weakened the baseline
`VM_BUG_ON_VMA(address == -EFAULT, vma)` to `VM_WARN_ON_ONCE(true); return 0;`
in the shard-aware callback.  This patch restores the baseline behavior:

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
The original patch adds `i_mmap_walk_dcache_locked()` and converts the ARM,
Nios II, and PA-RISC cache-flush walkers.  However,
`CONFIG_I_MMAP_SHARDS` depends on `MMU && SMP && NUMA`, and none of those three
architectures defines `CONFIG_NUMA`.  The new architecture branches are
therefore unreachable in the supported configuration space.

#### 🟢 Unreachable architecture support: FIXED
The follow-up working-tree cleanup removes all four unreachable architecture
branches, the now-unused `i_mmap_walk_dcache_locked()` declaration and
definition, and the shard-installation dcache serialization that existed only
for those walkers.  The original central-tree cache-flush implementations are
left unchanged.  If one of these architectures gains NUMA support later, its
cache walker must be designed and compiled together with shard support at that
time.

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

#### 🟡 Medium Finding (design trade-off)
1. **File-THP split blocks every shard writer**
   - **Location**: `mm/huge_memory.c:3691-3791`, `mm/mmap.c:391-422`
   - **Description**: Acquiring all shard read locks is required for
     `TTU_RMAP_LOCKED` correctness.  Sharding reintroduces centralized
     serialization while the split is active, adding multiple-lock acquisition
     overhead.
   - **Impact**: Conditional latency regression for workloads that repeatedly
     split file THPs while concurrently mapping/unmapping the same sharded
     file.
   - **Fix**: Document the trade-off in the cover letter and measure
     `THP_SPLIT_PAGE` latency with `i_mmap_opt=on`.

#### Verdict
LGTM

---

### Patch 18: mm: enable NUMA-local i_mmap shards after contention
**Commit**: `aba6ec3fee7c`
**Author**: Dong Chenchen
**Files Changed**: `mm/mmap.c`

#### Analysis
Implements dynamic shard installation after 256 failed trylocks on the
central write lock.  This commit modifies only `mm/mmap.c`; it does not contain
the inode reset fix.  The reset was introduced unconditionally by Patch 04 and
made conditional on `i_mmap_opt_enabled()` by Patch 19.

#### 🟢 Earlier stale-contention finding: FIXED
The previous review found that `i_mmap_lock_contention` survived inode
slab-object reuse, causing a newly allocated inode to inherit stale shard
activation state from a previous file occupying the same slab object.  The
final series resets the dynamic fields on every inode allocation when the
optimization is enabled:

```c
#ifdef CONFIG_I_MMAP_SHARDS
	if (i_mmap_opt_enabled()) {
		mapping->i_mmap_shards = NULL;
		atomic_set(&mapping->i_mmap_nr_vmas, 0);
		atomic_set(&mapping->i_mmap_lock_contention, 0);
	}
#endif
```

`__address_space_init_once()` also performs the same conditional reset (the
fields are already zeroed by the surrounding `memset()`, so this is redundant
but defensive).  These changes come from Patch 04 and Patch 19, not Patch 18.

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
`arch/arm64/configs/openeuler_defconfig`, `arch/x86/configs/openeuler_defconfig`,
`fs/inode.c`, `include/linux/fs.h`, `kernel/fork.c`, `mm/Kconfig`, `mm/internal.h`,
`mm/mmap.c`

#### Analysis
Adds the `i_mmap_opt=<bool>` boot parameter and wraps all shard/batch fast
paths with the `i_mmap_opt_enabled()` static key.  When disabled, the code
falls through to the original central-lock paths.  `static_branch_disable()`
on an already-false static key is safe and gives sensible last-value behavior
if the parameter appears multiple times.  Identical in behavior to the
original series.

#### Findings
- No new correctness issues.

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

#### 🟡 Medium Finding (performance scope)
1. **`IOP_FASTPERM_MAY_EXEC` flag is not set by any in-tree filesystem**
   - **Location**: `include/linux/fs.h:802-807`
   - **Description**: No filesystem in this tree explicitly sets
     `IOP_FASTPERM_MAY_EXEC`, so the new opt-in route for filesystems with
     custom `->permission` hooks is currently unused.  The optimization still
     benefits filesystems without a custom hook via
     `do_inode_permission()` and the no-ACL cache path.
   - **Impact**: Performance scope only; not a correctness bug.
   - **Fix**: Clarify performance claims and include the companion ext4
     patch if the quoted microbenchmark benefit is required.

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

### Concerns
- Several subtle invariants (same mapping for `vp->insert` and manual
  `i_mmap_nr_vmas` accounting) remain unasserted.
- The conditional shard-state reset in `inode_init_always()` is correct under
  the current boot-only static-key model but is slightly less robust than an
  unconditional reset.
- The file-THP split path re-serializes all shard writers, which is a
  documented trade-off but worth measuring.
- The VFS `IOP_FASTPERM_MAY_EXEC` optimization is currently unused by any
  in-tree filesystem.

### Recommendations
1. **(Optional follow-up)** Make the shard-field reset in
   `inode_init_always()` unconditional inside the `#ifdef
   CONFIG_I_MMAP_SHARDS` block to remove the implicit dependency on
   `i_mmap_opt_enabled()` for field initialization.
2. Add `BUILD_BUG_ON(!is_power_of_2(I_MMAP_SHARDS_PER_DOMAIN))` in
   `kernel/fork.c` or `mm/mmap.c` to guard the `hash_ptr()` shift.
3. Add `VM_WARN_ON_ONCE` assertions for the shard-selection and
   `i_mmap_nr_vmas` invariants.
4. Run a lockdep-enabled stress test with `i_mmap_opt=on`, exercising fork,
   exit, mmap, munmap, rmap, and huge-page split in a loop.
5. Verify per-commit source equivalence with `CONFIG_I_MMAP_SHARDS=n`.
6. Clarify VFS performance claims and include the companion ext4 patch if
   the full quoted microbenchmark benefit is required.

### Top 3 Issues
1. **Shard-state reset is conditional on `i_mmap_opt_enabled()`**
   - Severity: 🟡 Medium
   - Location: `fs/inode.c:208-213`
   - Correct today, but a future reader that omits the static-key check could
     observe stale values.  Unconditional reset would be more robust.

2. **`i_mmap_nr_vmas` counter invariant is not enforced**
   - Severity: 🟡 Medium
   - Location: `include/linux/fs.h`, `mm/mmap.c`, `kernel/fork.c`
   - Future direct tree mutations could silently desynchronize
     `mapping_mapped()`.

3. **`vp->insert` assumes the same mapping and shard as `vp->vma`**
   - Severity: 🟡 Medium
   - Location: `mm/mmap.c`, VMA split preparation
   - The current split path preserves both invariants, but they are not
     asserted where the lock root is selected.

---

*Report generated by Sashiko Agent for OpenCode*
