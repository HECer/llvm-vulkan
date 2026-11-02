# LLVM GPU coverage and remaining work

**Version 1.0 covers a defined subset. This repository is an acceleration layer for
selected LLVM operations. It is not a complete LLVM port, a Rust rewrite of
LLVM, or a validated faster compiler. RTX 5090 core execution is validated;
physical AMD/Intel testing and whole-compiler GPU measurements remain outstanding.**

The implementation retains LLVM's C++ IR ownership, parser, pass manager,
legality checks and mutation utilities. Vulkan compute performs selected bulk
calculations. This makes it possible to compare the same operation through an
ordinary CPU implementation and an actual Vulkan execution path. It does not
make every LLVM pass execute on the GPU.

## Exact source baseline

The default source target is LLVM main commit
[`b8ef6b0ed582fb5a235e7989685f5b188c620dc1`](https://github.com/llvm/llvm-project/commit/b8ef6b0ed582fb5a235e7989685f5b188c620dc1),
observed on **2026-11-01**, with `LLVM_VERSION_MAJOR=24`, minor `0`, patch `0`,
suffix `git`. The latest stable release observed on that date is
[`llvmorg-23.1.4`](https://github.com/llvm/llvm-project/releases/tag/llvmorg-23.1.4),
published 2026-10-20. Both exact commits, version-file hashes and archive hashes
are recorded in [`upstream-lock.json`](../upstream-lock.json). Main is deliberately
pinned; "latest" does not mean silently changing the source at every build.

The complete main archive was downloaded, checksum recorded, safely extracted,
and inventoried. The source inventory contains **187,974 non-hidden regular
files**, **27 top-level component groups including root files**, **449 textual
core new-pass-manager registrations**, and **25 target backend directories**.
It also identifies 45 files containing syntactic TableGen pass declarations.
These are enumeration results, not a claim that every line was semantically
reviewed or translated. The core registry also excludes some legacy,
generated, target-specific, MLIR and downstream passes.

Reproduce the inventory after fetching the pinned source:

```sh
python3 scripts/fetch_llvm.py --destination third_party/llvm-project
python3 scripts/inventory_llvm.py --source third_party/llvm-project \
  --output docs/upstream-inventory.json
```

[`upstream-inventory.json`](upstream-inventory.json) lists every discovered core
registration. All entries retain `upstream_pass_ported: false`: a small, safe
constant folder is not a port of all of InstCombine, and a graph traversal is
not a port of every pass that uses graphs.

## Implemented interfaces and exact boundaries

| Area | Implemented work | Integration boundary and omissions |
| --- | --- | --- |
| Vulkan runtime | Native compute dispatch, storage buffers, SPIR-V shader pipelines, explicit CPU/GPU device selection, limits and failures | The 1.0 runtime has a defined support matrix; other drivers and physical GPU performance require their own validation. A required Vulkan backend must not silently claim success through scalar execution. |
| Integer evaluation | Batched scalar integer arithmetic and comparisons for widths 1 through 64, plus independent CPU references | `lv-fold` uses the original 32-bit kernel for i1–i32 and a separate `shaderInt64` kernel for i33–i64. Devices without that optional feature can still run the 32-bit kernel; an explicit nonempty Vulkan 64-bit request fails visibly if unsupported. APInt wider than 64 bits, vectors, floating point, pointers, constrained operations and poison-generating flags remain outside the accepted subset. Invalid shifts/division cases are retained. |
| Dependency closure | CSR graph reachability with seed flags | `lv-dce` marks dependencies of conservative live roots, then LLVM removes only eligible dead pure SSA instructions. Calls, memory operations, control flow and EH are retained. This is not full ADCE or dead-store elimination. |
| CFG reachability | Reachability of basic blocks | `lv-cfg` first uses LLVM's host `ConstantFoldTerminator` for constant branch/switch and PHI repair, then offloads reachability and removes unreachable blocks while preserving address-taken roots. It is not a replacement for all of SimplifyCFG, jump threading, loop restructuring or branch-probability analysis. |
| Local equivalence candidates | Parallel record hashing | `lv-cse` uses hashes to find candidates within one basic block and verifies exact LLVM instruction identity on the CPU. It excludes memory, PHIs, freeze, flags and metadata-sensitive cases. Hash equality alone never proves equivalence. |
| Dominator sets | CPU and Vulkan dense bitset solver | Standalone primitive only. LLVM's native DominatorTree analysis and its incremental updater do not consume this result. Dense set construction can be much less efficient than native algorithms. |
| Backward bitset data flow | CPU and Vulkan gen/kill liveness solver | Standalone primitive only; no claim that machine live intervals, MemorySSA, register allocation or full language analyses are ported. |
| Scan | Unsigned exclusive prefix scan | Standalone building block. It does not currently speed tokenization, object layout, compaction or serialization inside LLVM. |
| Sort | Unsigned key sorting | Standalone building block. It is not integrated into linker symbol resolution, register allocation or a complete GPU hash table. |
| Pass pipeline | `lv-fold`, `lv-cse`, `lv-dce`, `lv-cfg`, `lv-pipeline` | Opt-in new-PM plugin. CPU is the conservative default; explicit Vulkan selects real dispatch. Hardware speedup is unproven. Frontend and backend work remain upstream. |

The combined pipeline runs folding, local CSE, CFG cleanup, then dead-code
cleanup. Host-side LLVM utilities perform semantic edits; GPU kernels compute
the accepted arithmetic results, hashes and graph closure. Thread-local
per-backend contexts retain device/pipeline state within a process. Opt-in
automatic dispatch uses a configurable size cutoff; its default is an
uncalibrated policy value, not a measured RTX 5090 crossover point.

The fold implementation bounds successive dependency frontiers. A chain deeper
than the configured limit may be only partly simplified; that is a missed
optimization, not permission to fabricate a result. Unsupported inputs remain
for LLVM's standard passes. See source comments and the tests for the exact
accepted subset; this table does not expand it.

## Compiler-stage opportunity map

The following judgments are engineering hypotheses based on the upstream
architecture and the source inventory. **Candidate** means there is an
identifiable parallel operation worth profiling, not that a speedup has been
measured. Unimplemented entries are intentionally explicit.

| Stage or subsystem | Potential GPU work | Main difficulty | Current status |
| --- | --- | --- | --- |
| Source input and dependency scanning | File hashes, delimiter classification, batched include lookup | Filesystem latency, include ordering and small tasks | Not integrated |
| Lexing and preprocessing | Byte classification, prefix scans, independent token chunks | Comments/strings cross chunks; macros alter future token streams | Not implemented |
| Parsing | Parallel grammar recognition, tree construction for restricted grammars | Full C++/Fortran grammar and recovery are not a flat array problem | Not implemented |
| Name lookup and template instantiation | Bulk candidate lookup, independent instantiations | Context-sensitive lookup, substitutions, diagnostics, recursive dependencies | Not implemented |
| Type checking and constant expressions | Batched constraints and homogeneous arithmetic | APInt/APFloat, target layout and language-specific evaluation semantics | Not implemented |
| IR construction and SSA | ID assignment, prefix allocation, bulk edge construction | Concurrent mutation of owned IR objects and metadata | CPU/upstream |
| IR verification | Local type/operand checks, graph properties | Whole-program structural invariants and deterministic diagnostics | CPU/upstream verifier retained |
| Bitcode/textual IR I/O | Checksums, block decoding, bulk record packing | Abbreviations, stream dependencies, allocation and I/O | Not implemented |
| Arithmetic constant folding | Many independent pure scalar operations | Packing overhead and dependency depth | Narrow integrated subset |
| Instruction combining | Candidate classification, pattern matching batches | Thousands of legality-sensitive rewrites with cascading mutations | Not ported |
| CSE/GVN | Hashing, equality buckets, value-number refinement | Dominance, memory effects, poison and congruence-class convergence | Local pure CSE subset only |
| SCCP/value ranges/known bits | Monotone bitset or lattice transfer | Executable-edge tracking, arbitrary bit widths, convergence | Not implemented |
| CFG reachability | Frontier propagation across many functions | Low graph diameter versus repeated launches; pointer-to-ID conversion | Integrated subset |
| Dead-code elimination | Reverse def-use reachability | Root semantics, control dependencies and debug information | Conservative pure-SSA subset |
| Dead stores and memory optimization | Alias candidate filtering, interval overlap | MemorySSA, atomics, volatile, lifetime, ordering and exceptions | Not implemented |
| Dominance/post-dominance | Dense bitset intersections across batches | Quadratic memory; strong sparse/incremental CPU baselines | Standalone dominator primitive |
| Loops and SCCs | SCC partitioning, loop membership, graph frontiers | Irreducible flow and incremental updates | Not implemented |
| Liveness and reaching definitions | Bulk gen/kill bitset equations | Sparse live ranges, PHI edge semantics, iteration counts | Standalone generic liveness primitive |
| Alias and points-to analysis | Constraint propagation, large set unions | Context sensitivity, enormous relation sizes, conservative legality | Not implemented |
| Interprocedural summaries | Call-graph reachability, summary aggregation, bitset propagation | Linkage, indirect calls, COMDAT, visibility, recursion | Not implemented |
| Inlining and function specialization | Batched cost estimates and feature evaluation | Inlining changes both graph and future costs | Not implemented |
| Loop transformations and SCEV | Repeated dependence tests, affine constraints | Symbolic recurrences, exact overflow rules, mutation and code size | Not implemented |
| Vectorization | Dependence candidate filtering, pack scoring, cost queries | Target legality and interdependent choices | Not implemented |
| PGO/sample data | Histograms, merges, checksums and large table joins | Profile compatibility, stable summation and I/O | Not implemented |
| ML advice and IR embeddings | Batched model inference and embedding reductions | Model loading/dispatch can dominate small functions | Not implemented |
| ThinLTO/full LTO | Whole-program graph summaries, large cross-module batches | Existing CPU parallelism and caches are strong baselines | Not integrated |
| Instruction selection/legalization | Independent pattern scoring and table lookup | Target constraints, DAG rewrites and divergent opcode mixes | Not implemented |
| Scheduling | Dependency/frontier analysis and batch candidate scoring | Critical-path decisions are sequential within a region | Not implemented |
| Register allocation | Live-range intersections and interference edges | Spill decisions feed back into code and intervals | Not implemented |
| Machine peepholes and branch relaxation | Bulk local candidates and offset computation | Target-specific semantics; relaxation changes layout | Not implemented |
| Assembly/MC/object emission | Encoding batches, checksums, section scans | Fixups, target relocation semantics and serial output order | Not implemented |
| Linker symbol handling | Hashes, sorting, string equality, relocation classification | Resolution precedence and file-format differences | Not integrated |
| Linker GC and identical-code folding | Reachability, partition refinement, section fingerprints | Relocations and COMDAT/address-significance semantics | Not implemented |
| Linker layout and relocation | Prefix scans, independent relocations, compression | Alignment, relaxation and format-specific side effects | Not implemented |
| Debug data | Hashing/deduplication, compression, table joins | DWARF/PDB identity, references and deterministic output | Not implemented |
| JIT and ORC | Batch independent materializations or large queries | Short latency budgets, runtime symbol resolution, executable memory | Not integrated |
| Binary optimization/BOLT | Profile aggregation, CFG analysis, layout scoring | Decode, relocation repair and profile-guided iterative decisions | Not implemented |
| Testing/build orchestration | Parallel differential cases and corpus reduction | Process startup, output validation and build dependency graph | CPU harness; not GPU compiler work |
| Cache/CAS/compression | Bulk content hashing, chunking, dictionary operations | Reuse may eliminate work more cheaply than offloading it | Not integrated |

The code generator has distinct selection, scheduling, register allocation and
emission stages; those are not covered by an IR-only plugin. The current source
contains target directories for AArch64, AMDGPU, ARC, ARM, AVR, BPF, CSKY,
DirectX, Hexagon, Lanai, LoongArch, M68k, MSP430, Mips, NVPTX, PowerPC, RISCV,
SPIRV, Sparc, SystemZ, VE, WebAssembly, X86, XCore and Xtensa. No backend-specific
port is claimed. See the upstream [code-generator description](https://llvm.org/docs/CodeGenerator.html)
and the generated inventory.

## Monorepo coverage

| Component groups present in the pinned tree | Treatment |
| --- | --- |
| `llvm` | Selected IR plugin plus standalone primitives; remaining libraries are inventoried and remain upstream. |
| `clang`, `clang-tools-extra` | No frontend, clangd, clang-tidy, indexer, analyzer or formatting port. A compatible Clang may load the plugin through its pass-plugin interface; that does not move its frontend onto the GPU. |
| `flang` | No parser, semantic analysis, lowering or FIR/HLFIR pass port. An LLVM IR bridge is possible but requires separate compiler integration and tests. |
| `mlir`, `polly` | No dialect, rewrite engine, polyhedral optimizer or affine solver port. GPU dialect code generation describes programs targeting GPUs, not execution of the compiler on one. |
| `lld`, `bolt` | No native linker or binary-optimizer integration. General hashes/graphs/scans are insufficient to claim those integrations. |
| `lldb` | Debugger indexing or symbol lookup could have batch candidates; interactive control and process I/O remain outside this implementation. |
| `compiler-rt`, `libc`, `libcxx`, `libcxxabi`, `libunwind`, `llvm-libgcc`, `flang-rt`, `orc-rt` | Target/runtime libraries are not compile-time GPU implementations. They are not ported here. |
| `libclc`, `libsycl`, `openmp`, `offload` | These contain GPU-related language/runtime infrastructure; their existence does not mean LLVM's own optimizers run on a GPU. No port here. |
| `cmake`, `runtimes`, `cross-project-tests`, `third-party`, `utils`, root files | Build/support/test/dependency material. Inventory and reproducible fetch cover their presence; no wholesale translation is claimed. |

Rust, Swift, Julia and other LLVM clients do **not** automatically gain GPU
compilation by installing this plugin. They must expose an appropriate pass hook,
use a compatible LLVM build, or explicitly pass supported LLVM IR through `opt`.
Frontend work, client-specific LLVM forks, JIT constraints and ABI compatibility
remain separate integration work.

## Completion criteria before stronger claims

1. Build and test against the pinned LLVM source; record exactly which binaries,
   targets and tests completed. Building the plugin is not a whole-monorepo test.
2. Run the same SPIR-V paths under lavapipe and a physical RTX 5090 with Vulkan
   validation, CPU differential checks and LLVM verification enabled.
3. Compare program behavior, diagnostics and object output where appropriate;
   valid IR alone does not prove semantic equivalence.
4. Measure cold process time, warm device time, packing, transfer, dispatch,
   synchronization, readback and mutation separately. Also compare ordinary LLVM
   passes and realistic full builds, not just a scalar reference microbenchmark.
5. Broaden accepted semantics only with adversarial regressions and, where
   practical, translation validation. Preserve deterministic behavior.
6. Integrate additional primitives into real consumers only when measurements
   justify their transfer, memory and maintenance cost. Keep unprofitable work
   on the CPU. Every proposed stage in the table needs its own evidence.

The complete LLVM translation requested by the long-term project remains open.
This coverage ledger makes that remaining scope inspectable instead of hiding
it behind a broad project name.
