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

No CPU minimum is silently raised. Model preparation separately addresses the
pinned engine's F32 normalization and metadata contract; see
[MODEL_BENCHMARK.md](../docs/MODEL_BENCHMARK.md).
