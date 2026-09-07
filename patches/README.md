# Pinned engine corrections

`geistlib-compat.patch` applies only to geistlib
`32b432660948a50be05b355efa74a789456a37dd`. Make includes the patch checksum in
both the exported-source directory and build configuration. The neighbouring
geistlib checkout is never modified. Patch application is mandatory and fails
the build if its expected source no longer matches.

The acceptance runs reproduced these issues:

- Generic ARM64 builds used `vdotq_s32` in attention and PTQTP kernels while
  checking only NEON availability. Dot-product requires its own CPU feature.
  Guard these paths with `__ARM_FEATURE_DOTPROD`; retain the existing scalar
  fallback. Mark its unused lookup table accordingly for strict Clang builds.
- Each BitNet embedding layer owns 18 weight buffers, but the engine allowed
  only 16. Keep bounded ownership tracking, with capacity 24. The existing
  overflow check and cleanup remain intact. The real-model load and E2E test
  exercise all 28 layers.
- Session cleanup omitted the embedding projection-input scratch alias. Pi5
  LeakSanitizer found eight leaked buffer headers (512 bytes) in real-model E2E.
  Include this alias in the existing destruction list, before freeing its pool.

- Intel macOS compiled an unused ARM-only sysctl helper and failed strict
  Clang warnings. Match the helper guard to its Apple Silicon call sites;
  retain all warnings and the existing x86 CPU detection.

No CPU minimum is silently raised. Model preparation separately addresses the
pinned engine's F32 normalization and metadata contract; see
[MODEL_BENCHMARK.md](../docs/MODEL_BENCHMARK.md).

The model allocation acceptance adds `geist_model_load_with_opts`, an additive
engine entry point that passes explicit session bounds to model creation. The
existing `geist_model_load` retains its default behavior. geist-memory passes
its 258-token bound at load and session creation; otherwise the engine creates
model-owned RoPE/default-session buffers for 4096 tokens even though no public
operation can use that capacity. The regression compares exact finite embeddings
from legacy and bounded loads, including a full window. This does not change
KV precision, batching, weights, or the model fingerprint policy.

Full-model fault injection also reproduced silent token loss: BPE/Unigram
scratch allocation failure returned a zero-length chunk, and the enclosing
encoder reported success. Return an internal `SIZE_MAX` failure sentinel and
propagate it through every GPT-2/Qwen2/SPM/Unigram call site; free scratch and
zero the reported token count before returning failure. The adapter then rejects
the operation before committing changed vectors. `test-tokenizer-oom` injects
every scratch allocation for synthetic inputs in all four paths, including
special-token boundaries, and verifies cleanup and identical successful retries.
