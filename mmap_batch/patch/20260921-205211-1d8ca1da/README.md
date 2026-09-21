# mmap_batch v5

Base: `9b87fdc9af2fbfcdb5c24a64139685ef80f6573f`
Head: `c3bdfa308189f4c91f0dff8f93373b9c4183a9f3`
Branch: `develop/mmap_batch`

Apply patches 0001 and 0002 in the order listed in `series`.
Patch 1 combines the shared copy predicate and batching implementation;
patch 2 adds the fork/rmap tests. `range-diff-v4.txt` compares this series with v4.
No messages have been sent.

Two explicit final() calls remain: before copying a VMA with a file, and
at loop_out to drain the remaining batch on success or error. open callbacks
and arch_dup_mmap() retain their baseline placement; callback presence alone
no longer forces insertion. Copying a VMA without a file keeps pending file
VMAs available for batching. Capacity and mapping changes still flush in add().

Both patches pass strict checkpatch; sequential application reproduces HEAD.
HEAD is exactly the commit used to build the QEMU-tested kernel.

QEMU run `20260921-205137-096ef079` passed and the guest has stopped:
fork/rmap 10/10, guard 5 passed and 4 skipped, 32 forced fork failure cleanups.
Across 30 traced forks, all 202 observed file VMA page-table copies followed
completed insertion. SysV SHM data and attachment lifetime checks passed;
the callback diagnostic recorded 33 shm_open() calls and four full batches.
The SysV diagnostic is a project artifact, not part of the upstream series.

Validation: `../../dev/validation.md`, `../../dev/review-v5/results.json`,
and `../../dev/review-v5/series-validation.json`.
Flush-site rationale: `../../design/overview.md`.
Discussion comparison: `../../design/lore-comparison.md`.
Original prototype: `../mm_batch_file_vma_insert_v1.patch`.
