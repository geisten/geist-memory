# Model quality and resource measurement

`make bench-model` runs the pinned real engine on the original Apache-2.0 corpus
in `test/retrieval_cases.h`. Version 1 contains eight short documents (four
German, four English) and sixteen queries, one per language for each document.
Each query has one manually assigned relevant document. Cross-language matches
are included. No third-party dataset or model weights are bundled.

```sh
GEIST_EMBED_GGUF_PATH=/path/to/embedding.gguf make bench-model > model-result.txt
# Match the policy required by the selected model:
GM_QUERY_PREFIX='query: ' GM_OMIT_BOS=0 GM_OMIT_EOS=1 \
  GEIST_EMBED_GGUF_PATH=/path/to/embedding.gguf make bench-model
```

The output includes Make configuration and compiler, SHA-256 of the complete
model, corpus version, dimension, prefix and BOS/EOS settings. A model must stay
immutable while hashing/loading. The runtime token window is 256 content tokens
with optional BOS/EOS. Every fixture document must fit one window; the tool
rejects truncation instead of comparing different text spans. Longer-document
chunking is covered by the separate E2E/core tests.

For the same unquantized embedding, the tool compares:

- cosine similarity using double-precision accumulators over float components;
- Hamming distance between strictly positive component signs (zero clears a bit).

Ties retain document order. Per-query output gives the one-based rank of the
relevant document for both representations. Aggregate output reports Recall@1,
Recall@3, mean reciprocal rank (MRR) and overlap of the two top-three sets. Metrics
are shown for all queries and separately for German/English query language.
With one relevant document per query, Recall@k is the fraction with rank <= k.

The resource report includes model hashing time, engine open/probe time,
document embedding time, query embedding total and p50/p95, content token
counts, and peak process RSS. Embedding timings include tokenization and adapter
inference, but exclude quality scoring, public API staging and persistent store
I/O. The process holds one real model and the corpus float vectors. These are
single-run diagnostics, not steady-state performance claims or an isolated
measurement of model-only RSS. Model open includes the engine's dimension probe.

`make test-quality` checks the ranking evaluator with known vectors: ties,
scale invariance, opposite signs, quantization-induced ranking changes, top-k
set overlap, extreme finite values, non-finite inputs and zero vectors. It also
runs the complete reporting tool using the mock engine. That output is clearly
labelled `model_source=mock` and is **not** evidence of real retrieval quality.
`make model-tools` compiles both real-model executables without requiring weights.

A completed benchmark reports measurements; it does not enforce a quality
threshold. The tiny corpus is useful for regressions and inspecting quantization
loss, but cannot establish multilingual retrieval quality. Before release, run
with the intended GGUF and policy, save the output, choose a larger representative
corpus and define acceptance thresholds from those results. The first genuine-model measurement is recorded below.


## Reproduce the first real-model run

Source: [Microsoft BitNet embedding 0.6B](https://huggingface.co/microsoft/bitnet-embedding-0.6b),
GGUF revision `459a4718ed183ebbf5d7c89e4908f66322790e9b`, MIT license.
The official file is 427935008 bytes, SHA-256
`c89c64f05a2d3f83565250a6762640197fc624df866d4a1bd5853f811219af17`.
The model card specifies last-token/EOS pooling, L2 normalization and per-projection
input normalization. It has 1024 embedding dimensions. This is the model actually
downloaded and tested; the 270M variant is not covered.

The pinned engine rejects the original file. `make prepare-model` uses a small
Python 3.11+ standard-library tool to prepare this exact SHA-pinned input:

1. Add the engine's projection-input-norm and last-token-pooling metadata keys.
2. Expand one-dimensional F16 normalization tensors exactly to F32, as required
   by its loader. Tensor values are preserved; token embeddings and packed I2_S
   bytes are copied. No retraining or requantization is performed.
3. Require the deterministic output SHA-256
   `4321e21b9da533f40386aa5ab968cced6196e21ecbf395ae04e5bce1ee88e767`
   before atomically publishing the new file. Preserve the source and an existing
   destination on input rejection. Other model hashes are refused.

This Python tool is optional model setup, not a dependency of library builds or
model-free tests. Model weights remain outside the repository and packages.
The required engine corrections are described in [patches/README.md](../patches/README.md).

```sh
mkdir -p build/models
curl -fL https://huggingface.co/microsoft/bitnet-embedding-0.6b/resolve/459a4718ed183ebbf5d7c89e4908f66322790e9b/bitnet-embeddings-0.6b-bf16-i2_s.gguf \
  -o build/models/bitnet-original.gguf
make prepare-model BITNET_SOURCE=build/models/bitnet-original.gguf
GM_QUERY_PREFIX='query: ' GM_OMIT_BOS=1 GM_OMIT_EOS=0 \
  GEIST_EMBED_GGUF_PATH="$PWD/build/models/bitnet-embedding-0.6b-geist.gguf" make bench-model
GEIST_EMBED_GGUF_PATH="$PWD/build/models/bitnet-embedding-0.6b-geist.gguf" make test-e2e
```

On the Pi, also run the complete real-model lifetime test with Linux leak
detection enabled (macOS ASan does not provide the same leak check):

```sh
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  make -j4 TARGET=pi5 CC=clang BACKENDS=cpu_neon MODE=asan \
  GEIST_EMBED_GGUF_PATH="$PWD/build/models/bitnet-embedding-0.6b-geist.gguf" \
  check fuzz test-e2e
```

## First measured result: macOS ARM64, 2026-09-06

Apple Clang 21, release, scalar CPU backend, native GEMM; prepared model hash as
above, query prefix `query: `, BOS omitted, EOS retained, original 8-document /
16-query corpus. No numerical-equivalence claim against Microsoft's inference
implementation is established by this test.

| Queries | Float Recall@1 | Binary Recall@1 | Float Recall@3 | Binary Recall@3 | Float MRR | Binary MRR |
| --- | --- | --- | --- | --- | --- | --- |
| All 16 | 93.75% | 87.50% | 93.75% | 100% | 0.9531 | 0.9271 |
| German 8 | 87.50% | 75.00% | 87.50% | 100% | 0.9062 | 0.8542 |
| English 8 | 100% | 100% | 100% | 100% | 1.0000 | 1.0000 |

Top-three overlap: 0.7292 overall. Hashing: 2.105 s; engine open/probe: 0.694 s;
291 document tokens: 119.723 s; 274 query tokens: 115.329 s. Query latency p50:
7.159 s, p95: 9.911 s. Peak process RSS: 2044.5 MiB. Other acceptance processes
were running on the same host; these are diagnostic measurements, not isolated
performance claims. The model memory is outside the store budget.

The real E2E test passed indexing, English retrieval, same-ID replacement,
obsolete-generation exclusion, a two-window document, compaction, reopening,
stable ordered hits and model-identity rejection. The multiwindow fixture uses
24 paragraphs; much larger admission boundaries are covered by model-free tests.


## Pi5 NEON measurement

Same prepared model hash, corpus and token policy, GCC 14.2 release,
`TARGET=pi5 BACKENDS=cpu_neon`, native GEMM and no OpenMP. Float Recall@1 is
93.75%, binary Recall@1 93.75%; Float Recall@3 93.75%, binary Recall@3 100%.
Float MRR is 0.9531, binary MRR 0.9688, top-three overlap 0.7708.
German binary Recall@1 is 87.5%; English is 100%.

Hashing: 3.952 s; engine open/probe: 0.228 s; document embeddings: 6.180 s;
queries: 5.671 s. Query p50: 351.286 ms, p95: 489.931 ms. Peak process RSS:
714.11 MiB. This explicitly accelerated backend is substantially faster and
smaller than the measured scalar backend; it adds no external runtime library.

The binary ranking differs slightly between the two engine backends. These
results do not establish bit-identical embeddings across platforms/backends,
nor do they isolate hardware performance from backend implementation differences.

## Larger external corpus: SciFact-256/100

`make bench-model-large` uses a generated, external fixture with 256 documents
and 100 queries from [BEIR SciFact](https://github.com/beir-cellar/beir).
Source dataset: Wadden et al., [SciFact](https://github.com/allenai/scifact),
*Fact or Fiction: Verifying Scientific Claims*. Claims/evidence annotations are
CC BY 4.0; abstracts are ODC-By 1.0 according to the
[source license](https://github.com/allenai/scifact/blob/master/LICENSE.md).
Neither dataset text nor model weights are committed or shipped in library packages.

The BEIR archive is fixed by SHA-256
`536e14446a0ba56ed1398ab1055f39fe852686ecad24a6306c80c490fa8e0165`.
`tools/prepare-quality.py` uses only Python's standard library and refuses other
hashes. Selection v1 takes the first 100 numerically sorted BEIR test query IDs
with exactly one positive judgment, includes every relevant document, and fills
the candidate set to 256 using SHA-ordered distractor IDs. A JSON manifest records
all selected IDs and the generated-header hash; no model output influences selection.

Each document is its title followed by its abstract. The benchmark tokenizes
within a 65536-token bound and explicitly uses the **first 256 content tokens**
for long inputs, appending EOS. It reports the truncation count and token policy;
the small DE/EN benchmark continues to reject truncation by default. This is
single-window document retrieval, not a test of the library's multi-chunk
aggregation. It is larger external evidence, **not the full BEIR SciFact score**
and not representative German/multilingual or application-specific acceptance.
Filtering single-positive queries and reducing the distractor pool make this a
different, potentially easier task than full-corpus SciFact retrieval.

```sh
mkdir -p build/quality
curl -fL https://public.ukp.informatik.tu-darmstadt.de/thakur/BEIR/datasets/scifact.zip -o build/quality/scifact.zip
python3 tools/prepare-quality.py build/quality/scifact.zip build/quality/scifact.h
mkdir -p build/model-results
make -j4 BACKENDS=cpu_neon bench-model-large > build/model-results/quality.log 2>&1
python3 tools/check-model-quality.py build/model-results/quality.log
```

The check script requires all 100 distinct query records, the fixed corpus,
model SHA and BOS/EOS/prefix policy. Initial conservative regression floors are
Float Recall@3 >= 50%, Binary Recall@3 >= 45%, and at most 15 percentage points
of binary Recall@3 loss versus float. These floors were chosen before the first
large result, not tuned to make a failing result pass. They detect gross
regressions; passing them is not an application-quality guarantee.

## Larger measured results, 2026-09-07

Both runs use `cpu_neon`, native GEMM, the same fixed model, 256 candidates and
100 English queries. 181 documents exceed the first-256-content-token window;
60915 document tokens and 2084 query tokens are actually embedded. The source
contains 90371 document tokens before clipping.

| Host | Float R@1 | Binary R@1 | Float R@3 | Binary R@3 | Float MRR | Binary MRR |
| --- | --- | --- | --- | --- | --- | --- |
| Pi5, GCC 14 | 90% | 80% | 94% | 92% | 0.9235 | 0.8630 |
| macOS ARM64, Apple Clang 21 | 90% | 84% | 95% | 95% | 0.9244 | 0.8890 |

Both pass the preselected regression floors. Top-three overlap is 0.6867 on Pi
and 0.6333 on macOS. Binary ranking loses ten and six percentage points of
Recall@1 respectively, even where Recall@3 remains close to float.

Pi document/query inference takes 1517.34/42.87 seconds, query p50/p95
375.19/776.27 ms. macOS takes 465.99/12.65 seconds, p50/p95 110.76/228.03 ms.
These runs shared their hosts with other acceptance work. The Pi quality run
started before bounded model loading (RSS 818.27 MiB); macOS uses bounded
loading (RSS 430.50 MiB). These are not controlled before/after memory or speed
comparisons. See [MODEL_MEMORY.md](MODEL_MEMORY.md) for matched Pi measurements
and exact legacy/bounded embedding checks. Backend/platform KV precision also
differs, so ranking differences cannot be attributed solely to hardware.

## Nightly orchestration

`.github/workflows/model-nightly.yml` defines a separate 02:23 UTC nightly and
manual workflow on native Linux ARM64, with a 90-minute job limit. It downloads
hash-pinned inputs, performs the larger quality gate, exact embedding preservation,
real allocation-failure tests and ASan/UBSan/LeakSanitizer E2E. Reports, selection
IDs and failure logs are retained as artifacts for 30 days; weights are excluded.
The ordinary CI matrix remains model-free and adds the small allocator-wrapper
self-test and real-tokenizer OOM regression on Linux.

Scheduled/manual dispatch requires this workflow to exist on the repository's
default branch. Publishing it on an acceptance branch alone does not activate
the nightly schedule. See [GitHub's workflow documentation](https://docs.github.com/en/actions/how-tos/manage-workflow-runs/manually-run-a-workflow).
The actual activation status and run evidence are tracked in VALIDATION.md.
