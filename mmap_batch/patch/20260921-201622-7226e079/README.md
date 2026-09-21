# mmap_batch v4

Base: `9b87fdc9af2fbfcdb5c24a64139685ef80f6573f`
Head: `28a841781404c164c362c1d4f48cda907d4d939e`
Branch: `develop/mmap_batch`

Apply patches 0001 and 0002 in the order listed in `series`.
Patch 1 combines the shared copy predicate and batching implementation;
patch 2 adds the fork/rmap tests. `range-diff-v3.txt` compares this series with v3.
No messages have been sent.

This revision removes the pre-open flush and merges the normal and error
loop-exit flushes. Three explicit flush sites remain: after queueing a VMA
with an open callback, before page-table copying, and at the common loop exit.
arch_dup_mmap() is called only after successful VMA iteration.

Both patches pass strict checkpatch; sequential application reproduces HEAD.
The complete source tree matches the built and QEMU-tested image. Commit
messages were rewrapped after the build; no source changed afterward.

QEMU run `20260921-201731-81592202` passed and the guest has stopped:
fork/rmap 10/10, guard 5 passed and 4 skipped, 32 forced fork failure cleanups.
Across 30 traced forks, all 202 observed file VMA page-table copies followed
completed insertion. Full and failure batches were observed as expected.

Validation: `../../dev/validation.md`, `../../dev/review-v4/results.json`,
and `../../dev/review-v4/series-validation.json`.
Flush-site rationale: `../../design/overview.md`.
Discussion comparison: `../../design/lore-comparison.md`.
Original prototype: `../mm_batch_file_vma_insert_v1.patch`.
