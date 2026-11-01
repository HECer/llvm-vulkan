# Research and engineering rationale

Research date: **2026-11-01**. Implementation target: the exact LLVM main commit
in [`upstream-lock.json`](../upstream-lock.json). The assessment below separates
upstream facts, original engineering judgments, and still-unmeasured hypotheses.

## 1. What this experiment can establish

The useful question is whether executing selected compiler computations on a
GPU reduces the time to produce an equivalent result. Compiling a user's
program *for* a GPU answers a different question. LLVM already has GPU target
and offloading infrastructure; that does not make its host compiler pipeline
GPU-resident. For example, LLVM's CUDA guide explains a compilation target,
and MLIR's GPU dialect models device execution. Neither is evidence that the
optimizer itself is running on the GPU. [1][2]

A real prior example is **Pareas**, from Voetter, Huijben and Rietveld. Its
repository accompanies the 2022 paper *Compilation on the GPU? A feasibility
study* and the related 2021 theses. It demonstrates redesigned compilation
stages for a restricted language and RISC-V target. That establishes that
GPU compilation is possible; it does not establish that arbitrary C++, LLVM's
entire pass suite, its target backends, or its diagnostics can be mechanically
translated with a speedup. No Pareas code is incorporated here. [3][4]

The evidence this repository can produce now is narrower: real SPIR-V compute
dispatch, agreement with independent CPU algorithms, integration with selected
LLVM IR transformations, and repeatable correctness/benchmark inputs.
Lavapipe exercises the Vulkan API and shader path on a CPU. That is valuable
functional coverage, but cannot measure RTX 5090 launch latency, occupancy,
cache behavior, PCIe transfer cost, driver compiler behavior or speedup.
Mesa documents the LLVM-based software execution behind llvmpipe. [5]

## 2. Why retain C++ and LLVM's existing IR

LLVM's optimization interface is built around its C++ IR and analysis managers.
The new pass manager has explicit analysis lifetimes and invalidation rules.
A pass that mutates IR must correctly report what remains valid. LLVM supplies
a plugin interface that makes scoped experiments possible without replacing
the parser, IR ownership model, backend or toolchain. [6][7]

**Engineering decision:** keep that boundary in C++, flatten only the selected
work into arrays, run a CPU or Vulkan implementation, then apply validated
changes through LLVM. A Rust host runtime is feasible, but it would introduce
an FFI boundary without removing the required LLVM ownership and semantic
work. Translating C++ containers into Rust containers does not itself expose
GPU parallelism. A clean implementation here means clear ownership, checked
array bounds, explicit dispatch and small interfaces; it does not mean copying
all upstream files into a second language.

This choice also preserves a useful failure boundary. Unsupported LLVM
operations are excluded before dispatch. The GPU answers questions over a
snapshot; it does not independently free IR instructions or rewrite arbitrary
LLVM objects. Exact equality checks and final mutation stay with the host.

## 3. Cost model: speedup must include the whole operation

For an offloaded region, measure:

```text
offloaded time = pack + upload + dispatch + kernel + synchronize
               + download + validate/apply
```

Also record Vulkan instance/device initialization and pipeline creation for a
cold invocation. Compare against the corresponding upstream operation and
against a competitive CPU implementation. A slow scalar reference establishes
correctness, but is not sufficient evidence of practical acceleration.

For a fraction `p` of the original compile time, acceleration `s` of that
fraction, and added overhead `h` expressed as a fraction of the original whole
compile, the idealized total speedup is:

`speedup = 1 / ((1 - p) + p / s + h)`

This is a planning model, not a benchmark result. If only 25% of a build can
be accelerated, even infinitely fast kernels with zero overhead cap the gain
at 1.33x. The real available fraction must be measured in the intended
workload. A large synthetic batch of constant additions cannot establish
the fraction for a real C++ or Rust project.

**Hypothesis:** GPU-friendly work becomes more plausible when many independent
functions or modules can share a dispatch, immutable arrays survive several
operations, and intermediate results remain resident. Work that changes only
a few instructions is likely to lose to launch and packing overhead. These
claims require device measurements; they are not asserted speedups.

Cold startup matters for command-line compilation. A persistent service or
long-lived JIT could amortize it, but would introduce concurrency, cancellation,
failure recovery and version-isolation obligations. Those systems are not
implemented merely by caching a shader pipeline in one process.

## 4. Promising workloads, with the difficult parts preserved

### Batched arithmetic and local candidates

Independent integer operations have regular input/output and cheap exact
comparison. They make a good initial correctness test. LLVM, however, permits
arbitrary-width integers and has semantics beyond the hardware instruction:
`nuw`/`nsw`, exact operations, oversized shifts, signed division overflow,
poison, undef, freeze and other attributes must be respected. The language
reference and UB manual define these differences. [8][9]

The present folder accepts modular scalar integers through 64 bits, using a
separate optional `shaderInt64` path for widths above 32. This remains a narrow
semantic subset: wider APInt, poison-generating annotations, vectors and
non-integer operations stay upstream. Supporting floating point would
additionally need the target's specified formats,
rounding/exception behavior, constrained FP and fast-math distinctions.
Native shader floating point is not automatically a faithful implementation
of APFloat. APInt arithmetic beyond 64 bits could use limbs, but that work and
its tests remain outstanding.

GPU hashes can safely nominate equivalence candidates if the host then proves
identity. They cannot justify replacing instructions by themselves. Even a
collision-free byte encoding is insufficient when dominance, memory state,
metadata, poison flags or operands differ. The initial CSE intentionally stays
inside one basic block and excludes difficult instruction classes.

### Graphs and monotone data flow

Reachability, set propagation and batched bitset intersections expose useful
parallelism. This implementation uses them for conservative dependency marking
and CFG reachability, and offers separate dominator/liveness primitives.
The important research variables are graph size, degree distribution, number
of independent components, diameter, frontier sparsity and convergence rounds.

A synchronous fixed-point traversal over a long chain can require one round
per edge depth. A CPU queue may visit that graph once. Dense dominator sets
also require quadratic space in the block count. Those asymptotic differences
can overwhelm the GPU's parallel throughput. Future work should compare sparse
frontiers, component batching, work-efficient queues and native LLVM algorithms
before replacing an analysis. A dense bitset reference is not automatically
an improvement over LLVM's existing dominator tree.

Machine liveness is not interchangeable with a textbook gen/kill example.
PHI uses occur on edges, live intervals have subranges and target register
constraints, and mutations invalidate results. A correct standalone equation
solver is a building block, not a complete register allocator.

### Large linking and whole-program workloads

Bulk symbol/string hashing, section reachability, identical-code candidate
grouping, profile merging, scans and compression deserve profiling because
they may provide much larger batches. LLD's documented design already avoids
unnecessary work, defers reads and retains symbol handles. A GPU design must
preserve those advantages instead of adding duplicate scans and serialization.
Its object-format-specific logic is also substantial. [10]

ThinLTO already supports parallel backend compilation and caching. The useful
comparison is against those enabled facilities, not a deliberately serial
baseline. Cross-module GPU batching may increase available work, while also
increasing latency and peak memory or competing with CPU workers. The actual
tradeoff depends on the build. [11]

### MLIR, frontend work and broader rewrites

MLIR operation passes have rules specifically designed to support parallel
execution without accessing mutable sibling operations. Those are useful
boundaries for future batching, but do not permit arbitrary parallel writes
to shared IR. An alternative GPU rewrite engine would need to preserve
operation interfaces, regions, dominance and dialect-specific invariants. [12]

Clang preprocessing, templates, overload resolution and recovery cannot be
replaced by simply applying a byte-classification kernel. Parallel lexing and
parsing research provides techniques, especially for restricted grammars, but
the full language frontend needs separate design and semantic validation.
Likewise, instruction selection, scheduling, register allocation and object
emission require distinct backend work; LLVM's code-generator documentation
describes those separate responsibilities. [13]

## 5. Vulkan execution requirements

Vulkan gives explicit control over execution and memory dependencies. Command
submission order alone is not an excuse to omit the required shader-to-shader,
host-to-device or device-to-host visibility rules. Non-coherent mapped memory
requires flush/invalidate treatment. Bounds and limits must be checked before
dispatch, and the selected device must actually support the required queue
and features. [14][15]

The CPU-only validation path must select a software Vulkan device and execute
the same SPIR-V. An ordinary C++ reference function is a separate backend,
not "Vulkan CPU mode". For a physical test, record the device name, IDs,
driver and API version, and fail a GPU-only request when only a CPU device is
available. A requirement to run Vulkan should fail visibly rather than silently
fall back and produce a false benchmark.

There is a bootstrap consideration: lavapipe can itself load an LLVM library
to compile shaders. Loading a plugin linked to a different LLVM build into the
same process can expose symbol or dependency problems. The exact dynamic
libraries and versions must be recorded when investigating such failures.
Successful scalar tests do not cover this boundary.

## 6. Validation that supports a defensible claim

The proposed evidence ladder is:

1. **Primitive correctness.** Compare deterministic CPU and Vulkan results over
   empty, tiny, irregular and randomized inputs; include invalid graph layouts,
   integer boundary cases and device-limit failures.
2. **IR legality.** Run LLVM verification after each experimental transformation.
   Verify PHIs, unreachable regions, EH, address-taken blocks, debug information
   and analysis invalidation.
3. **Semantic equivalence.** Execute source and transformed IR over multiple
   inputs, and use translation validation where applicable. The LLVM verifier
   checks structure, not equivalence of all observable program behavior.
4. **Real workloads.** Compile representative C/C++, Fortran and LLVM-IR inputs
   and, with explicit version-compatible integration, inputs from other
   languages. A standalone IR benchmark is not a Rust compiler integration.
5. **Hardware characterization.** Run lavapipe and the RTX 5090, record every
   backend selection, keep cold and warm measurements separate, report
   distributions and peak memory, and include all transfer/application costs.
6. **Whole-build comparison.** Preserve optimization levels, target flags,
   caches, compiler versions, CPU thread counts and resulting program quality.
   A faster compile that silently performs fewer optimizations is a different
   tradeoff and must be labeled accordingly.

Alive2 is relevant as a translation-validation project for LLVM optimization;
its own documented limitations still apply. It is a future validation layer
here, not a tool that this repository claims to have run automatically. [16]

The priority after functional correctness is to establish an actual
compile-time profile and break-even range on the target device. Expanding the
number of kernels before answering that question could make the compiler
slower and harder to maintain. The coverage ledger records unimplemented
opportunities so that measurement, rather than omission, determines the next
stage.

## Sources

All links below are primary project, specification or author sources, consulted
2026-11-01. Source-specific performance claims from unrelated hardware are not
used as speedup estimates for this project.

1. [LLVM: Compiling CUDA with Clang](https://llvm.org/docs/CompileCudaWithLLVM.html).
2. [MLIR: GPU dialect](https://mlir.llvm.org/docs/Dialects/GPU/).
3. [Pareas author repository and publication references](https://github.com/Snektron/pareas).
4. [Voetter: Parallel Lexing, Parsing and Semantic Analysis on the GPU, Leiden thesis](https://theses.liacs.nl/pdf/2020-2021-VoetterRobin.pdf).
5. [Mesa: LLVMpipe](https://docs.mesa3d.org/drivers/llvmpipe.html) and [source tree description](https://docs.mesa3d.org/sourcetree.html).
6. [LLVM: Using the New Pass Manager](https://llvm.org/docs/NewPassManager.html).
7. [LLVM: Writing an LLVM Pass](https://llvm.org/docs/WritingAnLLVMNewPMPass.html).
8. [LLVM Language Reference](https://llvm.org/docs/LangRef.html).
9. [LLVM IR Undefined Behavior Manual](https://llvm.org/docs/UndefinedBehavior.html).
10. [LLD: ELF, COFF and Wasm linker design](https://lld.llvm.org/NewLLD.html).
11. [Clang: ThinLTO](https://clang.llvm.org/docs/ThinLTO.html).
12. [MLIR: Pass Infrastructure](https://mlir.llvm.org/docs/PassManagement/).
13. [LLVM Target-Independent Code Generator](https://llvm.org/docs/CodeGenerator.html).
14. [Vulkan specification: Synchronization and Cache Control](https://docs.vulkan.org/spec/latest/chapters/synchronization.html).
15. [Vulkan specification: Memory Allocation](https://docs.vulkan.org/spec/latest/chapters/memory.html).
16. [Alive2 author repository](https://github.com/AliveToolkit/alive2).
