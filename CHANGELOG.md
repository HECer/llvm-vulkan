# Changelog

## 1.1.0 — 2026-11-02

- **Cross-Block Dominator GVN (`lv-gvn` / `gvn`)**: Connected the GPU dense dominator
  solver (`dominators_rounds.comp`, `dominators_local_banked.comp`) into `GVNPass` to
  eliminate common subexpressions across basic blocks dominated by the definition.
- **Commutative CSE Canonicalization**: Normalized operand IDs for commutative operations
  (`add`, `mul`, `and`, `or`, `xor`, `eq`, `ne`) before hashing so that reversed operand
  pairs (`a + b` and `b + a`) produce matching hashes and are eliminated.
- **Drop-In Standard Pass Aliases**: Aliased standard LLVM pass names (`early-cse`, `gvn`,
  `simplifycfg`, `dce`, `adce`, `instcombine`) to enable direct drop-in usage with
  `opt -passes='early-cse,gvn,simplifycfg,dce'`.
- **Automatic Clang Pipeline Integration**: Implemented EP callbacks (`PipelineStart`,
  `PipelineEarlySimplification`, `OptimizerLast`) enabling automatic pass insertion when
  compiling with `clang -fpass-plugin=LLVMVulkan.so -O2` and `LV_AUTO_INSERT=1`.
- **Extended Test Suite**: Added `tests/llvm/gvn.ll` and `tests/llvm/cse_commutative.ll`.
  Verified against LLVM 21 with 19/19 fixtures passing and native binary execution.

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
