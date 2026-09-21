# mmap_batch patch series

Base: `9b87fdc9af2fbfcdb5c24a64139685ef80f6573f`
Head: `08740c0575d5329fb551729bed01474b2b31651d`
Branch: `develop/mmap_batch`

Apply patches 0001 through 0003 in the order listed in `series`. The cover
letter summarizes the design, validation and outstanding coverage. Email
headers are prepared; no messages have been sent.

Each patch passes `scripts/checkpatch.pl --strict` with no errors, warnings
or checks. Sequential application to the base reproduces the committed tree.
The changed source hashes match the previously built and QEMU-tested sources.

Validation: `../../dev/validation.md` and `../../dev/series-validation.json`.
Original prototype: `../mm_batch_file_vma_insert_v1.patch`.

The manifest records the development checkpoint. `uncommitted.patch` is empty;
all four changed source files are included in the three commits.
