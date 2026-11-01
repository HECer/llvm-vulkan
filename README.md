# LLVM Vulkan

**Experimental GPU-assisted LLVM compilation — work in progress, physical GPU testing pending.**

A C++ implementation of selected compiler calculations as Vulkan compute
shaders, connected to real LLVM optimization passes. Project owner: **Hermann
Fischer / HECer**. SPDX license: **Apache-2.0 WITH LLVM-exception**.

This is a functioning research implementation of scoped acceleration paths,
**not a complete GPU port of LLVM**. It does not yet establish a compilation
speedup. Software Vulkan correctness tests are distinct from RTX 5090 hardware
tests. The full source-wide gap inventory is in [coverage.md](docs/coverage.md).

## What actually executes on Vulkan

| LLVM integration | Device work | Host responsibility |
| --- | --- | --- |
| `lv-fold` | Batched scalar integer arithmetic and comparisons | LLVM candidate filtering, overflow/poison guards, dependency frontiers, replacement |
| `lv-cse` | Instruction record hashing | Exact LLVM equality, same-block dominance, collision-safe replacement |
| `lv-cfg` | Module-batched CFG reachability | Constant terminator folding, block-address roots, PHI repair and block deletion |
| `lv-dce` | Backward def-use reachability | Conservative side-effect roots, debug salvage, removal of dead pure SSA cycles |
| `lv-pipeline` | All four paths | Fold → CSE → CFG → DCE |

Additional **standalone**, tested compute primitives provide dominator bitsets,
gen/kill liveness, exclusive scans and key sorting. They are not connected to
LLVM's DominatorTree, register allocator, linker or other consumers yet.

The runtime includes device-local storage, staging upload/readback, explicit
barriers, cached pipelines, per-thread compiler contexts, iterative graph
buffers retained on the device, memory-budget checks, dispatch-grid tiling,
and embedded SPIR-V. No CUDA dependency is required. An explicitly requested
Vulkan backend never reports a scalar fallback as GPU execution.

Unsupported LLVM semantics stay in upstream LLVM: integers wider than 64 bits,
vectors, floating point, memory optimizations, most instruction combining,
loop transformations, instruction selection, register allocation and linking
are not ported. See [integration.md](docs/integration.md) for exact accepted
types, feature requirements and environment options.

## Upstream source target

The source pin observed on **2026-11-01** is LLVM main
[`b8ef6b0ed582fb5a235e7989685f5b188c620dc1`](https://github.com/llvm/llvm-project/commit/b8ef6b0ed582fb5a235e7989685f5b188c620dc1),
which reports **24.0.0git**. The latest stable alternative is **23.1.4**.

[upstream-lock.json](upstream-lock.json) records exact revisions, archive
checksums and provenance. The complete latest source was acquired and
inventoried; the repository stores our implementation and a reproducible source
fetcher instead of duplicating LLVM's entire monorepo. The plugin must be built
against the exact LLVM installation loading it; an LLVM 20 plugin is not an
LLVM 24 binary plugin.

## Quick start: Linux, build the pinned latest LLVM

On Ubuntu 24.04, install dependencies:

```sh
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build python3 \
  libvulkan-dev glslang-tools spirv-tools mesa-vulkan-drivers \
  vulkan-tools vulkan-validationlayers

bash scripts/build_latest.sh
```

The script downloads the pinned LLVM archive, verifies SHA256, extracts it
safely, builds `opt`, `lli`, `llc` and `llvm-config` with the X86 target and LLVM
assertions, then builds the plugin and test tools. It defaults to four compile
jobs and one LLVM link job. Set `LV_BUILD_JOBS=2` on memory-constrained systems.
This is a significant LLVM source build; allow substantial disk and time.

If you already have a compatible LLVM development installation:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_DIR=/path/to/llvm/lib/cmake/llvm
cmake --build build --parallel 4
```

Build only the Vulkan library, benchmarks and tests without LLVM:

```sh
cmake -S . -B build-core -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DLV_BUILD_LLVM_PLUGIN=OFF
cmake --build build-core --parallel 4
```

## Run the actual Vulkan path on a CPU

First locate the lavapipe ICD file with `ls /usr/share/vulkan/icd.d/lvp*.json`.
Its basename varies by Mesa package. Set the returned path explicitly:

```sh
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json
export LV_VALIDATION=1
export VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT

build/lv-bench --list-devices
build/lv-core-tests --backend vulkan --device cpu --cases 64 --seed 5090
ctest --test-dir build --output-on-failure
```

On systems using `lvp_icd.x86_64.json`, use that filename instead. A missing
validation layer is an error when `LV_VALIDATION=1`; install the layer instead
of silently treating an unvalidated run as validated.

An individual LLVM pass invocation:

```sh
LV_BACKEND=vulkan LV_DEVICE=cpu LV_VERIFY=1 LV_TRACE=1 \
  build-llvm/bin/opt -load-pass-plugin=build/llvm/LLVMVulkan.so \
  -passes=lv-pipeline -verify-each -S tests/llvm/fold.ll -o result.ll
```

`LV_VERIFY=1` compares returned device data to the independent
CPU reference before consuming it. The fixtures and semantic tests additionally
compare behavior with the original LLVM program and LLVM's own constant folder.

## Test on your RTX 5090

**Native Windows, core only:** open a Visual Studio developer PowerShell with
CMake, a C++17 compiler, Python and the Vulkan SDK available, then run:

```powershell
pwsh -File scripts/Test-Rtx5090.ps1
# Add -Large to exercise dispatch tiling above 8 million elements.
```

The script requires an NVIDIA device whose name contains `5090`, runs three
seeded correctness suites with validation, then collects CPU and GPU timings
at multiple batch sizes. It refuses to substitute lavapipe for the requested
GPU. The Windows runner is provided for your hardware tests; it was not run in
the Linux development environment.

**Native Linux, core and matching LLVM plugin:** after the build above, remove
any CPU-only ICD override and run:

```sh
unset VK_ICD_FILENAMES VK_DRIVER_FILES
bash scripts/test_rtx5090.sh
```

Results go to `results/rtx5090`. Where matching LLVM tools are available, this
also runs LLVM differential tests, an upstream constant-fold oracle, dynamic
semantic tests, and an `opt` + `llc` + native-link benchmark. WSL must be checked
with `lv-bench --list-devices`; working CUDA in WSL does not itself establish
that this Vulkan path can access the RTX 5090.

Native Windows LLVM plugin loading additionally requires a matching LLVM build
with appropriate exported symbols/import libraries. Building the standalone
Vulkan core on Windows does not establish Windows LLVM plugin compatibility.

## Measure compilation, not just kernel time

```sh
mkdir -p results
python3 examples/generate_ir.py --functions 128 --width 256 \
  --output results/workload.ll
python3 scripts/benchmark_pipeline.py \
  --opt build-llvm/bin/opt --lli build-llvm/bin/lli --llc build-llvm/bin/llc \
  --plugin build/llvm/LLVMVulkan.so --input results/workload.ll \
  --device gpu --device-name 5090 --output results/pipeline.json
```

This compares ordinary upstream O2 with the same O2 pipeline preceded by our
CPU or Vulkan passes. Each timed sample starts a fresh compiler process and
includes its initialization and all device transfer/orchestration work. Native
code generation and linking are timed when `--llc` is supplied; output behavior
is checked separately. Mode order rotates between repetitions. This measures
the current additive integration honestly, including any slowdown.

`lv-bench` reports cold calls and repeated warm calls, including transfers and
readback, for individual algorithms. Its CPU baseline is a scalar correctness
reference, **not LLVM's optimized native implementation**. Faster kernel time
does not establish a faster compiler. Lavapipe is a correctness environment,
not a prediction of RTX performance. See [research.md](docs/research.md) for
the cost model and profiling requirements.

## Validation and project status

[validation.md](docs/validation.md) and [evidence](evidence/) record the actual
local build/test results, versions, limitations and machine-readable reports.
The final seven-gate CMake suite passes against both LLVM 20.1.2 and the pinned
LLVM 24 source build. The LLVM 24 run also passes 120 upstream corpus fixtures
with zero skips; physical GPU validation remains outstanding.
The GitHub workflows provide software Vulkan regression checks and an explicit
manual source-build gate. Their presence does not mean that GitHub-hosted CI
or hardware tests have run.

CPU is the plugin's conservative default. Select `LV_BACKEND=vulkan` explicitly
for testing. Automatic offload requires an additional opt-in and an uncalibrated
size cutoff; do not treat the default cutoff as an RTX 5090 tuning result.

Installing this plugin does not transparently accelerate every LLVM language.
Clang, Flang, Rust, Swift, Julia and other clients have different pass hooks,
LLVM versions/forks and compiler ownership. Integration must be tested for
each client. This project does not replace `rustc` or rewrite LLVM in Rust.

## Repository map

- `src/`, `include/lv/`: Vulkan runtime, independent CPU references and benchmark CLI.
- `shaders/`: readable compute shaders compiled and embedded during the build.
- `llvm/`: real LLVM new-pass-manager integration.
- `tests/`: randomized core tests, LLVM fixtures and independent semantic oracles.
- `examples/`: executable input generator and C workload.
- `scripts/`: pinned source fetch, inventory, source build and hardware benchmark runners.
- `docs/coverage.md`: complete opportunity and omission ledger.
- `docs/integration.md`, `docs/testing.md`: integration and test instructions.
- `evidence/`: captured experimental validation records; no hardware speedup claim.

Contributions should include a defined semantic subset, differential tests,
actual end-to-end measurements and a clear statement of unimplemented cases.
Keep correctness and coverage claims narrower than the available evidence.
