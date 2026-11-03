# LLVM Vulkan Publication Kit & Community Announcements

Ready-to-post announcements tailored for compiler, systems programming, and graphics communities. Each draft highlights the technical architecture, reproducible benchmarks (including honest transparency where CPU beats GPU), drop-in standard pass aliasing, Clang `-O2` integration, live links, and release artifacts.

---

## 1. Reddit: r/programming & r/Compilers

**Post Title**:  
`LLVM Vulkan 1.1: Drop-in compiler passes (GVN, CSE, CFG, DCE) on Vulkan compute with benchmarks & Clang integration`

**Suggested Flairs**: `Project` / `C++` / `Compilers`

**Body Markdown**:
```markdown
Hey r/programming and r/Compilers,

I've just released **LLVM Vulkan 1.1**, an open-source project that moves beyond an experimental prototype into a **drop-in pass plugin** that accelerates LLVM middle-end passes using portable Vulkan compute shaders:

- **Live Project Site**: https://hecer.github.io/llvm-vulkan/
- **GitHub Repository**: https://github.com/HECer/llvm-vulkan
- **GitHub Release v1.1.0**: https://github.com/HECer/llvm-vulkan/releases/tag/v1.1.0
- **Interactive Benchmark Dashboard (109 rows)**: https://hecer.github.io/llvm-vulkan/graphs/benchmarks.html
- **12-Slide Offline Project Presentation**: https://hecer.github.io/llvm-vulkan/project-presentation.html

### What it actually does
Rather than claiming to rewrite LLVM or port the entire backend to GPU, LLVM Vulkan provides a standalone C++17 bulk-compute library (`LLVMVulkan::compute`) and an LLVM pass plugin with **strict host semantic authority**:
1. **GPU-Accelerated GVN (`gvn` / `lv-gvn`)**: Computes dense dominator trees on GPU workgroups to eliminate redundant expressions across basic blocks dominated by the candidate.
2. **Commutative CSE (`early-cse` / `lv-cse`)**: Normalizes operand IDs for commutative operations (`add`, `mul`, `and`, `or`, `xor`, `eq`, `ne`) before GPU record hashing, deduplicating reversed expressions like `a + b` and `b + a`.
3. **Control Flow Graph Simplification (`simplifycfg` / `lv-cfg`)**: High-throughput reachability closure over module-wide control flow graphs.
4. **Dead Code Elimination (`dce` / `adce` / `lv-dce`)**: Backward def-use reachability closure, salvaging debug records and dropping dead SSA cycles.
5. **Integer Constant Folding (`instcombine` / `lv-fold`)**: Batched modular integer arithmetic and bitwise folding through 64 bits (`shaderInt64`).

### Quick Drop-in Usage with Clang and opt
```bash
# Direct opt pipeline using standard pass aliases:
opt -load-pass-plugin=LLVMVulkan.so -passes='early-cse,gvn,simplifycfg,dce' input.ll -S -o output.ll

# Automatic Clang -O2 integration via extension points:
export LV_AUTO_INSERT=1
clang -fpass-plugin=LLVMVulkan.so -O2 -c main.c -o main.o
```

### The Numbers: RTX 5090 vs Ryzen 9 5950X (1,048,576 elements)
We measured complete host wall times including upload, GPU dispatch, convergence, synchronization, and host readback:

| Workload | Ryzen 9 5950X | RTX 5090 | Speedup |
| :--- | :--- | :--- | :--- |
| **Sort keys** (radix sort vs `std::sort`) | 55.96 ms | 1.67 ms | **33.59×** |
| **Integer fold** (32-bit scalar batches) | 12.66 ms | 4.17 ms | **3.04×** |
| **Integer fold** (64-bit) | 16.18 ms | 7.10 ms | **2.28×** |
| **Hash records** (instruction tuples) | 3.33 ms | 2.65 ms | **1.26×** |
| **Exclusive scan** (prefix sums) | 1.28 ms | 1.47 ms | *0.87× (CPU wins)* |

### Where the CPU Still Wins
Transparency is our top priority:
- **Small Workloads**: At 1,024 elements, the CPU wins every benchmark due to kernel launch and transfer latency. The plugin defaults to scalar CPU unless `LV_BACKEND=vulkan` or automatic threshold batching is enabled.
- **Sparse / Long-Chain Graphs**: On long single-chain graphs, the CPU is up to **151× faster** (0.064 ms vs 9.71 ms) because worklist algorithms beat GPU fixed convergence rounds.
- **Memory SSA / Pointer Aliasing**: The plugin deliberately handles pure SSA values, leaving complex memory disambiguation and pointer aliasing to upstream LLVM.

### Testing & Packages
- Verified against LLVM 21 and pinned LLVM main: 19/19 fixtures pass, 8,192 constant oracle tests pass, and semantic fuzzing passes.
- Relocatable packages for Windows (`.zip`) and Linux (`.tar.gz`) available on the GitHub Release page.
- Self-contained offline HTML presentation and interactive benchmark explorer.

Feedback, benchmark reproductions, and thoughts on GPU-accelerated compiler passes are very welcome!
```

---

## 2. Hacker News (Show HN)

**Submission URL**: `https://hecer.github.io/llvm-vulkan/`  
**Submission Title**:  
`Show HN: LLVM Vulkan 1.1 – Drop-in LLVM Passes (GVN, CSE, CFG, DCE) on Vulkan Compute`

**Top Comment / First Post**:
```text
Hi HN,

I built LLVM Vulkan (https://github.com/HECer/llvm-vulkan) to explore accelerating compiler middle-end optimization passes using portable Vulkan 1.1 compute shaders, while maintaining strict host LLVM semantic authority.

With version 1.1.0, it moves from a prototype to a drop-in replacement pipeline:
- Standard pass aliasing: Works directly with `opt -passes='early-cse,gvn,simplifycfg,dce'`.
- Automatic Clang hook-in: Setting `LV_AUTO_INSERT=1` injects Vulkan passes into `clang -fpass-plugin=LLVMVulkan.so -O2`.
- GPU Dominator GVN: Uses GPU dense dominator matrix solvers (`dominators_rounds.comp`) to eliminate redundant expressions across basic blocks dominated by the definition.
- Commutative normalization: Canonically sorts operands for commutative ops so `a + b` and `b + a` match identically.

Honest Benchmarking (RTX 5090 vs Ryzen 9 5950X at 1M items):
- Radix sort: 33.59x faster on GPU (1.67 ms vs 55.96 ms).
- 32-bit folding: 3.04x faster on GPU (4.17 ms vs 12.66 ms).
- Where CPU wins: At small sizes (1,024 elements) or sparse long-chain graphs, CPU worklists outperform GPU convergence rounds by up to 151x.

Interactive benchmark dashboard (109 rows): https://hecer.github.io/llvm-vulkan/graphs/benchmarks.html
12-slide offline presentation: https://hecer.github.io/llvm-vulkan/project-presentation.html
Release v1.1.0 packages: https://github.com/HECer/llvm-vulkan/releases/tag/v1.1.0

Happy to answer any questions about the Vulkan shaders, SPIR-V validation, or LLVM pass integration!
```

---

## 3. Reddit: r/cpp

**Post Title**:  
`LLVM Vulkan 1.1: A relocatable C++17 compute library and drop-in LLVM pass plugin`

**Suggested Flair**: `Show / Project`

**Body Markdown**:
```markdown
Hello r/cpp,

I've released version 1.1 of **LLVM Vulkan**, a modern C++17 library and LLVM pass plugin that offloads bulk compiler calculations to Vulkan compute:

- **Site & Presentation**: https://hecer.github.io/llvm-vulkan/
- **GitHub**: https://github.com/HECer/llvm-vulkan
- **Release 1.1.0**: https://github.com/HECer/llvm-vulkan/releases/tag/v1.1.0

### Highlights in 1.1
- **Modern C++17 Architecture**: Zero vendor lock-in; relies purely on standard Vulkan 1.1 and C++17. No CUDA or proprietary toolchains needed.
- **Relocatable CMake Target**: Exports `LLVMVulkan::compute` with relocatable CMake package configuration files, public headers, and embedded SPIR-V bytecode.
- **Drop-in Standard Passes**: Registers standard LLVM pass names (`early-cse`, `gvn`, `simplifycfg`, `dce`, `adce`, `instcombine`) so you can load it in `opt` or `clang` without custom pipeline strings.
- **Automatic Clang Integration**: Injects into Clang `-O2`/`-O3` optimization pipelines via EP callbacks (`LV_AUTO_INSERT=1`).
- **GPU Dominators & GVN**: Implements dominator-tree-based Global Value Numbering computed via parallel GPU bitset matrices.

Complete benchmarks, slides, and source code are available at https://hecer.github.io/llvm-vulkan/.
```

---

## 4. Reddit: r/vulkan

**Post Title**:  
`LLVM Vulkan 1.1: Accelerating compiler passes with Vulkan 1.1 compute shaders & subgroup operations`

**Suggested Flair**: `Compute` / `Project`

**Body Markdown**:
```markdown
Hi r/vulkan,

Version 1.1 of **LLVM Vulkan** is now live: an open-source project using Vulkan compute shaders to accelerate compiler optimization passes:

- **Repository**: https://github.com/HECer/llvm-vulkan
- **Release v1.1.0**: https://github.com/HECer/llvm-vulkan/releases/tag/v1.1.0
- **Interactive Graphs**: https://hecer.github.io/llvm-vulkan/graphs/benchmarks.html

### Vulkan Implementation Details
- **Shader Pipeline**: Written in GLSL, validated with `glslangValidator` and `spirv-val`, and compiled into embedded C++ SPIR-V bytecode.
- **GPU Solvers**:
  - `dominators_local_banked.comp` & `dominators_rounds.comp`: Solve dense dominator bitsets using workgroup shared memory and iterative convergence.
  - `radix_histogram.comp` & `radix_scatter.comp`: Fast 4-pass 8-bit radix sorting for large key batches.
  - `scan_subgroup.comp`: Hardware-accelerated subgroup arithmetic operations (`subgroupBallot`, `subgroupInclusiveAdd`) with fallback to workgroup shared-memory scan.
- **Synchronization & Verification**: Tested with mandatory synchronization validation (`VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT`) across both hardware GPUs (RTX 5090) and CPU software Vulkan (Mesa lavapipe).
```

---

## 5. LLVM Discourse (discourse.llvm.org)

**Category**: `Community` or `Projects`  
**Title**:  
`LLVM Vulkan 1.1: Drop-in middle-end passes (GVN, CSE, CFG, DCE) using Vulkan compute`

**Body**:
```text
Hi everyone,

I'd like to share LLVM Vulkan 1.1, an open-source project exploring whether standard Vulkan compute shaders can be used as an acceleration layer for LLVM optimization passes without compromising LLVM's host semantic authority.

- Project site: https://hecer.github.io/llvm-vulkan/
- GitHub: https://github.com/HECer/llvm-vulkan
- Release 1.1.0: https://github.com/HECer/llvm-vulkan/releases/tag/v1.1.0

Summary of the design and updates in 1.1:
- Host Semantic Authority: The host LLVM pass plugin retains exclusive IR mutation authority. The GPU computes candidate hashes, integer arithmetic, and CFG reachability/dominator matrices. LLVM verifies semantic equivalence on the host before replacement.
- Standard Pass Aliasing: Registered as drop-in aliases for `early-cse`, `gvn`, `simplifycfg`, `dce`, and `instcombine`.
- Clang Integration: Supports automatic pass injection via extension points (`registerPipelineStartEPCallback`, `registerPipelineEarlySimplificationEPCallback`, `registerOptimizerLastEPCallback`) when compiled with `clang -fpass-plugin=LLVMVulkan.so -O2` and `LV_AUTO_INSERT=1`.
- GPU Dominator GVN: Connects the dense dominator matrix compute shaders to `GVNPass` to eliminate redundant expressions across basic blocks.
- Tested & Validated: 19 test fixtures, 8,192 constant oracle tests, and differential testing against LLVM 21 with both physical GPU and software Vulkan (lavapipe).

We have documented the architecture and limitations in coverage.md and integration.md. We'd love feedback from the community!
```

---

## 6. X / Twitter Announcement Thread

**Tweet 1 (Hook)**:
> Excited to release LLVM Vulkan 1.1! 🚀
> 
> Can we accelerate compiler optimizations with Vulkan compute shaders?
> 
> Version 1.1 brings drop-in standard passes (`early-cse`, `gvn`, `simplifycfg`, `dce`), GPU dominator-tree GVN, and automatic `clang -O2` integration.
> 
> Thread 👇
> https://hecer.github.io/llvm-vulkan/

**Tweet 2 (How it works)**:
> How it works:
> - Strict host authority: LLVM retains IR mutation and semantic checks.
> - Vulkan compute evaluates batch arithmetic (`shaderInt64`), instruction hashing, and dense CFG dominator matrices.
> - Expressions like `a + b` and `b + a` are canonicalized and eliminated across blocks.

**Tweet 3 (Honest Benchmarks)**:
> Transparent benchmarks on RTX 5090 vs Ryzen 9 5950X (1M elements):
> ⚡ Radix sort: 33.6x faster on GPU
> ⚡ Integer fold: 3.0x faster on GPU
> 🐢 Where CPU wins: At <1,024 items or sparse long-chain graphs, CPU worklists are up to 151x faster!
> 
> Explore all 109 rows: https://hecer.github.io/llvm-vulkan/graphs/benchmarks.html

**Tweet 4 (Links & Download)**:
> Zero CUDA dependencies. Open source under Apache-2.0 WITH LLVM-exception.
> 
> 📦 Release v1.1.0 binaries & source: https://github.com/HECer/llvm-vulkan/releases/tag/v1.1.0
> 📽️ 12-slide interactive deck: https://hecer.github.io/llvm-vulkan/project-presentation.html
> ⭐ Star on GitHub: https://github.com/HECer/llvm-vulkan
