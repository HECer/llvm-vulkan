# Validation record

Latest validation: **2026-11-02**. Status: **1.0 release validation; RTX 5090 core validated**.

This document separates real execution evidence from unsupported claims.
The implementation is a scoped acceleration library and LLVM pass plugin;
neither successful tests nor the source inventory constitute a full LLVM port.

## Version 1.0 release

The release adds unified version metadata, installable public headers and a
relocatable CMake package, required build-time SPIR-V validation, and offline
presentation/benchmark assets. Its supported scope is defined in
[release.md](release.md). The installed-consumer gate moves the installation
before building and running CPU/Vulkan code against it.

The current code is rebuilt for Windows/RTX and Linux/lavapipe, including the
matching LLVM 21 fixture/oracle/semantic gates. The original optimization
snapshot also has a successful
[GitHub LLVM 20/lavapipe run](https://github.com/HECer/llvm-vulkan/actions/runs/41367958383).
Exact release tests, source/binary hashes and artifact checks are recorded in
[release-1.0.0.json](../evidence/release-1.0.0.json). Earlier performance records
retain their original pre-release identities and labels.

## Vulkan optimization update

The second iteration passes all six native Windows core CTest gates and three
large seeded RTX 5090 runs, each with 299,119 assertions and synchronization
validation. Its allocation-failure test checks recovery after host preparation
throws while a submission is in flight. Mixed component tests include the large
component at different positions, unrooted/cyclic components and dispatch tiling.
Optimized and forced-fallback lavapipe runs pass 299,114 and 299,102 assertions.
Matching LLVM 21.1.8 passes the 17 fixtures in both shader modes, the 8,192-case
oracle and the 64-function/512-call semantic differential. An initial integrated
run was interrupted by a WSL VM restart; its incomplete LLVM portion was rerun
successfully, and the interrupted log is retained.

Six alternating timing rounds (90 warm samples per core row) and five graph
rounds (55 samples per graph case) compare with the archived first-iteration
runtime. Large arrays improve 1.18–1.44x; the selected mixed graphs improve
2.64–10.50x. Small-call regressions and inconclusive controls are also recorded.
See [the second iteration](performance-iteration2.md) and its
[full evidence](../evidence/vulkan-iteration2.json). These measurements concern
inclusive core API calls; physical AMD/Intel and whole-compiler gains are not
established.

The subsequent [fresh CPU/GPU benchmark](cpu-gpu-benchmark.md) completes 90
processes and 4,070 checked warm calls with no process retries. Its 37 rows
include CPU wins on all tested graphs and small inputs. It uses the exact
second-iteration executable; no additional core runtime changes were made.

### First iteration (historical)

The portable Vulkan update is validated on Windows 11 with an RTX 5090, driver
617.42, a Ryzen 9 5950X and an MSVC Release build. All five core CTest gates pass,
including forced baseline shader paths. Three seeded GPU runs with Khronos
validation and synchronization validation pass 299,084 assertions each,
including 8,388,609-element dispatch tests and more than 65,535 independent
graph workgroups.

The same implementation is built with GCC on Ubuntu 26.04 under WSL and tested
with Mesa lavapipe (LLVM 21.1.8). Optimized and forced-fallback paths pass with
synchronization validation. Lavapipe reports 8-lane subgroups; the RTX reports
32 lanes. Matching LLVM 21.1.8 tools and libraries also pass the 17 IR fixtures,
8,192-case constant oracle and dynamic semantic differential.

The saved baseline and optimized native executables are compared in alternating
order over three rounds and four sizes, with nine warm calls per round. Timing
includes host orchestration and current-input upload/readback; validation is
disabled for measurements. See [performance.md](performance.md) and the
[machine-readable update evidence](../evidence/vulkan-optimization.json).

Physical AMD/Intel GPUs and a fresh LLVM 24 source build of this update remain
untested. Core timings do not establish whole-compiler acceleration. The
remaining sections record the earlier source snapshot and its original Linux
development environment; its LLVM 20/24 results are historical evidence.

## Original development environment

| Item | Observed environment |
| --- | --- |
| OS | Ubuntu 24.04.3 LTS, x86-64 |
| Visible processor model | AMD EPYC 9V74 80-Core Processor |
| Container CPU quota | 8 CPU equivalents; the visible processor model is not an allocation of 80 cores |
| Container memory limit | 8 GiB |
| Vulkan device | `llvmpipe (LLVM 20.1.2, 256 bits)` through Mesa lavapipe |
| Physical GPU used | None |
| Initial LLVM package | Ubuntu LLVM 20.1.2 |
| Latest source target | LLVM 24.0.0git, exact commit in `upstream-lock.json` |
| Shader target | Vulkan 1.1 / SPIR-V, with optional `shaderInt64` for wide folding |

The Vulkan path really creates compute pipelines, uploads buffers, dispatches
SPIR-V, synchronizes and reads results back. Lavapipe implements those Vulkan
operations on the CPU. It does not emulate RTX timing, memory bandwidth,
subgroup scheduling or NVIDIA driver behavior.

## Core execution evidence

[core-differential.json](../evidence/core-differential.json) captures one scalar
run and three required Vulkan runs with seeds **5090, 1 and 12345**. Each run
includes 124,972 narrow folding requests, 167,200 wide folding requests, 4,000
cross-checks between wide and narrow arithmetic, 64 randomized graphs, cyclic
and disconnected examples, a 257-node dependency chain, scans, sorting,
variable-length hashing and invalid-input cases.

Ordinary Vulkan runs pass **296,447 assertions**. The run that additionally
exercises large dispatches passes **296,450 assertions**. An assertion is a
checked condition, not an independent program or coverage percentage.

The large case crosses **8,388,609** elements/nodes for both scan and
reachability. The executable is also run from an unrelated directory without
external shaders. Final runs enable the Khronos validation layer and
synchronization validation, with no recorded errors.

All nine shaders compile and pass SPIR-V validation. Formatting is separately
checked to preserve the tested shader binaries in
[shader-format-equivalence.json](../evidence/shader-format-equivalence.json).

[strict-gpu-selection.json](../evidence/strict-gpu-selection.json) records the
expected failure of a GPU-only request on the CPU-only machine. Reporting a
software device as an RTX result would fail this gate.

## Sanitizers

[core-sanitizers.json](../evidence/core-sanitizers.json) records successful
AddressSanitizer and UndefinedBehaviorSanitizer runs for the CPU and Vulkan
paths, including 64-bit folding.

**LeakSanitizer was not available.** It aborted under the environment's ptrace
restrictions, including for CPU-only execution. The final runs use
`detect_leaks=0` while retaining address/undefined-behavior checks. The failed
capability attempt is retained in
[leak-sanitizer-unavailable.json](../evidence/leak-sanitizer-unavailable.json).

## Integrated LLVM evidence

The initial LLVM 20 validation includes:

| Gate | Evidence and scope |
| --- | --- |
| Adversarial IR fixtures | 17 fixtures through both CPU and Vulkan; LLVM verification, expected/forbidden IR, identical backend output and execution checks where defined. [Report](../evidence/llvm20-fixtures.json) |
| Independent arithmetic oracle | 8,192 defined cases over 23 operation kinds and 13 widths through 64 bits. Expected constants come from upstream LLVM InstCombine, with the plugin's internal CPU verification disabled. [Report](../evidence/llvm20-constant-oracle.json) |
| Dynamic semantic differential | 64 functions, 512 calls and 513 full integer output lines; original, CPU-transformed and Vulkan-transformed programs agree. [Report](../evidence/llvm20-semantic-differential.json) |
| Clang frontend hook | Clang 20 `-O1 -g -fpass-plugin` compilation with actual Vulkan work; baseline and transformed native programs both print `3153175647`. [Report](../evidence/clang20-native-hook.json) |
| Upstream regression corpus | 120 valid fixtures from pinned-source DCE, EarlyCSE, SimplifyCFG and InstCombine suites pass verification and exact CPU/Vulkan IR parity; six LLVM-20-incompatible inputs are recorded separately. This structural gate never executes potentially undefined source fixtures. [Report](../evidence/llvm20-upstream-corpus.json) |

Fixtures cover scalar and wide arithmetic, poison/undef, noundef, assume,
freeze, invalid division and shifts, overflow/exact flags, odd integer widths,
live/dead PHIs, memory and calls, atomics, exception handling, debug salvage,
address-taken blocks, indirect branches, duplicate switch edges and `optnone`.
Mixed narrow/wide frontiers are included.

The final LLVM 20 CMake gate passes all seven tests: CPU core, Vulkan core,
benchmark correctness smoke, source-fetch safety, LLVM fixtures, independent
constant-fold oracle and dynamic semantic comparison. See
[cmake-llvm20.json](../evidence/cmake-llvm20.json).

### Pinned latest-source LLVM: completed

The complete pinned LLVM source was downloaded and the X86 toolchain was built
in Release mode with LLVM assertions enabled. `opt`, `lli`, `llc`,
`llvm-config`, and `libLLVM.so.24.0git` were built successfully: 2,290 primary
build steps plus five for the `llc` follow-up. All four executable version
checks report `24.0.0git`. The complete monorepo's other frontends/backends and
upstream test suites were not built or run. See
[llvm24-source-build.json](../evidence/llvm24-source-build.json).

The plugin was then compiled and linked against those exact source-built
headers and libraries. **All seven CMake gates pass on LLVM 24**, including the
17 integrated fixtures, 8,192 independent constant-fold comparisons and dynamic
semantic test. The CPU/Vulkan outputs and executable results agree. Evidence:

- [Final LLVM 24 CMake build and tests](../evidence/cmake-llvm24.json).
- [LLVM 24 adversarial fixtures](../evidence/llvm24-fixtures.json).
- [LLVM 24 independent constant oracle](../evidence/llvm24-constant-oracle.json).
- [LLVM 24 dynamic semantic differential](../evidence/llvm24-semantic-differential.json).

The bounded upstream corpus also passes on the new tools: **120 valid source
fixtures, zero skips, zero failures, 3,163 Vulkan dispatches**. These comprise
14 DCE, 35 EarlyCSE, 35 SimplifyCFG and 36 InstCombine fixtures. The test checks
verification and exact backend parity, not semantics of every upstream test:
[LLVM 24 corpus report](../evidence/llvm24-upstream-corpus.json).

The source build exposed the new `OptionalPassInfoMixin` interface; the plugin
now uses a compile-time feature probe for both this and LLVM 20's interface.

One build-tree detail is recorded without hiding the failure: `llvm-config`
shared-library queries expect `libLLVM-24git.so`, an alias created by the
upstream **installation** script. The uninstalled tree has
`libLLVM.so.24.0git`. Our CMake imported-target linkage works and is tested;
the shared-query helper fails in this uninstalled snapshot. No manual alias or
upstream patch was introduced. Install LLVM normally if another integration
requires those `llvm-config` shared-link flags.

## Historical software-only performance interpretation

No physical GPU performance was measured in that original environment. Local benchmark execution was used
to validate reporting and semantic checks. Timings collected while the large
LLVM source build was active are not treated as a stable performance baseline.

After the source build completed, a seven-sample run on the supplied synthetic
64-function, width-128 workload measured the following medians for `opt`, `llc`
and native linking together. The experiment includes newly started compiler
processes and validates both JIT and native outputs outside the timed region.

| Configuration | Median | Compared with ordinary upstream O2 |
| --- | ---: | --- |
| Upstream LLVM 24 O2 | 80.99 ms | Reference |
| CPU plugin prepasses + same O2 | 84.99 ms | About 4.9% slower |
| Vulkan on lavapipe + same O2 | 179.09 ms | About 2.21 times the elapsed time |

This **does not demonstrate acceleration**. It records the overhead in this
CPU-only environment and verifies that the benchmark can expose regressions.
It does not predict RTX performance or a realistic application's build time.
Raw data: [llvm24-pipeline-lavapipe.json](../evidence/llvm24-pipeline-lavapipe.json).
All eight individual benchmark workloads also pass their differential checks:
[CPU reference](../evidence/core-timings-cpu.json) and
[Vulkan CPU driver](../evidence/core-timings-vulkan.json).

`lv-bench` measures complete host API calls, including transfers and readback,
and reports first-call and repeated-call timings. Its host comparator is a
scalar reference. `benchmark_pipeline.py` measures separate full compiler
processes with and without the plugin before the same upstream pipeline,
optionally including native code generation and linking. It verifies output
behavior outside the timed region. It can reveal regressions as well as gains.

The RTX scripts intentionally separate validation runs from timing runs.
They require the requested device rather than silently switching to a CPU.

## Outstanding validation

- Physical AMD/Intel GPU execution and performance.
- Fresh LLVM 24 source-build validation of the optimization update.
- Native Windows LLVM plugin ABI/import-library support.
- Broad LLVM/client compiler regression suites and realistic application builds.
- LLVM clients beyond the demonstrated Clang hook and `opt` IR workflow.
- Realistic optimization-quality, generated-code-size and runtime comparisons.
- Calibrated automatic offload policy and sustained GPU memory behavior.
- Leak detection in an environment supporting LeakSanitizer.

GitHub workflow files are provided, but no remote GitHub Actions success is
claimed until a repository is published and those workflows actually run.
