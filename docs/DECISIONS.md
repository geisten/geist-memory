# Implementation decisions

1. Keep one public header, an opaque handle and the original three-file store.
   Search stays beside its representation. A private engine adapter isolates
   tokenization/inference; tests replace it without a public plugin framework.
2. Coordinate the three files with a bounded undo journal. Compaction uses the
   same journal and hard-link backups. This preserves the existing record
   structure, but costs more recovery logic than a single authoritative file.
   The rejected single-log rewrite is not part of this implementation.
3. Use explicit little-endian v2 serialization behind a private codec. SHA-256
   protects each header and each record, including its file/model/position context.
   Keep the three files and the small undo protocol. Import v1 only into an absent
   destination, leaving source data unchanged. No new external dependency is added;
   the journal and data codec share the tested in-tree SHA-256 implementation.
4. Stage every embedding before replacing a document. Compare packed vectors
   for idempotence instead of timestamps or file lengths. Unchanged text is
   embedded again; equal vectors avoid disk writes. No text hash field or
   original text is persisted. Empty content keeps the ID with zero live chunks.
5. Derive model identity from model bytes and preprocessing policy. Use the
   existing 64-bit field, retaining 64 bits of SHA-256. This is a compatibility
   guard, not authentication. Legacy timestamp identities require re-indexing
   into a new directory; the explicit format importer preserves fingerprints and
   never claims to repair their historical weaknesses. No silent migration occurs.
6. Expose explicit BOS/EOS omission options. The pinned engine exposes token
   IDs, but not the model's add-token flags. The caller selects the model's
   required wrapping and query prefix. This policy participates in identity.
7. Enforce a store budget, keep checked arithmetic centralized, and use libc
   allocation. All vectors, including obsolete generations, remain resident
   until explicit compaction. No custom allocator, mmap, ANN or SIMD is added.
8. GNU Make orchestrates builds, tests and local packaging. Engine revision,
   backends and native GEMM are explicit. Static archives do not imply a fully
   static executable: macOS uses system libraries; Linux has a static profile.
9. Keep API/format experimental. Native platform runs, real-model retrieval
   quality and target hardware measurements are release gates, not claims
   inferred from a successful compile or model-free tests.
