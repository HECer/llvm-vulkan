# LLVM integration

This project supplies an experimental C++ bulk-compute library and real LLVM
optimization passes. LLVM retains ownership of the IR, pass manager, target
semantics, and machine-code generators. The plugin transfers compact arrays to
Vulkan and applies checked results through LLVM's normal mutation APIs.

It is **not a complete LLVM rewrite**, and loading it does not automatically
accelerate every LLVM-based language. The [coverage inventory](coverage.md)
distinguishes integrated transformations, standalone kernels, and unimplemented
compiler components. Compiler speedups remain a measurement question.

## Source and ABI requirements

`upstream-lock.json` records the exact upstream source commits, versions, archive
checksums, and retrieval date. The main source pin is LLVM 24.0.0git; the release
pin is LLVM 23.1.4. These are snapshots, not floating promises about future main.

The plugin must be built against the **same LLVM build** used by `opt`, `clang`,
or another host. LLVM's C++ ABI and plugin API are not cross-version contracts.
An LLVM 20 plugin must not be loaded into LLVM 24, nor should independently
configured builds of the same version be assumed compatible. Assertions, ABI
checks, standard library, architecture, runtime linkage, and build configuration
also matter.

The integration accepts LLVM 20 and newer at configuration time. A compile-time
feature probe selects the modern `OptionalPassInfoMixin` interface or the older
`PassInfoMixin`; header discovery similarly handles the relocation of
`PassPlugin.h`. This prevents a known source compatibility break in the pinned
LLVM 24 snapshot. Passing the version check is not a claim that every future
LLVM revision is tested. See the recorded validation results for exact builds
and tests that actually ran.

### Build the pinned source on Linux

Install a C++17 compiler, CMake, Ninja, Python 3.12 or a Python version with
`tarfile.data_filter`, Vulkan headers/loader, `glslangValidator`, and preferably
`spirv-val`. Use your distribution's documented packages or the Vulkan SDK.
Choose the build parallelism to fit your available RAM.

```sh
python3 scripts/fetch_llvm.py --channel main --destination third_party/llvm-project

cmake -S third_party/llvm-project/llvm -B build-llvm -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_ENABLE_PROJECTS=clang \
  -DLLVM_TARGETS_TO_BUILD=Native \
  -DLLVM_BUILD_LLVM_DYLIB=ON \
  -DLLVM_LINK_LLVM_DYLIB=ON \
  -DLLVM_ENABLE_ASSERTIONS=ON \
  -DLLVM_INCLUDE_TESTS=OFF

cmake --build build-llvm --target LLVM opt lli llc clang --parallel 4

cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_DIR="$PWD/build-llvm/lib/cmake/llvm"
cmake --build build --parallel 4
```

`fetch_llvm.py` rejects checksum mismatches and refuses to overwrite an existing
destination. To use the release snapshot, choose `--channel release` and a
separate source/build directory. You can also supply an existing compatible
LLVM installation with `-DLLVM_DIR=/absolute/path/to/lib/cmake/llvm`.

SPIR-V is compiled and embedded in the library during the normal build. Moving
the resulting plugin does not require a developer's absolute shader directory.
`LV_SHADER_DIR` is an explicit development override for external `.spv` files.

The CMake plugin target is `LLVMVulkan`. On Linux its file is normally
`build/llvm/LLVMVulkan.so`; multi-configuration generators can add a configuration
directory. Do not substitute a different LLVM shared library to make a load
error disappear.

## Running the passes

Loading the plugin only registers passes. It does not alter default compilation
unless `LV_AUTO_INSERT=1` is set.

```sh
LV_BACKEND=vulkan LV_DEVICE=gpu LV_VERIFY=1 LV_TRACE=1 \
  build-llvm/bin/opt \
  -load-pass-plugin="$PWD/build/llvm/LLVMVulkan.so" \
  -passes=lv-pipeline -verify-each input.ll -S -o optimized.ll
```

`LV_DEVICE=gpu` rejects software CPU devices. For lavapipe validation use
`LV_DEVICE=cpu` and select the lavapipe ICD as described in [testing](testing.md).

The plugin defaults to the scalar CPU reference. Select `LV_BACKEND=vulkan`
explicitly for experiments. A requested Vulkan operation never silently uses
the scalar algorithm. Initialization, capability, allocation, shader, and
dispatch failures cause a compiler diagnostic and a failing compilation.
Unsupported LLVM instructions remain in the IR for upstream LLVM to handle.
These are different behaviors: an unsupported instruction is not a failed GPU
operation.

### Implemented transformations

| Pass | Device work | Host work and boundaries |
|---|---|---|
| `lv-fold` | Batch evaluation of 23 integer arithmetic/bitwise/comparison operations; widths 1–32 and 33–64 use separate kernels. | Select only constant integer operands with no poison-generating flags/metadata. Apply valid results, then enqueue newly constant users. Preserve unsupported and unsafe requests. |
| `lv-cse` | Hash serialized instruction candidate records in a module-wide batch. | Prove equality with LLVM `isIdenticalTo`, require an earlier instruction in the same basic block, and update uses. Hashes alone never authorize replacement. |
| `lv-cfg` | Reachability closure over the module's disjoint control-flow graphs. | LLVM folds constant branch/switch terminators first, repairs PHIs, and deletes unreachable blocks. Address-taken blocks are additional roots. |
| `lv-dce` | Backward liveness closure from required instructions through SSA dependencies. | Conservatively retain memory operations, calls, EH, terminators, and all instructions outside the pure whitelist. Salvage debug information, drop dead cyclic references, and erase dead pure instructions. |
| `lv-pipeline` | The above kernels. | Run `lv-fold`, `lv-cse`, `lv-cfg`, then `lv-dce`. |

Every pass skips declarations and `optnone` functions. Mutating passes invalidate
their analyses through the new pass manager. The passes do not claim to be
drop-in replacements for InstCombine, GVN, ADCE, or SimplifyCFG: each has a much
narrower transformation scope.

**Integer semantics.** Folding includes modular add/subtract/multiply, bitwise
and/or/xor, three shifts, unsigned/signed division and remainder, and all ten
integer comparison predicates. Shifts by at least the operand width, division
by zero, and signed minimum divided/remaindered by minus one are rejected.
`nsw`, `nuw`, `exact`, disjoint/other poison-generating annotations are left to
upstream LLVM. The plugin preserves poison and undef inputs, vectors, floating
point, pointers, casts, and integers wider than 64 bits. The i33–i64 Vulkan path
requires the selected device's `shaderInt64` capability; an unsupported device
raises a clear error when a nonempty wide batch is requested. Empty wide batches
do not make an otherwise 32-bit workload require that capability.

**CSE correctness.** Candidate serialization deliberately does not encode every
LLVM subclass detail. Different GEP source types, comparison predicates,
aggregate indices, or shuffle masks can produce the same candidate record.
LLVM's exact equality check distinguishes them before mutation. Tests exercise
these deliberate collisions. PHIs, freeze, memory operations, poison-generating
flags, and metadata other than debug locations are excluded from CSE. Bounded
candidate buckets can miss opportunities but cannot make a hash collision into
a semantic equivalence.

**DCE correctness.** Liveness roots include every non-whitelisted instruction,
even unused plain loads and apparently pure calls. This conservative policy
retains `llvm.assume`, `llvm.sideeffect`, atomics, fences, and exception control
flow. Dead PHI/arithmetic cycles can disappear; a potentially nonterminating
control-flow loop is not removed merely because its arithmetic is unused.

**Standalone kernels.** Dense dominators, generic bitset liveness, exclusive
scan, and key sorting are implemented and tested in the compute library. They
do not yet replace LLVM's dominator-tree analysis, machine liveness, register
allocator, linker, or instruction scheduler. The coverage inventory records
these remaining integration gaps.

## Configuration

| Variable | Default | Meaning |
|---|---|---|
| `LV_BACKEND` | `cpu` | `cpu`, `vulkan`, or `auto`. |
| `LV_DEVICE` | `any` | Vulkan device class: `any`, `cpu`, or `gpu`. |
| `LV_DEVICE_NAME` | empty | Optional case-insensitive device-name substring. |
| `LV_VERIFY` | `0` | `1` compares every returned Vulkan batch with the scalar reference before consuming that batch; increases compilation time. |
| `LV_TRACE` | `0` | `1` emits requested backend, actual device, work/changed counts, scalar/Vulkan call counts, dispatches, transfer bytes, and complete pass wall time. |
| `LV_AUTO_INSERT` | `0` | `1` inserts `lv-pipeline` at the ordinary optimizer pipeline-start hook for hosts such as Clang. |
| `LV_AUTO_ENABLE` | `0` | Must be `1` before `LV_BACKEND=auto` selects Vulkan. |
| `LV_MIN_ITEMS` | `65536` | Uncalibrated auto crossover: fold candidates per width batch, hash words, or graph nodes plus edges. These are different units; calibrate with your workloads. |
| `LV_FOLD_MAX_ROUNDS` | `64` | Maximum constant-fold dependency frontiers. Remaining IR is preserved; `0` disables this transform. |
| `LV_CSE_BUCKET_LIMIT` | `64` | Bound on stored candidates per hash bucket in one block; `0` suppresses CSE replacements. |
| `LV_MEMORY_LIMIT_BYTES` | `536870912` | Runtime Vulkan buffer allocation budget passed by the plugin. Driver limits can be lower. |
| `LV_SHADER_DIR` | empty | Development override directory for compiled `.spv` files; normal builds use embedded shaders. |
| `LV_VALIDATION` | `0` | Runtime option: `1` requires `VK_LAYER_KHRONOS_validation`; selected Vulkan API validation errors fail execution. Use for correctness runs, not reported performance runs. |

The auto policy is deliberately opt-in. Its threshold is not trained, not an
RTX 5090 recommendation, and not a promise of a speedup. The library caches
devices and shader pipelines per compiler worker thread across passes/modules;
it does not provide a cross-process resident compilation service. Trace values
separate real Vulkan calls from scalar calls. `none-no-work` means a pass found
no eligible batch, not that an explicit Vulkan request was silently emulated.

## Language adoption

| Consumer | What works now | What still needs integration |
|---|---|---|
| Clang C/C++ | The matching Clang can load the plugin with `-fpass-plugin`; set `LV_AUTO_INSERT=1` to use the pipeline-start hook. | Benchmark the complete real build, including frontend, optimization, code generation, and linking. Loading this prepass may add overhead. |
| `opt` with LLVM IR/bitcode | Run explicit `lv-*` passes on IR produced by a compatible frontend. | Maintain matching IR/bitcode versions, target triple/data layout, and the frontend's required pipeline. |
| Flang | Its documented `-fpass-plugin` path can load matching LLVM plugins after lowering to LLVM IR. | This repository does not validate the current Flang driver/pipeline end to end. Use a matching build and test it independently. |
| Rust/rustc | Inspect or experiment with compatible `--emit=llvm-ir` output through `opt`. Nightly also exposes the unstable `-Z llvm-plugins` loading route. | Rust's documented unstable hook is an integration avenue, not a stable ABI or a tested configuration here. Build against the matching Rust LLVM revision/configuration and preserve codegen units, LTO, metadata and runtime/linker requirements. This project has not ported rustc or validated a rustc plugin build. |
| Julia, Swift, Zig, custom JITs and other LLVM users | The C++ compute API and pass registration are integration surfaces. | Each embedder must explicitly wire the matching LLVM pass pipeline and validate its semantics. There is no process-wide interception of every LLVM installation. |
| LTO / ThinLTO | Explicit `opt` experiments can process compatible modules. | The current auto insertion hook does not automatically register at the link-time backend extension points. LTO integration remains a separate task. |

Clang example:

```sh
LV_AUTO_INSERT=1 LV_BACKEND=vulkan LV_DEVICE=gpu LV_VERIFY=1 LV_TRACE=1 \
  build-llvm/bin/clang -O1 -g \
  -fpass-plugin="$PWD/build/llvm/LLVMVulkan.so" \
  examples/checksum.c -o checksum-vulkan
./checksum-vulkan
```

Compare it with the same compiler and options without the plugin. Keep the
plugin opt-in until your application regression tests and hardware measurements
justify a deployment decision.

Rust's nightly [LLVM plugin option](https://doc.rust-lang.org/nightly/unstable-book/compiler-flags/llvm-plugins.html)
is listed as an internal/unstable feature. It is a potentially useful direct
integration route, but this repository's LLVM 24 binary must not be loaded into
an unrelated rustc LLVM build. First inspect `rustc -vV`, then rebuild the plugin
against the exact host LLVM configuration and validate that its extension point
runs. No rustc plugin execution is included in the current validation claims.

## Primary references

- [LLVM: writing a new-pass-manager pass and registering plugins](https://llvm.org/docs/WritingAnLLVMNewPMPass.html)
- [LLVM: using the new pass manager](https://llvm.org/docs/NewPassManager.html)
- [LLVM language reference: integer semantics, poison, undef and freeze](https://llvm.org/docs/LangRef.html)
- [Flang driver: pass plugins](https://flang.llvm.org/docs/FlangDriver.html)
- [LLVM CMake build options](https://llvm.org/docs/CMake.html)
