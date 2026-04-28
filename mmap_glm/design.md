# Sharded i_mmap: Design Document

## 1. Problem Statement

On large multi-core systems (320 cores, 4 NUMA nodes), the execl benchmark suffers severe
lock contention on `mapping->i_mmap_rwsem`. When concurrent processes fork/exec/exit,
each must insert/remove VMAs into/from the file's i_mmap interval tree under this single
rwsem. For libc.so with 6000+ VMAs, this creates a scalability ceiling.

Existing batch optimizations (commit 3577dbb19241) reduce lock acquire count but don't
address the fundamental problem: a single global lock for all operations on the same file.

Community attempts:
- NUMA-level immap trees (Hygon): Split tree only, lock stays global - limited benefit
- Skip rmap for .so files: Rejected due to security/semantic issues
- Mateusz Guzik's suggestion: Shard both tree AND lock per ~8 CPUs, with dynamic conversion

## 2. Architecture

```
                    ┌─────────────────────────────────────────────┐
                    │           struct address_space              │
                    ├─────────────────────────────────────────────┤
                    │  i_mmap          : rb_root_cached (central) │← legacy path
                    │  i_mmap_shards_ptr : *i_mmap_shards (shard)│← sharded path
                    │  i_mmap_sharded : bool                     │← mode flag
                    │  i_mmap_rwsem    : rw_semaphore            │← transition lock
                    └─────────────────────────────────────────────┘
                                          │
                          ┌───────────────┴───────────────┐
                          │   i_mmap_sharded == false?     │
                          └───────────────┬───────────────┘
                                  ┌───────┴───────┐
                              YES │               │ NO
                          ┌───────┴──┐     ┌──────┴──────────────────────┐
                          │ Central  │     │        Sharded              │
                          │          │     │                              │
                          │ i_mmap   │     │  i_mmap_shards_ptr           │
                          │ +rwsem   │     │  ┌─────────────────────┐    │
                          │          │     │  │ struct i_mmap_shards │    │
                          │ VMA ops  │     │  │  nr_shards    = 8    │    │
                          │ hold     │     │  │  vma_count   (atomic)│    │
                          │ single   │     │  │  trylock_fails(at.) │    │
                          │ rwsem    │     │  │  shards[]:           │    │
                          │          │     │  │  ┌────┐ ┌────┐      │    │
                          └──────────┘     │  │  │ S0 │ │ S1 │ ...  │    │
                                           │  │  │tree│ │tree│      │    │
                                           │  │  │rsem│ │rsem│      │    │
                                           │  │  └────┘ └────┘      │    │
                                           │  └─────────────────────┘    │
                                           │                              │
                                           │  VMA ops hold per-shard      │
                                           │  rwsem(s) only               │
                                           └──────────────────────────────┘

  Dynamic Conversion (one-way: central → sharded):

          trylock(i_mmap_rwsem) fails      trylock_fails >= 8
          ┌──────────────────┐         ┌─────────────────────────┐
          │ alloc i_mmap_    │         │ acquire i_mmap_rwsem(W) │
          │ shards (GFP_    │  OR     │ walk i_mmap tree         │
          │ ATOMIC), set    │────────→│ move each VMA to shard   │
          │ i_mmap_shards_  │         │ set i_mmap_sharded=true   │
          │ ptr             │         │ release i_mmap_rwsem      │
          └──────────────────┘         └─────────────────────────┘
```

## 3. Data Structures

```c
#define I_MMAP_SHARD_SHIFT   3
#define I_MMAP_NR_SHARDS     (1 << I_MMAP_SHARD_SHIFT)  /* 8 */
#define I_MMAP_SHARD_THRESHOLD 8  /* trylock fails before convert */

struct i_mmap_shard {
    struct rb_root_cached     tree;       /* per-shard interval tree */
    struct rw_semaphore       rwsem;      /* per-shard lock (no-track for lockdep) */
};

struct i_mmap_shards {
    unsigned int              nr_shards;       /* = I_MMAP_NR_SHARDS */
    atomic_t                 vma_count;        /* total VMA count for mapping_mapped() */
    atomic_t                 trylock_fails;    /* contention counter (atomic for SMP safety) */
    struct i_mmap_shard      shards[];         /* flexible array of shards */
};

/* In struct address_space: */
struct rb_root_cached     i_mmap;              /* centralized tree (always valid) */
struct i_mmap_shards    *i_mmap_shards_ptr;    /* NULL=centralized, non-NULL=sharded */
bool                     i_mmap_sharded;       /* mode flag, set via WRITE_ONCE */
struct rw_semaphore      i_mmap_rwsem;         /* centralized lock / transition lock */
```

**Key design decisions:**

- `i_mmap` and `i_mmap_shards_ptr` are **separate fields** (not a union). The union
  approach caused `mapping_sharded()` to misinterpret `i_mmap.rb_root.rb_node` as
  a non-NULL `i_mmap_shards_ptr`, triggering crashes in early VMA operations.
- `i_mmap_sharded` is a dedicated bool flag, set only via `WRITE_ONCE`/`READ_ONCE`
  after the shard pointer is published with `smp_store_release`.
- Shard rwsems use `__lockdep_no_track__` to avoid lockdep class exhaustion (8 rwsems
  per mapping × thousands of mappings would exhaust the 8192-class limit).

## 4. Shard Assignment

```c
static inline unsigned long vma_shard_idx(struct vm_area_struct *vma)
{
    return (((unsigned long)vma >> L1_CACHE_SHIFT) * 0x9E370001UL)
        >> (32 - I_MMAP_SHARD_SHIFT);
}
```

Uses golden-ratio hash of the VMA pointer address (right-shifted by `L1_CACHE_SHIFT`
to ignore low bits which may be zero due to alignment). The result identifies which
of the 8 shards a VMA belongs to.

Properties:
- Same VMA always maps to the same shard (insert/remove consistency)
- Child VMAs from `vm_area_dup` have different addresses → different shards (reduced contention)
- Hash dispersion across shards is near-uniform for typical VMA address distributions

## 5. Operation Paths

### 5.1 Write Path (Insert/Remove VMA)

**Centralized mode** — identical to upstream:
```
i_mmap_lock_write(mapping)
  vma_interval_tree_insert/remove(vma, &mapping->i_mmap)
i_mmap_unlock_write(mapping)
```

**Sharded mode** — per-shard lock only:
```
shard = vma_i_mmap_shard(vma, mapping)
down_write(&shard->rwsem)
  vma_interval_tree_insert/remove(vma, &shard->tree)
  atomic_inc/dec(&shards_ptr->vma_count)
up_write(&shard->rwsem)
```

For operations touching multiple VMAs (split, merge, munmap), all affected shards
are locked in **ascending index order** to prevent deadlocks:

```c
static void vma_prepare(struct vma_prepare *vp)
{
    if (mapping_sharded(vp->mapping)) {
        /* lock all relevant shards in ascending order */
        vp->nr_locked_shards = i_mmap_lock_shards_write(
            vp->mapping, vp->locked_shards, lock_vmas, 5);
        vp->locked_sharded = true;  /* remember which lock type we took */
    } else {
        i_mmap_lock_write(vp->mapping);
        vp->locked_sharded = false;
    }
}

static void vma_complete(struct vma_prepare *vp, ...)
{
    if (vp->locked_sharded)
        i_mmap_unlock_shards_write(vp->locked_shards, vp->nr_locked_shards);
    else {
        i_mmap_unlock_write(vp->mapping);
        mapping_maybe_convert_to_shards(vp->mapping);
    }
}
```

**Critical fix**: `vp->locked_sharded` records which lock type was acquired in
`vma_prepare()`, because `mapping_sharded()` can change between prepare and complete
if a concurrent thread triggers conversion during `i_mmap_unlock_write` →

### 5.2 Read Path (Iteration / rmap walk)

**Centralized mode** — identical to upstream.

**Sharded mode** — iterate each shard independently:
```c
for (s = 0; s < shards->nr_shards; s++) {
    down_read(&shards->shards[s].rwsem);
    vma_interval_tree_foreach(vma, &shards->shards[s].tree, start, last) {
        /* process vma */
        if (done) { up_read(...); goto out; }
    }
    up_read(&shards->shards[s].rwsem);
}
```

Each shard lock is acquired/released independently, allowing concurrent writes
to other shards during the walk.

### 5.3 mapping_mapped() Fast Check

```c
static inline bool mapping_mapped(const struct address_space *mapping)
{
    struct i_mmap_shards *shards = READ_ONCE(mapping->i_mmap_shards_ptr);
    if (shards)
        return atomic_read(&shards->vma_count) > 0;
    return !RB_EMPTY_ROOT(&mapping->i_mmap.rb_root);
}
```

In sharded mode, `atomic_t vma_count` provides O(1) empty check without acquiring
any lock.

### 5.4 dup_mmap (fork) Batch Path

The batch insertion path in `dup_mmap` collects VMAs for the same mapping and
inserts them in one critical section. In sharded mode:

1. Determine all unique shards needed by the batch
2. Sort shard indices in ascending order
3. Acquire all shard write locks
4. Insert VMAs into their respective shard trees
5. Release all shard write locks

The batch unlock path correctly handles the case where the mapping was sharded
*before* the batch started — it only calls `i_mmap_unlock_write()` when NOT sharded,
preventing a lock imbalance.

### 5.5 vma_link_file Single-VMA Path

For single VMA insertions (mmap without batch):
```c
if (mapping_sharded(mapping)) {
    shard = vma_i_mmap_shard(vma, mapping);
    down_write(&shard->rwsem);
    __vma_link_file(vma, mapping);
    /* optionally return held shard for caller to release later */
} else {
    /* trylock first; on fail, take write lock then convert to sharded */
    i_mmap_lock_write(mapping);
    __vma_link_file(vma, mapping);
    i_mmap_unlock_write(mapping);
    mapping_maybe_convert_to_shards(mapping);
}
```

## 6. Dynamic Conversion

### 6.1 Trigger Conditions

Two paths trigger centralized → sharded conversion:

1. **VMA insertion contention**: When `vma_link_file` fails the trylock on
   `i_mmap_rwsem`, a `struct i_mmap_shards` is allocated and the VMA count
   on the shard pre-structure begins. After `I_MMAP_SHARD_THRESHOLD` (8)
   trylock failures, full conversion is triggered.

2. **Post-write-lock release**: After `vma_complete` releases `i_mmap_rwsem`
   in centralized mode, `mapping_maybe_convert_to_shards()` checks whether
   the pre-allocated shard structure has accumulated enough trylock failures.

### 6.2 Conversion Mechanism

```c
void mapping_convert_to_shards_locked(mapping, shards)
{
    /* Held under i_mmap_rwsem for write */

    /* Double-check: another thread may have already converted */
    if (mapping_sharded(mapping)) {
        i_mmap_shards_free(shards);  /* discard our allocation */
        return;
    }

    /* Move each VMA from centralized tree to shard tree */
    while ((vma = vma_interval_tree_iter_first(old_root, 0, ULONG_MAX))) {
        vma_interval_tree_remove(vma, old_root);
        shard = &shards->shards[vma_shard_idx(vma) % shards->nr_shards];
        vma_interval_tree_insert(vma, &shard->tree);
        atomic_inc(&shards->vma_count);
    }

    /* Publish: release semantics ensure all tree updates visible */
    smp_store_release(&mapping->i_mmap_shards_ptr, shards);
    WRITE_ONCE(mapping->i_mmap_sharded, true);
}
```

**One-way transition**: Once `i_mmap_sharded` is true, it never goes back to false.
The `i_mmap` centralized tree is empty after conversion (all VMAs moved to shards).
`i_mmap_rwsem` continues to protect `i_mmap_writable` and mode transitions.

### 6.3 Concurrency Safety

- **No duplicate creation**: `mapping_convert_to_shards` holds `i_mmap_rwsem` for
  write, and `mapping_convert_to_shards_locked` double-checks `mapping_sharded()`.
  The `i_mmap_shards_ptr` allocation in `mapping_maybe_convert_to_shards` also
  checks `!mapping->i_mmap_shards_ptr` under the write lock.
- **`trylock_fails` is atomic**: Uses `atomic_inc_return()` to prevent lost
  increments from concurrent CPUs.
- **Readers during conversion**: If a reader sees `i_mmap_sharded == false`,
  it uses the centralized path (protected by `i_mmap_rwsem` read side). After
  conversion publishes the shard pointer with `smp_store_release`, new readers
  see `i_mmap_sharded == true` and use sharded path. No reader can see a
  half-converted state.

## 7. Lockdep Considerations

Shard rwsems are initialized with `__lockdep_no_track__`, which causes
`__lock_acquire()` to return immediately. This avoids lockdep class exhaustion
(shard rwsems are self-managed under `i_mmap_rwsem` and don't need lockdep's
full dependency tracking).

The centralized `i_mmap_rwsem` retains normal lockdep tracking.

## 8. Commit History

| Commit | Description |
|--------|-------------|
| `afc01cab` | mm: introduce get_i_mmap_root() to abstract i_mmap access |
| `93f2035d` | mm: introduce sharded i_mmap infrastructure (data structures, helpers) |
| `391af59f` | mm: convert VMA insert/remove to use per-shard locks |
| `559b5aa2` | mm: convert i_mmap iteration to handle sharded mode (10 files) |
| `1a54e305` | mm: add dynamic sharding with contention detection |
| `f4e8690f` | mm: fix sharded i_mmap runtime bugs (3 fixes, see below) |
| `f090a539` | mm: use atomic_t for trylock_fails to fix concurrency race |

### Bug Fixes in `f4e8690f`

1. **Shard tree corruption (page fault at 0x100000000)**: `mapping_convert_to_shards()`
   used stale iterator — `vma_interval_tree_iter_next()` was called before removal,
   causing `rb_erase` on nodes already moved to shard trees. Fixed by re-querying
   the tree with `vma_interval_tree_iter_first()` after each removal.

2. **Lockdep crash (`__lock_acquire` page fault)**: Two sub-issues:
   - **Union overlay**: `i_mmap` and `i_mmap_shards_ptr` shared a union, causing
     `mapping_sharded()` to misinterpret `i_mmap.rb_root.rb_node` as a non-NULL
     shards pointer. Replaced union with separate fields + explicit `i_mmap_sharded` bool.
   - **Lockdep exhaustion**: Each shard rwsem consumed a lockdep class. Used
     `__lockdep_no_track__` to skip tracking (shard locks are managed under `i_mmap_rwsem`).

3. **Lock imbalance (`i_mmap_rwsem` held returning to userspace)**: `vma_complete()`
   re-checked `mapping_sharded()` to determine which lock to release, but the mapping
   could have transitioned between `vma_prepare()` and `vma_complete()`. Added
   `locked_sharded` flag to `struct vma_prepare` to record lock type at acquire time.
   Also fixed the same issue in `dup_mmap_file_batch_unlock()`.

### `f090a539`

- **`trylock_fails` race**: Changed from `unsigned int` to `atomic_t`, using
  `atomic_inc_return()` to prevent lost increments under concurrent access.

## 9. Modified Files

| File | Changes |
|------|---------|
| `include/linux/fs.h` | Data structures, inline helpers, `i_mmap_sharded` flag |
| `mm/i_mmap_shards.c` | New: shard alloc/free, conversion, lock helpers |
| `mm/vma.c` | `vma_prepare`/`vma_complete` shard paths, `sharded_remove_vma_from_tree` |
| `mm/vma.h` | `struct vma_prepare` (`locked_sharded`, `locked_shards` fields) |
| `mm/mmap.c` | `dup_mmap` batch shard handling, batch unlock fix |
| `mm/rmap.c` | Sharded rmap walk |
| `mm/memory.c` | Sharded `unmap_mapping_range` |
| `mm/pagewalk.c` | Sharded page walk |
| `mm/khugepaged.c` | Sharded khugepaged paths |
| `mm/hugetlb.c` | Sharded hugetlb paths |
| `mm/nommu.c` | Sharded NOMMU paths |
| `mm/memory-failure.c` | Sharded memory failure paths |
| `fs/inode.c` | Shard cleanup on inode destroy |
| `fs/dax.c` | Sharded DAX paths |
| `fs/hugetlbfs/inode.c` | Sharded hugetlbfs paths |
| `kernel/events/uprobes.c` | Sharded uprobes paths |
| `mm/vma_init.c` | No changes needed (vm_area_dup uses data_race memcpy) |

## 10. Expected Performance Impact

On 320-core system with execl benchmark:
- **Write contention**: reduced by ~8× (8 shards with independent locks)
- **Read overhead**: slightly increased (iterate 8 shards vs 1 tree), but rmap walks
  are less frequent than insert/remove in execl workloads
- **Memory overhead**: ~2 KB per address_space that gets sharded (8 shards ×
  (16 bytes rb_root_cached + ~232 bytes rw_semaphore)), only allocated on contention
- **Fast path**: `mapping_mapped()` remains O(1) via `atomic_t vma_count`
- **Centralized mode**: zero overhead for low-contention files (no shard allocation)