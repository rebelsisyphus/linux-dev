# mmap_batch v6

Base: `9b87fdc9af2fbfcdb5c24a64139685ef80f6573f`
Head: `1796d45180f56c5634cccbbed54869eb8a21cb86`
Branch: `develop/mmap_batch`

Apply patches 0001 and 0002 in the order listed in `series`.
Patch 1 combines the shared copy predicate and batching implementation;
patch 2 adds the fork/rmap tests. `range-diff-v5.txt` compares this series with v5.
No messages have been sent.

The v6 kernel change only moves vma_needs_copy() into the file batch flush
condition: `if (file && vma_needs_copy(tmp, mpnt))`. copy_page_range() retains
its baseline call condition and performs its own early-return check.
The two final() sites and the upstream selftest remain unchanged from v5.

Both patches pass strict checkpatch; sequential application reproduces HEAD.
HEAD is exactly the commit used to build the QEMU-tested kernel.

QEMU run `20260921-214333-225e6df8` passed and the guest has stopped:
fork/rmap 10/10, guard 5 passed and 4 skipped, two 32-iteration ENOMEM runs,
and SysV SHM data/attachment checks passed. All 202 file VMA calls requiring
page-table copying followed completed insertion. The 838 early-return calls
preceding insertion were identified separately; no ordering violations or
trace overwrite occurred. The project diagnostic uses build-specific flags
and VMA offsets, and is not included in the upstream series.

Validation: `../../dev/validation.md`, `../../dev/review-v6/results.json`,
and `../../dev/review-v6/series-validation.json`.
Flush-site rationale: `../../design/overview.md`.
Discussion comparison: `../../design/lore-comparison.md`.
Original prototype: `../mm_batch_file_vma_insert_v1.patch`.
