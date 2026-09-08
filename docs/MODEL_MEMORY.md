# Full-model allocation acceptance

The public store budget deliberately excludes model memory. These tests measure
`gm_open`, replacement, recall and compaction with the SHA-pinned, prepared
BitNet embedding 0.6B, not with the mock engine. Normal builds keep libc's
allocator; no allocator or tracing dependency is added to the library.

## Instrumentation and checks

`make model-memory` builds a Linux-only test executable using GNU ld `--wrap`
for malloc, calloc, realloc, aligned_alloc, posix_memalign, strdup and free.
It counts **requested live bytes**, allocation calls and phase peaks across
geist-memory and the statically linked engine. A fixed 16-MiB BSS hash table
tracks pointers without recursively allocating. `make test-model-alloc` checks
alignment, counting, one-shot failure and preservation on failed realloc.
The driver rejects a healthy requested-byte phase peak above 1100 MiB for this
fixed model/workload. This leaves headroom over the measured ~1.00 GB and catches
reintroduction of the previous ~1.46 GB context allocation; it is not a public
memory budget or an RSS ceiling.

These figures exclude libc-private allocations, mapped files, allocator metadata
and transient copying inside libc realloc. RSS includes the instrumentation,
resident mapped pages and allocator behavior, so it is a separate measurement.
The harness requires single-threaded native GEMM without OpenMP. Tracking-table
exhaustion is a test failure (exit 99), never silent loss of evidence.

`tools/check-model-memory.sh EXECUTABLE LOG_DIRECTORY` runs each fault in a
fresh process with a 180-second timeout. It checks:

- Null output handles on failed open, and zero counted live bytes after close.
- Byte-identical store files after failed replacement/compaction.
- Successful retries, exact ordered recall after successful fallbacks, and
  idempotent packed vectors on a normal retry of a successful replacement.
- Close/reopen and recall of persisted data after the mutation tests.
- Every measured allocation in replacement, recall and compaction (bounded to
  64 calls per phase); all startup allocations of at least 8 MiB; the first 16,
  seven evenly spaced interior and the last startup allocation.

The measured run contains 11 replacement, 8 recall, 2 compaction and 1065 startup
allocation calls; six startup calls request at least 8 MiB. This gives **51 fault
cases plus two baseline processes**. It is not an exhaustive sweep of all startup
calls, arbitrary texts, model families, native backends or libc-internal failures.
A failed optional engine allocation may legitimately return success via fallback;
the harness requires that the resulting packed data/search answers remain correct.

## Tokenizer failure found by result comparison

The first cleanup-only sweep passed, but a stronger comparison found that
replacement fault index 3 returned success with different packed embeddings.
The engine's BPE helper returned zero on scratch OOM; callers treated it as an
empty chunk and silently omitted text. The versioned patch now propagates an
internal failure sentinel through GPT-2, Qwen2, SentencePiece and Unigram,
cleans scratch and reports zero output tokens. The public operation fails before
committing changed vectors.

`make test-tokenizer-oom` covers all 40 measured scratch allocations across
synthetic examples in those four tokenizer paths, including special-token
boundaries and trailing whitespace. Every injected allocation must return
failure, leave no tracked allocation and permit an identical successful retry.
This fast real-engine test needs no model and is part of ordinary Linux CI.

## Context-bound optimization, Pi5 on 2026-09-07

The adapter previously limited only its explicit session to 258 tokens. The
engine had already created model-owned RoPE/default-session buffers for 4096
tokens. The additive patched `geist_model_load_with_opts` passes the same 258-token
bound to model creation. Legacy engine calls retain their behavior. Model weights,
KV precision, batch size, token policy and public geist-memory API are unchanged.

| Full-application metric | Previous load | Bounded load |
| --- | --- | --- |
| Open requested live/peak bytes | 1461045444 | 999257284 |
| Replacement requested peak bytes | 1461363361 | 999575201 |
| Recall requested peak bytes | 1461102212 | 999314052 |
| Compaction requested peak bytes | 1461100944 | 999312784 |
| After close requested live bytes | 0 | 0 |
| Process peak RSS | 726656 KiB | 275040 KiB |

The measured requested-byte peak falls by 31.6%; RSS falls by 62.1% in these
runs. RSS is not a guaranteed ceiling. Hardware: Pi5 4 GiB, GCC 14, release,
`TARGET=pi5 BACKENDS=cpu_neon`, native GEMM, the same prepared model SHA as
[MODEL_BENCHMARK.md](MODEL_BENCHMARK.md). Other acceptance processes ran on the
host; these are memory measurements, not isolated throughput comparisons.

`make test-model-equivalence` compares all finite float embedding components
bit-for-bit between legacy and bounded loading on the same backend. The tested
inputs cover a document, a prefixed query and a full 256-content-token window
with EOS. It passed on Pi5 and macOS ARM64. This checks this optimization against the previous
engine, **not** numerical equivalence against Microsoft's implementation.

## Reproduce

```sh
export GEIST_EMBED_GGUF_PATH=/path/to/prepared-model.gguf
make -j4 TARGET=pi5 CC=gcc BACKENDS=cpu_neon test-model-alloc model-memory
binary=$(make --no-print-directory TARGET=pi5 CC=gcc BACKENDS=cpu_neon print-config | sed -n 's/^BUILD=//p')/test-model-memory
sh tools/check-model-memory.sh "$binary" build/model-faults
make TARGET=pi5 CC=gcc BACKENDS=cpu_neon test-model-equivalence
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  make -j4 TARGET=pi5 CC=clang BACKENDS=cpu_neon MODE=asan model-memory test-e2e
```

Linux ASan/UBSan/LeakSanitizer passed the optimized full-model lifetime and E2E
runs. Additional startup fault runs exercise large allocations with leak detection.
Remaining gates include exhaustive startup failures, failure combinations,
other kernel operations, representative workloads and other model families.
