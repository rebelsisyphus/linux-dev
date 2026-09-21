# mmap_batch v3

Base: `9b87fdc9af2fbfcdb5c24a64139685ef80f6573f`
Head: `82f5a83137816658e316347e75c4378df47d4586`
Branch: `develop/mmap_batch`

Apply patches 0001 and 0002 in the order listed in `series`.
Patch 1 combines the shared copy predicate and the batching implementation;
patch 2 adds the fork/rmap tests. `range-diff-v2.txt` compares this series with v2.
No messages have been sent.

Both patches pass strict checkpatch and sequential application reproduces HEAD.
The kernel sources match the fully built and QEMU-tested image. The updated
rmap test was rebuilt separately and its binary hash matches the guest copy.

The QEMU run passed the fork/rmap, guard and failure-cleanup checks. The ordering
trace checks completed insertion before all 202 observed file VMA page-table
copy calls across 30 forks. The guest has stopped.

Validation: `../../dev/validation.md`, `../../dev/review-v3/results.json`,
and `../../dev/review-v3/series-validation.json`.
Discussion comparison: `../../design/lore-comparison.md`.
Original prototype: `../mm_batch_file_vma_insert_v1.patch`.
