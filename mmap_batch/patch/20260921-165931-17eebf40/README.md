# mmap_batch adapted draft

Source: user-supplied ../mm_batch_file_vma_insert_v1.patch by Dong Chenchen.

This archive adapts the fork file-VMA insertion batching to the registered
project base. It uses the shared vma_needs_copy predicate, preserves current
shared-writable accounting, flushes around open callbacks and fork teardown,
and extends the existing rmap selftest with ten file-fork cases.

No Git commits were created. uncommitted.patch contains the complete source
and selftest diff against the base recorded in manifest.json. Project-only
benchmarks, fault-path diagnostics and logs live under ../../test and ../../dev.
See ../../dev/progress.md and ../../design/overview.md for validation and limits.
