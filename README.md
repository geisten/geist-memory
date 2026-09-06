<p align="center">
  <img src="assets/header.png" alt="geist-memory" width="100%">
</p>

# geist-memory

A small, experimental C23 library for local semantic memory. It embeds text
with geistlib, stores one sign bit per embedding component and searches by
Hamming distance. No database, server, Python or additional third-party
runtime is needed by geist-memory itself.

The implementation keeps three flat data files, an undo journal for atomic
replacement and explicit compaction. Search is exact **over the packed bits**;
sign quantization is lossy and does not promise exact float-vector ranking.
The whole store, including obsolete chunks, resides in RAM until compaction.

## Build and use

Requires GNU Make 3.81+, a C23 compiler (GCC 14+ or Clang 19+), Git, patch and POSIX
build tools. C23 library features have centralized GCC/Clang fallbacks where
libc lacks `<stdckdint.h>` or `<stdbit.h>`. See [validation](docs/VALIDATION.md)
for the compiler/platform combinations actually tested.

```sh
# Model-free tests need neither geistlib nor a GGUF model.
make check
make MODE=asan check fuzz

# Provide an existing geistlib checkout containing the pinned revision.
git clone https://github.com/geisten/geistlib ../geistlib
make lib engine example
make print-config
```

`GEISTLIB=/path/to/geistlib` selects the source repository. Make archives commit
`32b432660948a50be05b355efa74a789456a37dd` into its own ignored build directory;
it never changes the source checkout or downloads dependencies. `make lib`
builds `libgeist_memory.a`; `make engine` builds `libgeist.a`. Artifacts live
under `build/<target>/<mode>/<configuration>/`. Both archives are required
when linking a consumer.

The default is `BACKENDS=cpu_scalar GEMM_PROVIDER=native LINK=system`.
Optional `cpu_neon` and `cpu_x86` backends must be selected explicitly and need
their own runtime validation. macOS consumers also link system Accelerate and
libSystem; no Homebrew OpenMP/BLAS runtime is required. `LINK=static` is an
explicit Linux release profile and requires a toolchain with static libraries.
It is rejected on macOS. `make check-linkage` inspects the actual consumer.

```sh
make CC=clang MODE=debug check
make CC=gcc-14 TARGET=linux-x86_64 LINK=static check-linkage
make CC=aarch64-linux-gnu-gcc TARGET=linux-aarch64 lib engine
make TARGET=pi5 lib engine       # on a Pi 5 with a C23 compiler
make check-install              # stage, compile and run an external consumer
make install PREFIX=/usr DESTDIR=/tmp/geist-package
make dist                       # normalized local package, licenses and hashes
make check-package              # normalization, corruption and tool-failure tests
make check-repro                # two fresh builds; compare complete packages
```

Cross-compiled tests must run on their target. `TARGET` is checked against the
compiler target; ARM64 is never implicitly treated as a Pi 5. `CC`, `AR`,
`RANLIB`, `CPPFLAGS`, `CFLAGS`, `LDFLAGS` and `LDLIBS` are configurable.
Configuration changes select separate build directories. `make clean` removes
only the resolved configuration; `make help` lists commands.

## API example

```c
#include <geist_memory.h>
#include <stdio.h>

int main(int argc, char **argv) {
    if (argc != 2) return 2; /* argv[1]: an embedding GGUF supported by geistlib */
    struct gm *memory = nullptr;
    const struct gm_opts opts = {.query_prefix = "query: "};
    enum gm_status s = gm_open("memory", argv[1], &opts, &memory);
    if (s == GM_OK)
        s = gm_remember_text(memory, "bread", "Yeast produces gas that lifts dough.");
    struct gm_hit hits[5];
    size_t n = 0;
    if (s == GM_OK)
        s = gm_recall(memory, 5, "why does bread rise?", hits, &n);
    if (s == GM_OK)
        for (size_t i = 0; i < n; ++i)
            printf("%s #%u (%u bits)\n", gm_doc_path(memory, hits[i].doc),
                   hits[i].chunk, hits[i].distance);
    else
        fprintf(stderr, "%s\n", gm_status_str(s));
    gm_close(memory);
    return s == GM_OK ? 0 : 1;
}
```

The compiled [CLI example](examples/memory.c) accepts
`memory DIR MODEL remember ID TEXT` or `memory DIR MODEL recall QUERY`.
Paths are literal: `~` expansion is the shell's responsibility. The store's
parent directory must exist. Original document text is not stored.

## Contracts

- One single-threaded handle owns the directory; another returns `GM_E_BUSY`.
- A document replacement prepares all embeddings before writing. An allocation
  or engine failure preserves old logical content. `GM_E_UNCERTAIN` means close
  and reopen for recovery; a failed operation may already have committed.
- Source length/mtime never determines freshness. Re-indexing recomputes vectors;
  identical packed vectors avoid writes. Empty text removes all live chunks
  while keeping the document ID.
- `gm_doc_path` is borrowed until the next mutating call or close. Use
  `gm_doc_path_copy` for caller-owned storage. Check every fallible return value.
- Documents: at most 262144 bytes and 65536 content tokens; IDs: 231 bytes;
  queries including prefix: at most 4096 bytes and 256 content tokens. Overlong
  input is rejected, never silently truncated. Explicit-length text rejects NUL.
- Chunk windows contain at most 256 content tokens with 64-token overlap.
  BOS/EOS wrapping and query prefix are model-specific. Defaults use available
  BOS/EOS IDs; `omit_bos`/`omit_eos` override this. The pinned engine cannot expose
  the model's add-token flags, so select the correct policy for your model.
- Store memory defaults to 256 MiB, configurable via `max_store_bytes`.
  Engine, input and embedding scratch are outside that budget.
  `gm_get_stats` reports capacity bytes, data-file bytes and live/obsolete chunks.
  `gm_compact` reclaims obsolete chunks while preserving IDs and ordered hits.

Packages normalize entry order, ownership, permissions and timestamps. Set
`SOURCE_DATE_EPOCH` (integer Unix seconds) to choose the timestamp; the default
is 2000-01-01 UTC. Packaging requires GNU tar or bsdtar and gzip. Reproducibility requires matching source (including the pinned engine patch), configuration,
compiler, tar and gzip implementations. Debug paths and cross-toolchain byte
identity are outside this guarantee. `check-repro` requires release mode.

The [public header](include/geist_memory.h), [format and recovery contract](docs/FORMAT.md)
and [design decisions](docs/DECISIONS.md) specify details and limits. In particular,
v2 explicitly serializes little-endian bytes and checksums every header and record
with SHA-256. The three data files remain; no external hashing library is needed.
Checksums detect accidental corruption, not malicious rewriting or rollback.

Build `make import-tool` and run the resulting `memory-import-v1 SOURCE NEW_DESTINATION`
to convert v1 into a new directory. Stop old writers first. Source data remains
unchanged; existing destinations and pending v1 journals are refused. Import
preserves the old model fingerprint: timestamp-based identities still require
re-indexing original texts into a new directory for current model compatibility.
See [import guarantees and failure handling](docs/FORMAT.md#importing-v1).

## Validation and status

```sh
make test                         # model-free core/store/format/import/quality tests
make analyze                      # Clang static analysis, warnings fail
make format-check                 # requires clang-format
make fuzz-libfuzzer FUZZ_SECONDS=30 # coverage-guided fuzzing, Clang required
make bench BENCH_CHUNKS=100000
GEIST_EMBED_GGUF_PATH=/path/model.gguf make bench-model
GEIST_EMBED_GGUF_PATH=/path/model.gguf make test-e2e
GEIST_EMBED_GGUF_PATH=/path/model.gguf make release-check
```

`test-e2e` is model-specific: it expects a BitNet embedding GGUF and the default
wrapping/query prefix. Missing models fail the mandatory Make target. The three
English retrieval examples are a smoke test. `bench-model` adds an original
DE/EN corpus and reports float-versus-sign Recall@1/3, MRR, model timings and RSS;
see [model benchmark](docs/MODEL_BENCHMARK.md). This small fixture is not a
representative multilingual benchmark. Model-free `test-quality` checks its
ranking calculations and reports with an explicitly labelled mock engine.

Tests cover independent reference search, malformed files, allocation failures,
short I/O, sync failures, interrupted replacement/creation/compaction and interrupted
recovery. All eight GitHub CI jobs passed using the same Make targets on
Linux and macOS x86-64/ARM64, including fully static Linux musl consumers.
The [validation report](docs/VALIDATION.md) links the exact tested commits and runs.
Real-model tests remain separate from this model-free CI matrix.

A broader retrieval corpus, numerical engine-reference checks and the remaining
fault-injection/integrity acceptance remain release gates. This is not yet a
finished showcase release.
See [PLAN.md](PLAN.md) and [validation evidence](docs/VALIDATION.md).

## Contributing and license

Keep patches small and contracts explicit; see [CONTRIBUTING.md](CONTRIBUTING.md).
API and format remain experimental; changes are recorded in [CHANGELOG.md](CHANGELOG.md).

Apache-2.0: [LICENSE](LICENSE). geistlib is also Apache-2.0 and bundles stb code
under its own notices. Installation includes the pinned engine's LICENSE and
NOTICE. No model weights are distributed; their licenses are separate.
