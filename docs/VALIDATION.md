# Validation evidence

Local run: 2026-09-06, macOS 26.6.2, ARM64. The exact CPU model was unavailable
in the sandbox. Engine revision: `32b432660948a50be05b355efa74a789456a37dd`;
backend `cpu_scalar`, native GEMM, no OpenMP. Results refer to the current
unreleased implementation, not to a released platform support promise.

## Completed locally

| Check | Result |
| --- | --- |
| Apple Clang 21, release and debug, model-free/C/C++ header tests | Passed |
| Homebrew GCC 16, release, model-free/header tests | Passed |
| Homebrew LLVM Clang 23, release, model-free/header tests | Passed |
| Apple Clang ASan + UBSan, full model-free tests | Passed |
| Clang static analyzer with analyzer diagnostics treated as errors | Passed |
| clang-format and `git diff --check` | Passed |
| Deterministic corrupt-store harness under ASan/UBSan | 3000 inputs passed |
| LLVM libFuzzer + ASan/UBSan, final 31-second run | 380611 inputs, no findings |
| Real engine + external consumer, Apple Clang and GCC 16 | Built and ran |
| Staged installation + independent consumer | Built and ran |
| Extracted tar package, SHA-256 verification + independent consumer | Passed |
| README C example and remember/recall CLI | Compiled against real archives |
| v1 import CLI | Ran against independently generated v1; exact independently calculated v2 bytes, source unchanged, existing-target/usage errors passed |
| Format and import tests | Passed under Apple Clang, GCC 16, LLVM Clang 23 and ASan/UBSan |
| Real-model E2E binary | Compiled; mandatory Make target correctly failed without a model |
| Fresh builds in two separate absolute directories | Static archives and complete normalized tar.gz packages byte-identical |
| Package regression tests | Metadata normalization, all payload checksums, invalid epoch and tar/gzip failure handling passed |
| Model quality evaluator and reporting harness | Synthetic ranking tests and explicitly labelled mock run passed |
| Real model benchmark tool | Compiled; genuine GGUF execution still pending |

Release consumers built by Apple Clang and GCC 16 both depend dynamically only on
`/usr/lib/libSystem.B.dylib` and system `Accelerate.framework`. Static archives
are 45056 bytes (geist-memory) and 844472 bytes (geistlib) in the Apple Clang
reference build. Size varies by compiler/configuration. GCC required the pinned
engine's existing `nonnull-compare`/`vla-parameter` warning exceptions; these
are scoped to engine compilation, never applied to geist-memory.

`make check-repro` copies the current source into two fresh absolute directories,
builds the pinned engine and library, installs into fresh staging directories and
compares the complete compressed packages. The result was byte-identical locally;
extracted payload hashes passed. `ZERO_AR_DATE=1` suppresses Darwin archive member
timestamps; the package normalizes entry order, ownership, permissions and times.
`SOURCE_DATE_EPOCH` selects the timestamp (default 946684800). `PACKAGE.txt` records
it, and `SHA256SUMS` now covers documentation, licenses and pkg-config as well.

This requires identical source, configuration, compiler and packaging tools.
It does not promise cross-compiler/cross-tar byte equality or normalized debug
paths. `check-repro` explicitly rejects non-release modes. Tool failures during
packaging leave an existing published local artifact untouched.

## What the tests exercise

- SHA-256 known answers and split-block updates; sign packing, zero, NaN/infinity.
- Independent bitwise Hamming/top-k reference for dimensions 8, 16, 56, 64, 72,
  120, 128, 1024; ties, k larger than the corpus, unaligned input and tail bytes.
- Same-size text/file changes, no-op vector replacements, empty replacements,
  byte/token admission boundaries and overlong inputs, copied-path lifetime.
- Staged inference failure on a later window preserves all old chunks.
- Allocation failures during public open/index, store loading/growth and budget
  limits. Failed opens release handles and locks for a successful retry.
- 160 replacement I/O failpoints with writes split into 17-byte pieces;
  30 replacement process-kill points; 16 creation process-kill points.
- 34 compaction failpoints and 34 compaction process-kill points, including empty
  documents and compaction to zero chunks.
- 192 interrupted-recovery cases: 48 points each for failed I/O and process kills,
  for both replacement and compaction recovery. Repeated open restores the same
  complete logical state.
- Each of the 528 journal bytes independently corrupted; malformed lengths,
  strings and document references; partial file sets and huge header counts.
- Independent v2 golden bytes; every byte of all three fixture files individually
  corrupted and rejected without changing data. Valid-checksum impossible counts/
  references/strings, swapped equal records, corrupt obsolete vectors, corrupted
  retained data during recovery, and 65536-bit records crossing codec batch boundaries.
- v1 import: 14 allocation, 260 split-I/O and 80 process-kill positions, including
  successful runs beyond the last hook. Reopening yields either zero or all imported
  documents; source files remain byte-identical. Empty/missing files, invalid source
  records, writer contention, pending journals and existing destinations are tested.
  IDs, empty documents, obsolete generations and physical tie order survive import.
- All four allocation sites during simultaneous vector/chunk/document growth
  and both allocation sites during compaction, with successful reopening afterward.
- Stable writer exclusion, FIFO rejection, and stale temporary hard links that
  must not truncate an authoritative file.

The harness injects short operations and EIO at wrapped read/write/sync/rename/
truncate boundaries. It does not exhaust kernel failures in every `open`, `link`,
`close` or `flock`, simulate a real ENOSPC filesystem, or reproduce hardware
power-loss/cache behavior. Sanitizers and fuzzing are evidence, not a proof of
absence of undefined behavior or persistent-data faults.

## Store-only measurements

Apple Clang release `-O2`, 1024-bit vectors, deterministic pseudorandom data,
one document, k=5, 101 scans. Bench process budget: 512 MiB. No model,
tokenization or inference is included. Timings use warm caches on this host;
other build activity may affect them. `make bench` reproduces the workload.

| Chunks | Store array capacity | Data files | Peak process RSS | Scan p50 | Scan p95 |
| --- | --- | --- | --- | --- | --- |
| 100000 | 18365184 B | 20800480 B | 41.64 MiB | 0.521 ms | 0.570 ms |
| 1000000 | 146815744 B | 208000480 B | 402.12 MiB | 5.213 ms | 5.370 ms |

| Chunks | Initial synced write | Reopen | Compact after replacement with one chunk |
| --- | --- | --- | --- |
| 100000 | 152.2 ms | 134.9 ms | 2.2 ms |
| 1000000 | 1420.5 ms | 1344.8 ms | 8.2 ms |

These are v2 measurements, including checksum encoding and verification. Compared
with the previous v1 measurement (1M: write 61.1 ms, reopen 23.8 ms, disk 144 MB),
checksums materially increase write/open time and disk use. Search uses the decoded
resident arrays and needs no checksum work. Removing unused legacy host fields
reduces resident array sizes; no SIMD or custom allocator was introduced.

Peak process RSS includes the benchmark's input vector buffer, allocator behavior
and growth/compaction peaks. It must not be confused with the store capacity or
a measured real-model application's RAM usage. Physical obsolete vectors are
still scanned for liveness until compaction.

## Pending release gates

- Native Linux x86-64 and ARM64 runs, with GCC and Clang; musl-static consumer
  verification and execution; Pi 5 tests and measured RSS/latency.
- The checked-in CI matrix has not been executed in this local session. Runner
  labels follow [GitHub's hosted runner reference](https://docs.github.com/en/actions/reference/runners/github-hosted-runners).
- A local embedding GGUF was not available. No real-model retrieval, embedding
  throughput, model-open hashing cost or model memory measurement was run.
  A linked missing-model consumer does not exercise inference.
- The original DE/EN fixture and float-versus-sign evaluator are now implemented
  and model-free tested. Genuine-model results and a larger representative corpus
  remain pending; see [MODEL_BENCHMARK.md](MODEL_BENCHMARK.md).
- v2 checksums and source-preserving v1 import are locally tested. Native platform
  fixture runs remain pending. Checksums are unkeyed, and the model identity still
  retains only 64 bits; neither authenticates data or prevents whole-store rollback.
- macOS x86-64 is a build profile only; Windows remains outside the POSIX scope.

The minimum documented GCC option is `-std=c23`, introduced with GCC 14
([GCC 14 changes](https://gcc.gnu.org/gcc-14/changes.html)). Minimum versions are
not a substitute for running the selected compiler/libc/engine combination.
