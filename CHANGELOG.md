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

- Complete native Pi5 and Ubuntu/Alpine ARM64 acceptance, including fully static
  musl consumers and real ENOSPC recovery. Add dependency-policy regressions.
- Carry a versioned, checksum-bound engine patch for generic ARM64 CPU guards
  and sufficient bounded embedding-layer ownership tracking. Free the projection
  scratch alias omitted by engine session cleanup, found by Pi LeakSanitizer.
- Download and SHA-pin the official BitNet embedding 0.6B. Add optional exact
  preparation for the pinned engine and publish genuine DE/EN and Pi5 measurements.

- Publish successful GitHub acceptance for Linux x86-64/ARM64 with GCC/Clang,
  fully static musl consumers and macOS ARM64/x86-64. Fix the engine
  architecture guard exposed by native Intel macOS; all eight CI jobs pass.

Representative quality and numerical-reference acceptance remain outstanding;
see docs/VALIDATION.md for exact coverage and the remaining release gates.

- Bound model-owned context buffers at load time as well as session creation:
  measured Pi full-application requested peak falls 31.6%, RSS 62.1%, with exact
  finite embedding preservation on the tested inputs.
- Reject tokenizer scratch OOM instead of silently dropping text; add real-model
  allocation-failure and model-free tokenizer regressions.
- Add a reproducible external SciFact 256-document/100-query benchmark, explicit
  truncation reporting, fixed quality/peak-memory gates and a separate model
  nightly workflow. Model/data preparation uses optional Python standard library;
  the production C library gains no dependency.
