# Store format 2 and durability

The authoritative files remain `vectors.gm`, `chunks.gm`, and `docs.gm`.
All integer fields are explicitly encoded little-endian; no C object layout,
alignment, padding or host byte order is persisted. The implementation currently
targets 64-bit macOS/Linux. The three-file architecture and document semantics
are retained; the byte format changes incompatibly from v1.

## Data layout

Each file starts with a 64-byte header:

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 4 | Magic `0x324d47ed` |
| 4 | 4 | Version `2` |
| 8 | 4 | Dimension, positive, divisible by 8, at most 65536 |
| 12 | 4 | Record stride, including checksum |
| 16 | 8 | Record count |
| 24 | 8 | Model/preprocessing identity |
| 32 | 32 | SHA-256 of bytes 0 through 31 |

Each record contains the following payload followed by a 32-byte SHA-256:

- `vectors.gm`: payload `dimension / 8`, one packed vector per physical chunk.
  Positive finite components set bit `i % 8` of byte `i / 8`; zero and negative
  components clear it. NaN/infinity from inference are rejected before commit.
- `chunks.gm`: payload 16 bytes: uint32 `doc` at offset 0, `ordinal` at 4,
  `generation` at 8, zero at 12. Vectors and chunks have identical counts.
  A chunk is live only when its generation matches the referenced document.
  Live ordinals start at zero and are consecutive within each document.
- `docs.gm`: payload 256 bytes: NUL-terminated ID/path in bytes 0–231, zero
  bytes 232–247, uint32 generation at offset 248, zero bytes 252–255.
  IDs are nonempty and unique, at most 231 bytes. Bytes after the first NUL
  must also be zero. Document IDs are stable array indices, including empty
  documents. Legacy mtime/size fields are not retained on disk.

The record checksum is SHA-256 over a 24-byte context followed by the payload.
Context fields are little-endian: uint32 file kind (vectors=0, chunks=1, docs=2),
uint32 dimension, uint64 model fingerprint, uint64 physical record index.
This detects accidental substitution between file kinds, models and positions,
including swaps of equal payloads. Counts are excluded so appends do not require
rehashing prior records. Compaction recalculates checksums at the new positions.
For example, 64-bit vectors have stride 40; chunks 48; documents 288.

File sizes must equal header plus count times stride. All headers, references,
generations, strings and live ordinals are checked before exposing a handle.
All three data files must exist, or none may exist when creating a store. A
missing file is never silently recreated without a valid creation journal.
Bounds and configured memory limits are checked before loading record arrays.

Headers are verified before allocating. Every record, including obsolete
vectors, is checksum-checked and decoded in bounded 64 KiB batches directly into
resident arrays. Invalid checksums or noncanonical reserved bytes return
`GM_E_FORMAT`. A checked-in byte fixture generated independently of this codec
specifies a complete small store.

Checksums detect accidental corruption, not malicious forgery: they are unkeyed.
They do not prevent replay of a complete older store, replacement by a consistent
store with the same identity, or edits by a writer able to recompute hashes.
The format has no store UUID or authentication key. Backups remain necessary.

## Model identity and compatibility

Compute SHA-256 over the complete model file, then SHA-256 over that digest,
policy bytes `{2, 0, 1, 64, omit_bos, omit_eos}` and query prefix bytes without
NUL. The policy identifies revision 2, a 256-token window, 64-token overlap
and token wrapping. The first eight digest bytes, interpreted little-endian,
are stored in `model_fp`. Identity thus retains only 64 bits, not a full
cryptographic digest. The model must remain immutable while opening/using it.

Older timestamp-based fingerprints will normally fail with `GM_E_MODEL`.
Re-index original texts into a **new** directory when model/preprocessing identity
is incompatible. Texts are not recoverable from sign vectors. Format import does
not establish model compatibility or guarantee quality across engine changes.

## Importing v1

`gm_open` rejects v1 without rewriting its data. Use
`gm_import_v1(source, destination, max_store_bytes)` or the CLI built by
`make import-tool`: `memory-import-v1 SOURCE NEW_DESTINATION`.

- Stop all v1 writers first, including old implementations without locking.
  The importer locks the source's regular `memory.lock` (creating it if absent)
  and opens its three data files read-only. It refuses any pending `undo.gm`;
  recover that source with its matching v1 implementation first.
- Validate the explicit legacy little-endian layout, dimensions, fingerprint,
  exact file lengths, budget, IDs, generations, references and live ordinals
  before creating a destination. Legacy reserved/mtime/size fields are ignored.
  v1 has no data checksums: plausible preexisting corruption cannot be detected
  retroactively. See the [historical layout](FORMAT_V1.md).
- The destination parent must exist; the destination itself must not exist.
  Existing targets are refused. Import preserves all physical vectors, IDs,
  generations, empty documents and tie ordering in one bulk transaction.
  It preserves the 64-bit fingerprint verbatim, never rebinds it to a new model.
  No model or original text is needed for this format-only conversion.
- Source data remains unchanged on success and failure. Failure may leave a
  newly created destination directory, an empty store, a complete imported store
  or an authoritative recovery journal. Inspect it; close/reopen for recovery
  before use. A retry requires a different absent destination. The importer never
  removes a destination automatically. A complete import becomes durable through
  the same commit protocol as other writes.

The configured budget limits loaded source arrays and validation scratch. The
empty destination adds only fixed store/descriptor/codec overhead. Import does
not temporarily allocate a second full copy of the source arrays.

## Lock and transaction protocol

`memory.lock` is a stable regular file held with nonblocking exclusive `flock`
for the entire handle lifetime. A second handle returns `GM_E_BUSY`. Never
remove the lock file while a handle may be open. Handles are single-threaded.

A 528-byte undo record contains:

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 8 | `GMUNDO2\n` |
| 8 | 192 | Three old encoded headers, including their checksums |
| 200 | 8 | Little-endian document index or operation sentinel |
| 208 | 288 | Old encoded document and checksum, or zero bytes |
| 496 | 32 | SHA-256 of bytes 0 through 495 |

For replacement, index equals an existing doc index or the old doc count for
insertion. `UINT64_MAX` means creation or import into an empty store (empty old headers);
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
destinations and every retained record checksum before rewriting anything. The
overwritten document is validated from the saved journal record; partial appended
tails are discarded. Recovery restores encoded old headers/document and truncates
appended tails.
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
checks old encoded header, length and all retained record checksums before any
rewrite. Stale `.tmp`, `.new` and `.bak` files have no authority without `undo.gm`; they may remain after an early error.
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
and descriptor overhead, 64 KiB codec scratch per call, input copies, ~256 KiB
token scratch and bounded embedding staging are outside this budget. Maximum staging at 65536 tokens and
65536 dimensions is 341 vectors, about 2.67 MiB. Statistics expose array capacity
bytes, the sum of the three data-file lengths, and obsolete/live counts. Disk
statistics exclude temporary files, backups, lock and journal.
