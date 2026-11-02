# Changelog

## 1.0.0 — 2026-11-02

- Define the supported compute API and scoped LLVM integration contract.
- Install relocatable CMake packages, public/version headers, library, benchmark,
  documentation and recorded evidence. Add a relocated CPU/Vulkan consumer gate.
- Require SPIR-V validation by default; retain an explicit developer override.
- Publish standalone, offline HTML benchmark graphs generated from evidence.
- Include pooled Vulkan buffers, persistent mappings, capability-gated solvers,
  radix sorting, mixed graph components and adaptive convergence batching.
- Preserve CPU/GPU losses, unsupported compiler features and hardware limits in
  the release documentation. This release does not claim whole-compiler speedup.

The earlier benchmark artifacts identify the measured pre-release snapshots.
They are retained unchanged as historical evidence, rather than relabeled as
measurements of this release's binaries.
