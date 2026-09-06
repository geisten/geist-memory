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
corpus and define acceptance thresholds from those results. No genuine-model
measurement is currently recorded in this repository.
