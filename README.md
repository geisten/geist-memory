<p align="center">
  <img src="assets/header.png" alt="geist-memory" width="100%">
</p>

# geist-memory 🧠

> **A semantic memory small enough to run permanently on a Raspberry Pi.**
> No database, no server, no Python. geistlib and libc.

```c
struct gm *mem;
gm_open("~/.geist/memory", "bitnet-embeddings-0.6b-bf16-i2_s.gguf",
        &(struct gm_opts){.query_prefix = "query: "}, &mem);

gm_remember_file(mem, "notes/arm-simd.md");

struct gm_hit hits[5];
size_t n;
gm_recall(mem, 5, "what did we decide about ARM SIMD?", hits, &n);
for (size_t i = 0; i < n; i++)
    printf("%s #%u\n", gm_doc_path(mem, hits[i].doc), hits[i].chunk);
```

That is the whole surface. There is no daemon to start, no index to build,
no schema to migrate.

[![License](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)
[![C Standard](https://img.shields.io/badge/C-C23-orange.svg)](https://en.wikipedia.org/wiki/C23_(C_standard_revision))
[![Engine](https://img.shields.io/badge/engine-geistlib-8b93c8.svg)](https://github.com/geisten/geistlib)
[![Status](https://img.shields.io/badge/status-experimental-yellow.svg)](#status)

---

## How it works

Most retrieval stacks are five processes in a trench coat: an embedding
server, a vector database, a cache, a queue, and glue. geist-memory is a
static library with two moving parts.

**1-bit embeddings.** Text goes through a ternary-weight embedding model on
the [geist](https://github.com/geisten/geistlib) engine, and the resulting
vector is kept as one bit per dimension — its sign. A 1024-dimensional
embedding is **128 bytes**. The sign is what survives quantization best,
because it is the one property that does not depend on scale, and cosine
order over unit vectors is well approximated by Hamming distance over signs.

**Three flat files.** The store is `vectors.gm`, `chunks.gm` and `docs.gm`,
each a small header followed by fixed-size records. The array index *is* the
file offset. That removes the index file, the parser, and the schema
migration in one stroke — and it is why the store can be understood by
reading two structs.

Search is a `popcount` loop over the whole store. No ANN index, no
clustering, no approximation: the answer is exact.

## A library, not a service

geist-memory does not watch your filesystem, run in the background, or own a
port. It exposes six functions plus three accessors, and holds no state you
cannot see. What
watches, schedules or serves is the caller's business — and the caller is
usually a shell pipeline:

```sh
find ~/notes -name '*.md' | xargs -n1 geist-remember     # a 30-line caller
history | geist-remember -
git log --format=%B | geist-remember -
```

One collector ships: files, plus strings through `gm_remember_text`. Browser
history, calendars and Home Assistant events have different half-lives than
this code and do not belong inside it.

## Bring your own model

Built against [microsoft/bitnet-embedding-0.6b][m] — a 1.58-bit multilingual
embedding model, 1024 dimensions. Any GGUF that geistlib can embed with will
work; pooling is read from the model's own metadata, so nothing here has to
be told which kind it is.

A store belongs to **one** model. Vectors from two models are not comparable
at all, and mixing them yields confident nonsense rather than an error — so
`gm_open` records the model's fingerprint and refuses a different one with
`GM_E_MODEL`. Changing models means re-indexing.

[m]: https://huggingface.co/microsoft/bitnet-embedding-0.6b

## Build

```sh
git clone https://github.com/geisten/geistlib ../geistlib   # or set GEISTLIB=
make                                                        # lib/<target>/release/libgeist_memory.a
```

`geist_session_embed` is still `EXPERIMENTAL` and unreleased in geistlib, so
the engine is a path (`GEISTLIB=../geistlib`) rather than a pinned submodule.
It becomes a submodule when the API lands in a release.

```sh
GEIST_EMBED_GGUF_PATH=path/to/model.gguf make test
```

The test is the only `main()` in the repository. It indexes three documents,
asks three questions that share no vocabulary with their answers, closes and
reopens the store, re-indexes a changed file, and checks that a store built
by another model is refused:

```
model dim=1024 bits (128 bytes/vector)
  "why does my loaf not rise?"                    -> doc_yeast.md    (d=296)
  "what makes the sea level change twice a day?"  -> doc_tides.md    (d=260)
  "how much of my instalment pays down the debt?" -> doc_mortgage.md (d=272)
long text -> 21 chunks
store built by another model -> refused
```

Clean under ASan and UBSan.

## The store on disk

```
vectors.gm   32-byte header + one packed sign-bit vector per chunk
chunks.gm    32-byte header + {doc, chunk, generation, n_tokens}
docs.gm      32-byte header + {path, mtime, size, generation, n_chunks}
```

Re-indexing bumps a document's `generation`; recall skips chunks whose
generation no longer matches. A changed file therefore stops matching
immediately, without a compaction pass. The superseded vectors stay on disk —
that is the shortcut, and reclaiming them is a rewrite of three append-only
files rather than a migration.

## Status

**Experimental.** The library does what the test says it does, on the one
model it was built against, on macOS and Linux. Known limits, all deliberate
and all marked in the source:

- **The Pi is the design target, not yet a measurement.** geistlib's own
  BitNet numbers come from a 4 GB Pi 5; geist-memory has not been run there.
  The arithmetic — 100k chunks is 12.8 MB of vectors — is not a benchmark.
- **The whole store is resident.** Fine into the hundreds of thousands of
  chunks; `mmap` when that stops fitting in the RAM you want to spend.
- **The scan is linear.** An ANN index earns its place when a scan measurably
  exceeds 100 ms, and not before.
- **No compaction.** Dead vectors from re-indexed documents accumulate.
- **Single-threaded, single-writer.** One handle, one thread, one process.

## Where this is going

In rough order: a thin `geist-remember` / `geist-recall` CLI so the shell
pipelines above are real; a Pi 5 measurement to replace the arithmetic;
compaction once a store has been abused long enough to need it. Watching the
filesystem is deliberately last — a manual re-index over a tree is cheap and
idempotent, and that has not yet become annoying enough to fix.

## Contributing

Small, boring patches welcome. The code follows geistlib's
[AGENT.md](https://github.com/geisten/geistlib/blob/main/AGENT.md): lengths
before the arrays they describe, `nullptr` over `NULL`, no silent truncation,
and a deliberate shortcut carries a `ponytail:` comment naming its ceiling
and its upgrade path.

## License

Apache License 2.0 — the same terms as
[geistlib](https://github.com/geisten/geistlib); see [LICENSE](LICENSE).
