# Contributing

Keep library behavior in `src/`; examples demonstrate the public header only.
`gm_store.c` owns format, generation liveness and search. `gm_engine.c` is the
only translation unit including geistlib headers. The platform module owns
exact I/O, sync helpers and bounded file reads.

Use C23 `nullptr`, `bool`, `constexpr`, `static_assert` and `[[nodiscard]]` where
useful. Keep checked arithmetic and compatibility fallbacks in `gm_internal.h`.
Put a buffer's length before its pointer in new interfaces. State ownership,
lifetime, capacity, empty-input and failure behavior in the public contract.
Do not add custom allocators, SIMD, indexes or dependencies without measurements.

Validate disk-derived sizes before allocating or indexing. Stage allocations
before committing changes. Every persistence change must update FORMAT.md and
include failed-write and interrupted-recovery tests. Never remove an uncertain
outcome merely to make a caller's error handling easier.

Run `make check`, `make MODE=asan check fuzz`, `make analyze` with Clang, and
`make format-check`. Test compiler/build changes with `check-linkage` and
`check-install`; packaging changes need `check-package` and `check-repro`.
Model changes also need `test-e2e` and `bench-model` with a real model. Use
unique temporary stores. Tests must execute with `-DNDEBUG`, not rely on assert.
Do not label unavailable model or target hardware checks as passed.

Use `make format` with clang-format (LLVM style in `.clang-format`). Keep comments
focused on constraints and reasons. Public API changes are experimental until a
stable release, but still require CHANGELOG entries and migration notes.
