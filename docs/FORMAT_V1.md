> Historical format. The current implementation only reads v1 through the explicit
> importer described in [FORMAT.md](FORMAT.md#importing-v1). It never writes v1.

# Store format 1 and durability

The authoritative files remain `vectors.gm`, `chunks.gm`, and `docs.gm`.
Integers use the native little-endian representation on supported 64-bit
macOS/Linux targets. Compile-time assertions fix sizes and important offsets;
big-endian and 32-bit builds are rejected. This is not a general portable C
serialization format. Reserved fields are ignored when loading and zeroed in
new records.

## Data layout

Each file starts with a 32-byte header:

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 4 | Magic `0x314d47ed` |
| 4 | 4 | Version `1` |
| 8 | 4 | Dimension, positive, divisible by 8, at most 65536 |
| 12 | 4 | Record stride |
| 16 | 8 | Record count |
| 24 | 8 | Model/preprocessing identity |

- `vectors.gm`: stride `dimension / 8`, one packed vector per physical chunk.
  Positive finite components set bit `i % 8` of byte `i / 8`; zero and negative
  components clear it. NaN/infinity from inference are rejected before commit.
- `chunks.gm`: stride 16: four uint32 fields `doc`, `ordinal`, `generation`,
  `reserved`. Vectors and chunks have identical counts. A chunk is live only
  when its generation matches the referenced document. Live ordinals start at
  zero and are consecutive within each document.
- `docs.gm`: stride 256: NUL-terminated ID/path in 232 bytes, int64 legacy mtime,
  uint64 legacy size, uint32 generation at offset 248, uint32 reserved.
  New writes zero the legacy fields. IDs are nonempty and unique, at most
  231 bytes. Document IDs are stable array indices, including empty documents.

File sizes must equal header plus count times stride. All headers, references,
generations, strings and live ordinals are checked before exposing a handle.
All three data files must exist, or none may exist when creating a store. A
missing file is never silently recreated without a valid creation journal.
Bounds and configured memory limits are checked before loading record arrays.

Data files have **no checksums**: structural validation cannot detect arbitrary
vector bit flips, valid-looking metadata corruption or malicious forgery.
The journal checksum detects accidental journal damage, not adversarial changes.
Backups remain necessary.

## Model identity and compatibility

Compute SHA-256 over the complete model file, then SHA-256 over that digest,
policy bytes `{2, 0, 1, 64, omit_bos, omit_eos}` and query prefix bytes without
NUL. The policy identifies revision 2, a 256-token window, 64-token overlap
and token wrapping. The first eight digest bytes, interpreted little-endian,
are stored in `model_fp`. Identity thus retains only 64 bits, not a full
cryptographic digest. The model must remain immutable while opening/using it.

Older timestamp-based fingerprints will normally fail with `GM_E_MODEL` even
though the record layout is unchanged. Re-index original texts into a **new**
directory. Texts are not recoverable from sign vectors. There is no automatic
migration, nor a claim that model quality is preserved across engine changes.

## Lock and transaction protocol

`memory.lock` is a stable regular file held with nonblocking exclusive `flock`
for the entire handle lifetime. A second handle returns `GM_E_BUSY`. Never
remove the lock file while a handle may be open. Handles are single-threaded.

A 400-byte undo record contains:

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 8 | `GMUNDO1\n` |
| 8 | 96 | Old vector, chunk and document headers |
| 104 | 8 | Little-endian document index or operation sentinel |
| 112 | 256 | Old overwritten document, or zero bytes |
| 368 | 32 | SHA-256 of bytes 0 through 367 |

For replacement, index equals an existing doc index or the old doc count for
insertion. `UINT64_MAX` means creation (empty old headers);
`UINT64_MAX - 1` means compaction.

1. Finish embeddings and all fallible allocations first. Write `undo.tmp` to
   a fresh inode, sync it, rename to `undo.gm`, then sync the directory.
2. Append vectors/chunks, update their counts, then replace/append the document.
   Until commit, the old in-memory document remains visible.
3. Sync all three data files and the directory. Remove `undo.gm` and sync the
   directory again. Only then publish the new in-memory document.

An error before publishing the journal returns `GM_E_IO` with the old logical
state intact. Once journal publication has occurred, an I/O failure returns
`GM_E_UNCERTAIN`; further store operations reject the poisoned handle. Close
and reopen. A failed operation may have committed fully; do not assume rollback.
`GM_OK` means the OS accepted the required sync operations.

Opening first validates an authoritative journal, opens and checks its recovery
destinations, then restores old headers/document and truncates appended tails.
All restored files and directory changes are synced before removing the marker.
Interrupted recovery is repeatable. A bad journal checksum is rejected without
rewriting the data files. Recovery can modify disk even if a later open step
fails, for example because the configured memory budget is too small.

Creation uses the same protocol and permits recovery to create missing files.
Creating the store directory also syncs its parent; the parent must already exist.

## Compaction

Copy live vectors/chunks into new bounded arrays and write/sync `vectors.new`,
`chunks.new`, `docs.new`. Create hard-link backups `vectors.bak`, `chunks.bak`,
`docs.bak` and sync the directory before publishing the compaction undo marker.
Rename all three new files into place; complete the normal sync/commit protocol.
Document IDs, generations, ordinals and tie ordering remain unchanged.

Recovery restores each backup whose rename has not already been undone. It
checks old header and length before any rewrite. Stale `.tmp`, `.new` and `.bak`
files have no authority without `undo.gm`; they may remain after an early error.
A subsequent operation reclaims its temporary names. Never manually remove
recovery files while a store is open or an authoritative journal exists.

## Scope of durability and resource limits

Requires a trusted directory on a local POSIX filesystem supporting regular
files, advisory locks, hard links, file/directory `fsync` and same-directory
atomic rename. External writers, directory renames while open and network
filesystem semantics are unsupported. Symlink data files are refused. Temporary
files are recreated rather than truncating a possibly aliased stale inode.

Process-kill tests do not simulate power loss, filesystem bugs or loss of device
write caches. On macOS `fsync` is used, not a claim of device-level `F_FULLFSYNC`.
A filesystem/device that does not honor the required ordering is outside this
contract.

The default 256 MiB store budget includes document capacity, all physical chunks
and vectors, validation scratch and conservative growth peaks. Compaction needs
room for old and new arrays simultaneously. Engine/model memory, store-object
and descriptor overhead, input copies, ~256 KiB token scratch and bounded
embedding staging are outside this budget. Maximum staging at 65536 tokens and
65536 dimensions is 341 vectors, about 2.67 MiB. Statistics expose array capacity
bytes, the sum of the three data-file lengths, and obsolete/live counts. Disk
statistics exclude temporary files, backups, lock and journal.
