# Changelog

## Unreleased — experimental 0.1.0

- Keep the original three-file architecture; add locked, synced undo transactions and
  repeatable recovery for creation, document replacement and compaction.
- Fix unaligned Hamming reads, residual vector bytes, same-size text replacement,
  stale generations, unchecked size arithmetic and borrowed-path lifetime claims.
- Add bounded explicit-length text, store budgets, statistics, path copying and
  compaction. All own fallible APIs use `[[nodiscard]]`.
- Change model identity to a content-derived 64-bit digest including preprocessing.
  Existing timestamp-bound stores normally return `GM_E_MODEL`; re-index original
  documents into a new directory. There is no automatic migration.
- `gm_doc_path` remains valid only until the next mutating call or close.
  `gm_chunk_count` reports live chunks. Empty text replaces content with zero chunks.
  Idempotence compares packed vectors after inference, not timestamps/length.
- `gm_opts` adds memory budget and BOS/EOS policy. New error statuses distinguish
  busy stores, configured limits and uncertain writes. This changes the experimental
  API/ABI; rebuild consumers against the installed header and matching archives.
- Pin geistlib, separate configuration artifacts, add Make test/link/install/fuzz/
  benchmark/package targets and a small CLI. No model weights are bundled.

- Introduce incompatible v2 serialization with explicit little-endian fields and
  SHA-256 for each header and record. Bind record checksums to kind, model, dimension
  and position; validate retained records before recovery writes.
- Add `gm_import_v1` and `make import-tool`: preserve legacy data, IDs, generations
  and tie order while converting into an absent destination. Format conversion
  preserves the historical fingerprint and cannot detect preexisting v1 bit errors.
- Add independent golden bytes, every-byte corruption checks, codec batch boundaries
  and import OOM/I/O/process-kill regression tests.
- Normalize package metadata and verify byte-identical packages from two fresh
  builds with `check-repro`. Hash every payload file and preserve an existing
  artifact if tar/gzip fails.
- Add an original Apache-2.0 DE/EN retrieval fixture and `bench-model` for float/
  sign ranking, Recall@1/3, MRR, model timings and RSS; test the evaluator separately.
- Mark completed and pending plan items individually, with native and real-model
  acceptance kept separate from implementation.

Native Linux/Pi and real-model validation remain outstanding; see docs/VALIDATION.md.
