# Portable Vulkan optimization

This page records the first optimization iteration. The
[second iteration](performance-iteration2.md) adds measured gains over this
already optimized baseline, with separate evidence and a reproducible source archive.
The subsequent [fresh CPU/GPU comparison](cpu-gpu-benchmark.md) measures the
final implementation against the C++ reference and includes GPU losses.

The optimization update keeps the Vulkan 1.1 baseline and the independent CPU
references. It adds no CUDA dependency and no vendor-ID-based shader decisions.
Explicit Vulkan requests still execute Vulkan or fail; automatic CPU offload
policy remains opt-in and uncalibrated.

## Runtime changes

- Device buffers and separate upload/readback buffers are pooled by role and
  capacity. Leases and recorded-use markers prevent recycling before completion.
  Idle entries can be evicted to satisfy the configured allocation budget.
- Host-visible staging allocations remain mapped. Upload selection prefers
  coherent memory; readback prefers cached memory. Non-coherent allocations
  retain flush/invalidate handling. Driver memory budgets are queried when the
  optional memory-budget extension is present.
- Upload, compute and readback share a command buffer and fence wait for simple
  operations. Dependencies remain explicit. Unsubmitted work from a rejected
  operation is discarded before subsequent calls can reuse its buffers.
- Optional timestamps measure the complete device submission, including copies;
  allocation, submission, reuse and fence-wait counters expose host overhead.
  Timestamps are off in the library/plugin by default and on in `lv-bench`.
- Pipelines are retained per context and use a Vulkan pipeline-cache object.
  Contexts remain per compiler worker; there is no cross-process GPU service or
  persistent on-disk pipeline cache.

## Shader changes and fallback selection

| Operation | Optimized path | Portable fallback/requirements |
| --- | --- | --- |
| Exclusive scan | Subgroup scans plus shared subgroup totals | Query compute-stage basic/arithmetic subgroup support. Dense logical indexing uses active lane counts, without assuming a 32-lane subgroup or a particular local-ID mapping. Existing shared-memory Blelloch scan remains available. |
| Unsigned key sort | Eight 4-bit stable radix passes, hierarchical histogram scan and bitmask-based local ranks | Uses Vulkan 1.1 integer/shared-memory operations, with no ballot extension. Tiny batches use a shared-memory bitonic network; the original global bitonic path remains available for tighter configured budgets or forced fallback. |
| Reachability | GPU-local convergence for up to 1,024 nodes per independent component; multiple components execute as separate workgroups | Requires 8,196 bytes of shared memory. Larger components retain the general Jacobi solver and convergence readback. |
| Dense dominators | Reachability and bitset initialization remain on device; small matrices converge locally | Local matrix limit: 4,096 words, requiring 32,772 bytes of shared memory. Lower shared-memory limits use the general solver. Dense matrix storage remains quadratic. |
| Gen/kill liveness | Small matrices converge locally | Same local matrix/shared-memory limits as dominators; general solver retained. |
| Folding and hashing | Pooled memory and combined submission | Integer semantics and FNV-1a output are unchanged. Wide folding still requires optional `shaderInt64`. |

All shaders remain embedded and validated at build time when `spirv-val` is
available. `Options::enable_fast_paths` and `LV_FAST_PATHS=0` allow baseline
shader testing independently of pooling/submission changes. Fixed 128-thread
workgroups are retained; no unmeasured device-specific workgroup tuning is
advertised.

## Validation

The current code is tested with Khronos validation and synchronization validation
on a native Windows RTX 5090 and Mesa lavapipe under Ubuntu in WSL. Lavapipe
reports 8-lane subgroups; the RTX reports 32-lane subgroups. Passing both helps
exercise portability, but does not replace physical AMD/Intel tests.

Regression cases cover changing pooled inputs, hierarchy and radix boundaries,
all-equal/descending keys, invalid component metadata, independent module-sized
graph batches, low allocation budgets, recovery after an abandoned recording,
optional timestamp disabling, forced baseline shaders, and large dispatch tiles.
The large suite also crosses 65,535 independent graph workgroups.

Matching LLVM 21.1.8 development libraries and tools are used for plugin
compilation, all 17 differential fixtures, the 8,192-case constant oracle and
the 64-function/512-call semantic differential. Previous LLVM 20/24 evidence
describes the earlier implementation; the optimization update has not yet
been rebuilt against the pinned LLVM 24 source.

## Iteration-one RTX 5090 results

The final alternating comparison improves every measured workload/size pair.
At the 1,048,576-element batch size, warm median API times are:

| Workload | Original Vulkan | Optimized Vulkan | Original / optimized |
| --- | ---: | ---: | ---: |
| Integer fold | 9.819 ms | 6.501 ms | 1.51x |
| Integer fold64 | 16.809 ms | 12.003 ms | 1.40x |
| Reachability, 257 nodes | 3.970 ms | 0.136 ms | 29.21x |
| Dominators, 257 nodes | 8.210 ms | 0.516 ms | 15.91x |
| Liveness, 257 nodes | 3.854 ms | 0.127 ms | 30.25x |
| Exclusive scan | 6.650 ms | 2.571 ms | 2.59x |
| Key sort | 7.500 ms | 2.785 ms | 2.69x |
| Record hash | 7.294 ms | 3.650 ms | 2.00x |

All eight workloads at all four measured sizes report zero new warm-call Vulkan
allocations and one submission per call. Sorting 1M keys records 56 dispatches,
compared with 210 in the original. Graph sizes remain fixed at 257 nodes in
this comparison; they do not grow with the array batch size.

These comparisons use the saved original executable under the same alternating
run protocol, rather than mixing results from the earlier, more heavily loaded
desktop session. The complete 27-sample distributions, p95 values, binary hashes,
source hashes and validation summary are in
[vulkan-optimization.json](../evidence/vulkan-optimization.json).

## Measurement boundaries

The core benchmark compares API calls including upload/readback and host
orchestration. Input generation and independent correctness checks are outside
the timed interval. Initialization and cold calls are separate. Warm calls reuse
allocations and pipelines, while still uploading current input and reading back
results. The CPU comparator is the scalar reference, not upstream LLVM.

`benchmark_compare.py` alternates saved baseline/current process order across
four sizes and three rounds, preserving nine warm samples per process. The
result is 27 samples per workload/size and includes median and p95. Timings are
collected with validation disabled. Other desktop/GPU workloads remain active;
ratios are observations on this machine, not cross-card speed guarantees.

The host reports a PCIe 4.0 x8 connection during this work. Host transfer costs
must be measured separately from device-local bandwidth. Whole-compiler GPU
timing still requires a compatible host LLVM/plugin setup; faster primitive
calls do not establish faster application builds.
