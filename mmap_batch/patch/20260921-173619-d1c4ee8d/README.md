# mmap_batch v2

Base: `9b87fdc9af2fbfcdb5c24a64139685ef80f6573f`
Head: `8c1352822173dc6489f03de81c252f433e6aa7eb`
Branch: `develop/mmap_batch`

Apply patches 0001 through 0003 in the order listed in `series`.
The cover letter describes the design, changes from the first local revision,
validation and remaining coverage. No email has been sent.

Compared with v1, the second commit follows the unlink batching pattern in
3577dbb19241 and shares `struct vma_file_batch`. See `range-diff.txt` for the
complete comparison; patches 1 and 3 are unchanged in content.

All patches pass strict checkpatch. Sequential application reproduces HEAD,
whose tree matches the fully built and QEMU-tested tree. The test guest stopped
successfully after passing the fork/rmap, guard and failure-cleanup cases.

Evidence: `../../dev/style-v2/series-validation.json`,
`../../dev/style-v2/results.json`, and `../../dev/validation.md`.
Original prototype: `../mm_batch_file_vma_insert_v1.patch`.
