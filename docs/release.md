# LLVM Vulkan 1.0

Version **1.0.0** is the stable source release of the implemented bulk-compute
API and scoped LLVM passes. Stability applies to this documented subset. It
does not mean a complete LLVM port, guaranteed compiler acceleration, or
certification of every Vulkan device.

## Supported contract

- C++17 static library, public `lv/compute.hpp` and generated `lv/version.hpp`.
- Integer folding through 64 bits, CSR reachability, dense dominator sets,
  gen/kill liveness, modular unsigned scan, unsigned key sort and FNV-1a record
  hashing, with the semantics documented in the public header.
- Vulkan 1.1 compute with embedded SPIR-V and capability-gated optional paths.
  Wide integer dispatch requires `shaderInt64`. Limited shared memory or absent
  subgroup arithmetic selects the general shader; explicit Vulkan never
  silently becomes scalar CPU execution.
  External SPIR-V overrides are a developer interface and must match the exact
  release; the embedded shader path is the distribution contract.
- Invalid shapes and resource limits fail explicitly. Invalid arithmetic
  requests return `valid=0` according to the public API. Device loss and driver
  failures are exceptions, not a promise of automatic device reconstruction.
- One `Compute` instance per worker or externally serialized use. Separate
  worker instances own separate device state and pools.
- LLVM `lv-fold`, `lv-cse`, `lv-cfg`, `lv-dce` and `lv-pipeline` preserve the
  conservative accepted subset in [integration.md](integration.md). CPU is the
  default; Vulkan and automatic offload require explicit selection.

Within 1.x, compatible source-level additions and correctness fixes are allowed.
Rebuild static-library consumers with their matching headers, C++ toolchain and
runtime. No cross-compiler C++ binary ABI is promised. LLVM plugins must always
match the exact host LLVM build; major/minor version equality alone is insufficient.

## Tested configurations

| Configuration | Release coverage |
| --- | --- |
| Windows x64, MSVC 19.44, RTX 5090 / driver 617.42 | Core CPU/Vulkan tests, forced fallback, allocation-failure recovery, large dispatches and relocated installed consumer |
| Linux x64 under WSL, GCC, Mesa lavapipe | Core optimized/fallback SPIR-V execution, recovery and LLVM 21.1.8 integration |
| GitHub Ubuntu 24.04, matching LLVM 20 and lavapipe | Automated core, package-consumer, LLVM fixture, oracle and semantic gates |

Physical AMD/Intel devices, native Windows LLVM plugin ABI and the current
release against a fresh LLVM 24 source build are outside the verified matrix.
Other devices can build/run through the same capability checks, but are not
claimed as validated platforms. Historical LLVM 24 results describe an earlier
snapshot and are retained as such.

## Build, test and install

Install CMake 3.20+, a C++17 compiler, Python 3.12+, Vulkan development files,
`glslangValidator` and `spirv-val`. Runtime Vulkan calls
need the Vulkan loader and a compatible driver, including in binary packages.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DLV_BUILD_LLVM_PLUGIN=OFF
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release --prefix install
```

For the LLVM plugin, use `-DLV_BUILD_LLVM_PLUGIN=ON` and the exact host's
`-DLLVM_DIR=/path/to/llvm/lib/cmake/llvm`. Matching `opt`/`lli` are required when
plugin tests are enabled. The plugin installs under `lib/llvm-vulkan` on Linux;
do not substitute an unrelated system LLVM library to load it.

SPIR-V validation is required by default. `-DLV_REQUIRE_SPIRV_VALIDATION=OFF`
is an explicit developer escape hatch; release builds and CI retain validation.

## Consume the installed library

```cmake
find_package(LLVMVulkan 1.0 CONFIG REQUIRED)
target_link_libraries(my_application PRIVATE LLVMVulkan::compute)
```

Set `CMAKE_PREFIX_PATH` to the installation prefix. Consumers need Vulkan
development files but do not need the shader compiler or Python just to link
the installed library. `tests/test_install.py` installs into a temporary prefix,
moves it, builds an unrelated consumer and runs CPU/Vulkan calls with embedded
shaders. Package discovery carries no build-tree include dependency.

## Distribution and release gates

Run `cpack -C Release --config build/CPackConfig.cmake` to produce a Windows ZIP
or Linux TGZ. The package contains the static library, benchmark executable,
headers, CMake config, licenses, offline presentation/graphs and evidence. Source
build commands in the documentation assume a source checkout. Windows binary
consumers also require the matching MSVC runtime. LLVM modules, when included,
remain bound to their exact LLVM installation.

Release checks include CPU/GPU differential suites, forced shaders, injected
post-submit allocation failure, `--version`, installed-package relocation and
the benchmark dashboard's 109 evidence-backed rows. The LLVM-enabled build adds
17-fixture verification, an 8,192-case arithmetic oracle and dynamic semantic
comparison. Khronos synchronization validation is enabled for correctness runs,
and disabled for reported timing. See [validation.md](validation.md) and
[release evidence](../evidence/release-1.0.0.json).

## Presentation, graphs and identity

Open [the project presentation](project-presentation.html) or the
[benchmark explorer](graphs/benchmarks.html) directly from disk. Both work
without a server, CDN, remote font or JavaScript dependency. The presentation
supports keyboard navigation and printing; the explorer includes all recorded
CPU losses, p95 values, source hashes and CSV export.

Editable [icon](assets/icon.svg), [dark wordmark](assets/logo-dark.svg) and
[light wordmark](assets/logo-light.svg) assets are generated by
`scripts/build_brand_assets.py`. They identify this project, not LLVM or Khronos
endorsement. Rebuild the documents with `scripts/build_benchmark_graphs.py` and
`scripts/build_presentation.py`. The design and presentation are AI-assisted;
absence of C2PA or a detected watermark does not establish human authorship.
The [transparent PNG](assets/icon.png) and [multi-size ICO](assets/favicon.ico)
are exported from the canonical SVG with the optional `scripts/export_icon.py`
helper (Playwright and Pillow; an existing browser, no automatic download).
The [read-only publication audit](../evidence/release-assets-provenance.json)
records each asset's scan coverage and unknowns. No marks were removed; a
conforming C2PA verifier/trust policy and proprietary watermark keys were not
provided, so cryptographic verification and signer trust remain unknown.

## Performance and future work

The [fresh benchmark](cpu-gpu-benchmark.md) compares a single-threaded project
reference with inclusive GPU API calls. It demonstrates large-batch wins and
substantial graph losses. It is not an upstream LLVM or parallel CPU benchmark.
Earlier timing artifacts keep their pre-release source/binary identities; they
are not relabeled as measurements of the 1.0 binaries.

Work-efficient graph frontiers, representative compiler build measurements,
calibrated offload and broader physical hardware coverage remain future work.
The [coverage ledger](coverage.md) continues to describe every unimplemented
compiler area. A stable supported subset can coexist with that open roadmap.
