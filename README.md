# geist-memory

A semantic memory small enough to run permanently on a Raspberry Pi.

Not a vector database. A library: point it at a directory and a 1-bit
embedding model, feed it files, ask it questions.

```c
struct gm *mem;
gm_open("~/.geist/memory", "bitnet-embeddings-0.6b-bf16-i2_s.gguf",
        &(struct gm_opts){.query_prefix = "query: "}, &mem);

gm_remember_file(mem, "notes/arm-simd.md");

struct gm_hit hits[5];
size_t n;
gm_recall(mem, 5, "what did we decide about ARM SIMD?", hits, &n);
for (size_t i = 0; i < n; i++)
    printf("%s #%u (d=%u)\n", gm_doc_path(mem, hits[i].doc), hits[i].chunk, hits[i].distance);
```

## What is not in here

No SQLite, no HNSW, no server, no Python. The dependency list is geistlib
and libc.

**The store is three fixed-stride files.** `vectors.gm` holds one packed
sign-bit vector per chunk, `chunks.gm` and `docs.gm` hold fixed-size
records. The array index *is* the file offset, so there is no index, no
parser and no schema.

**The search is a popcount loop.** Embeddings are binarized to one bit per
dimension — 128 bytes for a 1024-dim vector — and ranked by Hamming
distance over a linear scan. 100k chunks is 12.8 MB streamed once; that is
arithmetic, not a measurement, and the store has not yet been built large
enough to time. An ANN index earns its place when a scan measurably exceeds
100 ms.

## Model

Built against [microsoft/bitnet-embedding-0.6b][m] (ternary weights, 1024
dims). Pooling comes from the GGUF, so nothing here chooses it.

A store belongs to one model: `gm_open` records the model's fingerprint and
refuses a different one rather than mixing two incomparable vector spaces.
Changing models means re-indexing.

[m]: https://huggingface.co/microsoft/bitnet-embedding-0.6b

## Collectors

There is one: files, plus strings through `gm_remember_text`. Everything
else is a shell pipeline the caller writes — `history`, `git log`,
`find -name '*.md'`. Browser, calendar and Home Assistant have different
half-lives than this code and do not belong inside it.

## Build

```sh
make                                              # lib/<target>/release/libgeist_memory.a
GEIST_EMBED_GGUF_PATH=path/to/model.gguf make test
```

`GEISTLIB=../geistlib` by default; it becomes a pinned submodule once
`geist_session_embed` lands in a geistlib release.

## License

Apache License 2.0 — the same terms as [geistlib](https://github.com/geisten/geistlib);
see [LICENSE](LICENSE).
