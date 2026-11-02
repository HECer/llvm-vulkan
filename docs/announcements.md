# LLVM Vulkan Publication Kit & Community Announcements

Ready-to-post announcements tailored for relevant compiler, systems programming, and graphics communities. Each draft highlights the technical architecture, reproducible benchmarks (including honest transparency where CPU beats GPU), live links, and release artifacts.

---

## 1. Reddit: r/programming & r/Compilers

**Post Title**:  
`LLVM Vulkan 1.0: Offloading compiler passes to Vulkan compute with reproducible benchmarks (and where CPU still wins)`

**Suggested Flairs**: `Project` / `C++` / `Compilers`

**Body Markdown**:
```markdown
Hey r/programming,

I've just released **LLVM Vulkan 1.0**, an open-source project exploring whether selected compiler optimization calculations can be accelerated using standard, portable Vulkan compute shaders while preserving host LLVM semantic authority:

- **Live Project Site**: https://hecer.github.io/llvm-vulkan/
- **GitHub Repository**: https://github.com/HECer/llvm-vulkan
- **Interactive Benchmark Dashboard (109 rows)**: https://hecer.github.io/llvm-vulkan/graphs/benchmarks.html
- **12-Slide Offline Project Presentation**: https://hecer.github.io/llvm-vulkan/project-presentation.html
- **GitHub Release v1.0.0**: https://github.com/HECer/llvm-vulkan/releases/tag/v1.0.0

### What it actually does
This is not a full port of LLVM to GPU, nor a claim that compilation is always faster. Rather, it implements a standalone C++17 bulk-compute library (`LLVMVulkan::compute`) and connected LLVM passes:
1. `lv-fold`: Batched integer arithmetic and bitwise folding through 64 bits (`shaderInt64`).
2. `lv-cse`: Instruction record hashing for Common Subexpression Elimination (exact equality and dominance remain on the host).
3. `lv-cfg`: CFG graph reachability using CSR format, dense dominator sets, and gen/kill liveness analysis.

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
Transparency was our top priority:
- **Small Workloads**: At 1,024 elements, the CPU wins every single benchmark due to transfer and dispatch overhead.
- **Sparse/Long-chain Graphs**: On mixed long-chain graphs, the CPU was up to **151× faster** (0.064 ms vs 9.71 ms) because worklist algorithms beat GPU fixed convergence rounds.
- **Prefix Scans**: CPU L1/L2 cache locality outperforms GPU roundtrips at array sizes under 1M items.

### Package & Offline Docs
- Self-contained, relocatable CMake package (`LLVMVulkan::compute`) with embedded SPIR-V and mandatory synchronization validation.
- All presentation slides and benchmark graphs are fully self-contained HTML files that run completely offline without an external server.

Feedback, benchmark reproductions, and thoughts on work-efficient GPU graph frontiers are very welcome!
```

---

## 2. Reddit: r/vulkan

**Post Title**:  
`LLVM Vulkan 1.0: Using Vulkan 1.1 compute shaders & subgroup operations to accelerate compiler algorithms`

**Suggested Flair**: `Compute` / `Project`

**Body Markdown**:
```markdown
Hi r/vulkan,

I wanted to share **LLVM Vulkan 1.0**, an open-source project that connects Vulkan compute shaders directly to LLVM optimization passes:

- **Repository**: https://github.com/HECer/llvm-vulkan
- **Live Demo & Benchmarks**: https://hecer.github.io/llvm-vulkan/
- **Interactive Graphs**: https://hecer.github.io/llvm-vulkan/graphs/benchmarks.html

### Vulkan Pipeline Details
- **Baseline**: Targeted at Vulkan 1.1 with fixed 128-thread workgroups and embedded, pre-validated SPIR-V shaders (`spirv-val`).
- **Subgroup Acceleration**: When `VK_EXT_subgroup_size_control` and `subgroup_arithmetic` are available, exclusive scans and histogram reductions utilize fast on-chip ballot/shuffle operations.
- **Memory Management**: Employs pooled device buffers and persistent mappings to amortize allocation overhead across passes.
- **Validation**: Enforces synchronization validation (`VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT`) across all regression tests and large random graph dispatches (299k assertions per seed).
- **Fallbacks**: Tested on both physical discrete GPUs (NVIDIA RTX 5090) and software rasterizers (Mesa `lavapipe` with LLVM 21.1.8).

Would love any thoughts from the compute community on pipeline pooling and optimizing convergence rounds for graph traversal!
```

---

## 3. Reddit: r/cpp

**Post Title**:  
`LLVM Vulkan: A relocatable C++17 bulk-compute library and scoped LLVM pass plugin`

**Suggested Flair**: `Show / Project`

**Body Markdown**:
```markdown
Hello r/cpp,

I've released version 1.0 of **LLVM Vulkan**, a C++17 library and plugin that offloads bulk compiler primitives to Vulkan compute:

- **Site & Presentation**: https://hecer.github.io/llvm-vulkan/
- **GitHub**: https://github.com/HECer/llvm-vulkan
- **Release 1.0.0**: https://github.com/HECer/llvm-vulkan/releases/tag/v1.0.0

### Modern C++ & CMake Features
- **Modern CMake Target**: Exports relocatable `LLVMVulkan::compute` static library and headers with `find_package(LLVMVulkan)` support.
- **Zero Heavy Dependencies**: Pure Vulkan C API + C++17 standard library. No CUDA, OpenCL, or vendor-locked SDKs required.
- **Header Embedding**: SPIR-V compute bytecode is validated at build time and embedded into C++ headers via custom targets.
- **Tested Consumer**: Verified via external CMake consumer builds after package relocation on Windows (MSVC) and Linux (GCC 15 / Clang).

Detailed documentation and benchmark data: https://hecer.github.io/llvm-vulkan/
```

---

## 4. Hacker News (Show HN)

**Submission URL**: `https://hecer.github.io/llvm-vulkan/`  
**Submission Title**:  
`Show HN: LLVM Vulkan – Portable compute and scoped LLVM passes with benchmarks`

**Top Comment / First Post**:
```text
Hi HN,

I built LLVM Vulkan (https://github.com/HECer/llvm-vulkan) to explore offloading batch compiler calculations (integer folding, instruction hashing for CSE, and CFG reachability/dominators) to Vulkan compute shaders without giving up host LLVM semantic authority.

A few engineering highlights:
1. Honest Benchmarking: On an RTX 5090 vs Ryzen 9 5950X (1M elements), radix sort is 33.59x faster on GPU and 32-bit folding is 3.04x faster. However, on long-chain graphs, the CPU is up to 151x faster due to worklists outperforming GPU convergence passes. All 109 raw measurement rows are explorable in an interactive dashboard: https://hecer.github.io/llvm-vulkan/graphs/benchmarks.html
2. Zero Server Dependency: Both the 12-slide project presentation and the benchmark explorer are self-contained HTML files that function completely offline.
3. Architecture: Conservative CPU defaults with explicit Vulkan offload; memory pooling with persistent mappings; and mandatory Vulkan synchronization validation in CI and test suites.

Code, docs, and relocatable v1.0.0 packages: https://github.com/HECer/llvm-vulkan/releases/tag/v1.0.0

Looking forward to your questions and feedback!
```

---

## 5. LLVM Discourse (discourse.llvm.org)

**Category**: `Community` or `Projects`  
**Title**:  
`LLVM Vulkan 1.0: Experimental bulk-compute API and scoped LLVM optimization passes`

**Body**:
```text
Hi everyone,

I'd like to share a project exploring portable Vulkan compute shaders connected to LLVM optimization passes:

- Project site: https://hecer.github.io/llvm-vulkan/
- Repository: https://github.com/HECer/llvm-vulkan
- Release notes: https://github.com/HECer/llvm-vulkan/releases/tag/v1.0.0

Summary of the design:
- Host Semantic Authority: The host LLVM passes (`lv-fold`, `lv-cse`, `lv-cfg`) retain strict IR mutation authority. The Vulkan compute API processes bulk numerical or reachability arrays, while verification, dominance checks, and poison handling stay on the host.
- Standalone C++17 compute library: Includes integer folding (up to 64-bit), CSR graph reachability, dominators, liveness, unsigned scan/sort, and record hashing.
- Testing & Reproducibility: Includes 17 IR fixtures per mode, 8,192 constant oracle tests, differential testing against software Vulkan (lavapipe), and RTX 5090 hardware benchmarks.

We've detailed the scope, contract, and known limits in coverage.md (https://github.com/HECer/llvm-vulkan/blob/main/docs/coverage.md) and release.md (https://github.com/HECer/llvm-vulkan/blob/main/docs/release.md).

We welcome discussion from community members working on compiler offloading and bulk-compute passes!
```
