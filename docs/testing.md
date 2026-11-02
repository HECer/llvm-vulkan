# Validation and RTX 5090 testing

The repository is experimental. Software Vulkan correctness checks are useful
evidence for dispatch, synchronization, serialization, and transformations;
they do not establish real-GPU correctness or a hardware performance benefit.
The hardware validation plan is to repeat these tests on the selected NVIDIA
device, then measure complete compilation pipelines against unmodified LLVM.

Use **native Linux** as the primary LLVM-plugin test path. Native Windows can
build the Vulkan compute library and benchmark; a native Windows plugin also
needs a compatible LLVM shared/import library and toolchain. WSL availability
of CUDA does not establish that Vulkan exposes an NVIDIA device. In any
environment, trust the device enumeration and strict GPU selection below.

## What is tested

| Layer | Test | Purpose |
|---|---|---|
| Vulkan/core arithmetic | `lv-core-tests` | Exhaustive small-width and edge/random integer folding, wide integer folding, graph closure/dataflow, primitives, resource validation, and backend provenance. |
| LLVM transformations | `tests/run_llvm_tests.py` | Parse/verify each fixture, apply CPU and required Vulkan backends, check transformed IR, compare outputs, and execute defined fixtures with `lli`. |
| Independent integer oracle | `tests/llvm/verify_constant_oracle.py` | Compare all 23 folding operation kinds at widths through 64 with upstream LLVM InstCombine, without using the project's scalar implementation as the oracle. |
| Dynamic semantic differential | `tests/fuzz_llvm.py` | Execute original and transformed programs with runtime arguments, conditional poison, freeze, no-wrap annotations, observable stores, assumes, branches and PHIs; compare full uint32 output streams. |
| Bounded upstream corpus | `tests/upstream_corpus.py` | Verify up to 120 pinned upstream IR fixtures before/after the plugin and compare CPU/Vulkan IR. Never execute these possibly undefined programs; this is structural validation, not a semantic proof or the whole LLVM suite. |
| Frontend adoption | Clang `-fpass-plugin` checksum example | Check the automatic pipeline hook, C frontend/code generation, debug build, and native executable behavior. |
| Performance | `lv-bench` and `scripts/benchmark_pipeline.py` | Separate compute API timing from complete `opt`-process and optional codegen/link timing. |

Targeted IR fixtures cover same-hash CSE candidates with different hidden
semantic state, noundef/assume, poison and undef, exact/no-wrap/disjoint flags,
integer overflow, PHI cycles, optnone, atomics/volatile effects, EH, debug-value
salvage, indirect branches, global blockaddress initializers, and PHI repair.
These are regression checks, not a proof over all possible LLVM programs.

## 1. Build and identify the device

Follow [integration.md](integration.md) to build the matching LLVM and plugin.
The examples below assume the compiler tools are in `build-llvm/bin` and this
project's binaries are in `build`. Adjust paths to your actual build.

```sh
mkdir -p reports
build/lv-bench --list-devices > reports/devices.json
build-llvm/bin/opt --version > reports/llvm-version.txt
vulkaninfo --summary > reports/vulkan-info.txt
nvidia-smi > reports/nvidia-driver.txt
```

The device report must list the intended NVIDIA device with `is_cpu: false`.
Wide integer tests also require `supports_int64: true`. Keep these reports with
the test/benchmark JSON. If your machine has several GPUs, constrain benchmark
and plugin selection with `--device-name 5090` or `LV_DEVICE_NAME=5090` as
appropriate. The core-test executable also accepts `--device-name 5090`; the
Vulkan loader's driver selection remains useful when isolating an ICD.

## 2. Correctness on the actual GPU

Install the Khronos validation layer and perform an initial validation run:

```sh
LV_VALIDATION=1 build/lv-core-tests --backend vulkan --device gpu --device-name 5090 --seed 5090

LV_VALIDATION=1 python3 tests/run_llvm_tests.py \
  --opt build-llvm/bin/opt --lli build-llvm/bin/lli \
  --plugin build/llvm/LLVMVulkan.so --device gpu --device-name 5090 \
  --output reports/llvm-rtx5090-validation.json

LV_DEVICE_NAME=5090 LV_VALIDATION=1 \
python3 tests/llvm/verify_constant_oracle.py \
  --opt build-llvm/bin/opt --plugin build/llvm/LLVMVulkan.so \
  --backend vulkan --device gpu --cases 8192 --seed 5090 \
  --output reports/constant-oracle-rtx5090.json

LV_DEVICE_NAME=5090 LV_VALIDATION=1 \
python3 tests/fuzz_llvm.py \
  --opt build-llvm/bin/opt --lli build-llvm/bin/lli \
  --plugin build/llvm/LLVMVulkan.so --device gpu \
  --functions 64 --calls 512 --seed 5090 \
  --output reports/semantic-differential-rtx5090.json
```

`LV_VALIDATION=1` fails if the validation layer is unavailable; install it or
explicitly record a correctness run without API validation. It must not be
silently described as a validation-layer run. The LLVM integration runner also
compares device results to the scalar reference (`LV_VERIFY=1`). The independent
oracle and semantic differential scripts deliberately use `LV_VERIFY=0`, so
their independent comparison remains the correctness check.

After the ordinary tests pass, the optional tiled-dispatch case crosses the
single-dispatch workgroup-count boundary. It is a larger resource stress test:

```sh
LV_VALIDATION=1 build/lv-core-tests \
  --backend vulkan --device gpu --large 1 --seed 5090
```

Stop and retain the seed, command, report, driver/device information, and failing
IR if a comparison fails. For semantic reproduction, `tests/fuzz_llvm.py`
accepts `--keep-dir reports/fuzz-repro` to retain original and transformed IR.
Do not derive performance claims from a run with a correctness mismatch.

### Optional pinned upstream corpus gate

With the pinned upstream source available, run this additional bounded gate:

```sh
LV_VALIDATION=1 python3 tests/upstream_corpus.py \
  --source third_party/llvm-project \
  --opt build-llvm/bin/opt --plugin build/llvm/LLVMVulkan.so \
  --device gpu --device-name 5090 --limit 120 --seed 5090 \
  --output reports/upstream-corpus-rtx5090.json \
  --keep-failure-dir reports/upstream-failure
```

The runner balances selection across upstream `DCE`, `EarlyCSE`, `SimplifyCFG`,
and `InstCombine`. Source-relative filenames are ordered by a deterministic
seeded hash. It records the pinned source identity, every sampled file's SHA256,
the plugin SHA256, LLVM version, actual dispatch counts, and skip diagnostics.
The default maximum is 240 candidate files to obtain up to 120 verifier-valid
fixtures; small suites contribute all available valid files and the remaining
suites share the balance. Split-file fixtures, verifier-incompatible IR, target
limitations, and resource exclusions remain separate from passing cases.

A transformation error, verifier failure after transformation, or CPU/Vulkan
output mismatch fails the run immediately and is never converted into a skip.
These upstream inputs are **never executed**: many intentionally exercise poison,
undefined behavior, and target-specific edge cases. This gate establishes
structural preservation and backend parity for the sampled inputs. It does not
establish semantic correctness, run lit directives, check every upstream
expectation, or run the complete LLVM regression suite.

## 3. Compute API performance

Disable validation for timing and measure cold and warm calls separately:

```sh
LV_VALIDATION=0 build/lv-bench \
  --backend vulkan --device gpu --device-name 5090 \
  --size 1048576 --graph-nodes 1024 --repeat 10 \
  --json reports/primitives-rtx5090.json

LV_VALIDATION=0 build/lv-bench \
  --backend cpu --size 1048576 --graph-nodes 1024 --repeat 10 \
  --json reports/primitives-scalar.json
```

The benchmark includes input transfer, device dispatch, convergence, readback,
and host orchestration in each call. Device initialization is reported
separately; the cold call includes lazy pipeline setup. Independent reference
generation/checking is outside warm timed regions. Graph, fold, hashing, sorting,
and scan workloads have different scaling behavior: `--size` does not make the
graph as large as the array. Increase sizes only while driver and runtime memory
limits remain satisfied.

The CPU comparison is this project's scalar reference, **not optimized upstream
LLVM**. A fast kernel relative to that reference does not prove that a compiler
build becomes faster. Dense dominators retain quadratic storage; the baseline
sorting fallback is bitonic. Read the coverage and research documents before
extrapolating timings.

`lv-bench` enables optional device timestamps by default; pass `--timestamps 0`
to remove query overhead. `device_ms_last_call` measures device-side submission
execution, including transfers, and is not the median host API time. Reports
also contain submissions, new Vulkan allocations, buffer reuse and retained
allocation bytes for the last call. `iterations_per_call` counts host-recorded
rounds; local solvers perform their convergence loop inside the shader.

Both `lv-core-tests` and `lv-bench` accept `--fast-paths 0` to force baseline
shaders. CTest includes `core-vulkan-fallback` with optional shaders and device
timestamps disabled. Keep Khronos synchronization validation enabled for both
correctness paths. Shader compilation also runs `spirv-val` when available.

The core-only build has six CTest gates. `readback-allocation-failure` injects
an allocation failure after queue submission, then checks completed work and
correct pooled-buffer reuse. Graph checks cover mixed component positions,
unrooted/cyclic components, per-round flags and adaptive batching. `--large 1`
also crosses the dispatch limit for both contiguous and paired component ranges.

To isolate a benchmark, use `--workload reachability` (or one of the workload
names in the JSON). `--graph-shape independent --components 64 --graph-nodes 128`
builds 64 independent chains. `--graph-shape mixed` adds one large component;
`--large-nodes 2048 --large-shape star --large-components 1` selects its shape.
Use `chain` for slow propagation. These component modes require the reachability
workload. The report records the actual graph size separately from array size.

To compare saved pre-change and current executables on any selected GPU:

```sh
python3 scripts/benchmark_compare.py --baseline /path/to/old/lv-bench \
  --candidate build/lv-bench --device gpu --device-name YOUR_GPU \
  --rounds 3 --repeat 9 --output results/vulkan-comparison
```

This rotates process order, checks outputs and device identity, preserves raw
reports, and summarizes warm median/p95 times. Unsupported wide folding remains
an explicit capability skip. It does not measure whole-compiler acceleration.
Repeat `--benchmark-arg=VALUE` to pass identical workload options to both drivers.
The [second-iteration record](performance-iteration2.md) includes an archived
runtime and a builder that links the current input generator against that old
runtime, so new graph shapes can be compared with matching inputs.

For a fresh CPU-reference versus physical-GPU comparison of one executable:

```sh
python3 scripts/benchmark_cpu_gpu.py --binary build/lv-bench --device-name YOUR_GPU \
  --rounds 5 --repeat 11 --include-graphs --output results/cpu-gpu-fresh
```

The output directory must be new. Both backends receive matching seeds and
inputs; process order alternates. Initialization and cold calls are separate,
and every timed output is checked outside the timing interval. The runner
requires the GPU, stops on failure, and includes slower GPU results. Read the
[published CPU/GPU report](cpu-gpu-benchmark.md) for interpretation limits.

## 4. Measure complete compiler processes

Generate a reproducible, executable synthetic workload:

```sh
python3 examples/generate_ir.py \
  --functions 128 --width 256 --dead 16 --output reports/synthetic.ll

python3 scripts/benchmark_pipeline.py \
  --opt build-llvm/bin/opt --lli build-llvm/bin/lli \
  --plugin build/llvm/LLVMVulkan.so --input reports/synthetic.ll \
  --device gpu --device-name 5090 --repeat 7 --warmup 1 \
  --output reports/pipeline-rtx5090.json
```

This compares three modes: upstream `default<O2>`, the scalar plugin followed
by the same upstream pipeline, and the Vulkan plugin followed by that pipeline.
Every sample uses a new `opt` process. The measured interval includes parsing,
IR packing, initialization, transfer, mutation, upstream optimization, and output
serialization. Execution comparisons are outside the measured interval. Mode
ordering rotates to reduce systematic order bias.

To include native code generation and linking, add the matching `--llc` and
host compiler/link driver:

```sh
python3 scripts/benchmark_pipeline.py \
  --opt build-llvm/bin/opt --lli build-llvm/bin/lli \
  --llc build-llvm/bin/llc --cc cc \
  --plugin build/llvm/LLVMVulkan.so --input reports/synthetic.ll \
  --device gpu --device-name 5090 --repeat 7 --warmup 1 \
  --output reports/pipeline-native-rtx5090.json
```

The example deliberately contains redundant arithmetic; label it synthetic.
Then repeat with representative real LLVM IR with a defined executable `main`
and the same target/runtime setup. For an actual C++ project, measure the
unchanged build and the Clang plugin build using the same optimization flags,
job count, source revision, clean/incremental condition, and compilation cache
policy. Include frontend work in that real-build measurement. Report regressions
as well as improvements, output-code size, output quality tests, and the precise
GPU/driver/LLVM versions. Do not combine only favorable individual kernel ratios
into an invented whole-compiler speedup.

## CPU Vulkan reproduction

On Linux, select your installed lavapipe ICD. The path below is an example;
distributions can use names such as `lvp_icd.x86_64.json` instead.

```sh
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json
export MESA_SHADER_CACHE_DIR="$PWD/build/mesa-cache"

build/lv-core-tests --backend vulkan --device cpu
python3 tests/run_llvm_tests.py \
  --opt build-llvm/bin/opt --lli build-llvm/bin/lli \
  --plugin build/llvm/LLVMVulkan.so --device cpu \
  --output reports/llvm-lavapipe.json
ctest --test-dir build --output-on-failure
```

This runs real SPIR-V through Vulkan on a CPU device. `LV_BACKEND=cpu` instead
selects the independent scalar C++ implementation; the two modes are not the
same. Clear a forced lavapipe ICD before attempting NVIDIA tests, or strict GPU
selection will correctly fail.

## Native Windows core tests

Use a developer shell with a C++17 compiler, CMake, Ninja, Python, and the Vulkan
SDK. The core path does not require an LLVM installation:

```powershell
cmake -S . -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release -DLV_BUILD_LLVM_PLUGIN=OFF
cmake --build build-win --parallel 4
.\build-win\lv-bench.exe --list-devices
.\build-win\lv-core-tests.exe --backend vulkan --device gpu
.\build-win\lv-bench.exe --backend vulkan --device gpu --device-name 5090 --size 1048576 --repeat 10 --json rtx5090-core.json
```

Native Windows LLVM plugin compilation is conditional on an appropriate LLVM
shared/import target and matching host tools. The plugin exports its entry point
for Windows builds, but Windows integration has separate validation requirements.
If the LLVM package cannot satisfy those linkage requirements, use the core-only
Windows tests and a native Linux LLVM integration environment; do not relabel a
core-only result as a complete compiler integration test.

## Primary references

- [Khronos Vulkan Guide: validation layers](https://docs.vulkan.org/guide/latest/validation_overview.html)
- [Khronos Vulkan loader: driver discovery and selection](https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderDriverInterface.md)
- [Mesa source tree: lavapipe software Vulkan frontend](https://docs.mesa3d.org/sourcetree.html)
- [LLVM testing infrastructure](https://llvm.org/docs/TestingGuide.html)
