# Second Vulkan performance iteration

This iteration compares against the already optimized first Vulkan iteration,
not against the original unpooled implementation or upstream LLVM. The changes
remain Vulkan 1.1 compatible and use no vendor-ID-based selection.
The later [fresh CPU/GPU comparison](cpu-gpu-benchmark.md) uses this final
implementation and demonstrates where the CPU reference still wins.

## Retained changes

### Overlap host result allocation with GPU execution

For results of at least 64 KiB, the runtime submits the upload/compute/readback
commands before allocating and initializing the independent host result vector.
That CPU work can overlap the device submission. Small results remain allocated
before submission. The readback memcpy still happens after the fence completes.

If host preparation throws, the runtime waits for the submitted work before
rethrowing or recycling buffers. The isolated `readback-allocation-failure`
CTest injects the exception after submission and checks successful pooled reuse.
The synchronization model follows the
[Vulkan fence and memory-dependency rules](https://docs.vulkan.org/spec/latest/chapters/synchronization.html).

### Separate small functions from one oversized component

Previously, one component over 1,024 nodes disabled local convergence for the
entire module. The mixed solver now closes the small components independently
and preserves their final values in both device-state buffers. Global iterations
then touch only the oversized component.

The specialization requires one oversized component, at least half the total
nodes in small components, supported range-buffer sizes and enough shared memory.
Other module shapes keep the general solver. Tests cover the oversized component
at the start, middle and end, rooted/unrooted components, cyclic small functions,
and dispatch-grid tiling of the paired component-range representation.

### Stop at a stable final round and reduce host polling

Each recorded round now has its own convergence flag. Reading the final round's
flag recognizes a fixed point even if earlier rounds in the same batch changed
values. The previous accumulated flag required an additional all-stable batch.

For sparse reachability working sets of at most 65,536 active nodes and average
degree at most four, batch size grows from 8 to 16, 32 and 64 rounds while work
remains. Dense/larger reachability problems and generic dominator/liveness
solvers retain eight-round batches. The existing `n+16` iteration bound remains
in force; the last batch is shortened when necessary.

The adaptive-batching ablation reduced submissions from 257 to 36 on the
2,048-node long-component case, with median API time falling from 50.51 ms to
12.18 ms. This ablation's baseline already had final-round flags and mixed
component execution; it isolates batching rather than combining every gain.

### Switch shared banks for larger local dominator matrices

The 1,024–4,096-word local dominator solver switches source/destination banks
instead of copying every matrix cell after each round. All workgroup barriers
remain. Smaller matrices keep the previous kernel.

Seven paired process runs each improved at 257 and 352 nodes. Equivalent
reachability and liveness changes were removed: their timings were inconsistent,
despite some favorable pooled medians. Small-matrix dominator timing was also
inconsistent, so that size retains the original kernel.

## Evidence and interpretation

The RTX 5090 results below compare inclusive warm API time against iteration
one. Array rows contain 1,048,576 elements; graph rows name their actual shape.

| Workload | Iteration one median | Iteration two median | Speedup | Faster process pairs |
| --- | ---: | ---: | ---: | ---: |
| Integer fold, 32-bit | 9.332 ms | 6.707 ms | 1.39x | 4/6 |
| Integer fold, 64-bit | 15.785 ms | 10.951 ms | 1.44x | 5/6 |
| Exclusive scan | 3.227 ms | 2.382 ms | 1.35x | 6/6 |
| Sort keys | 3.333 ms | 2.578 ms | 1.29x | 6/6 |
| Hash records | 4.228 ms | 3.594 ms | 1.18x | 5/6 |
| 64 small chains of 32 + 2,048-node star | 0.924 ms | 0.350 ms | 2.64x | 5/5 |
| 64 small chains of 128 + 2,048-node star | 3.122 ms | 0.410 ms | 7.61x | 5/5 |
| 64 small chains of 512 + 2,048-node star | 10.917 ms | 1.040 ms | 10.50x | 5/5 |
| 64 small chains of 128 + 2,048-node chain | 45.211 ms | 12.626 ms | 3.58x | 5/5 |

These are ratios of pooled medians, not guaranteed speedups for arbitrary
programs. The evidence also preserves every measured size and the controls:
independent small components 1.09x (3/5 faster pairs, inconclusive), low local
coverage 1.55x, two oversized components 2.03x, and shallow global convergence
1.49x (each 5/5 faster pairs). The last three keep the general solver and benefit
from the convergence changes. At 1,024 elements, scan measured 0.157 to 0.161 ms and
unchanged local liveness measured 0.169 to 0.183 ms. Small-call timing does not
show a uniform gain, and unchanged-kernel fluctuations are not credited to a
new shader optimization.

The core comparison alternates process order across six rounds, with 15 warm
samples per process: 90 samples per workload/size. Module-graph comparisons use
five rounds and 11 samples, including independent-component, low-local-coverage,
multiple-large-component and long-chain controls. Initialization and cold calls
remain separate; all timed outputs are checked against the CPU reference.

Other desktop/GPU work remained active. A driver-initialization failure is not a
valid timing sample: incomplete experiments are preserved and restarted in full.
Only complete matched baseline/candidate runs enter the published comparisons.
No retry accepts a correctness failure or one successful half of a failed pair.
The first final graph experiment stopped on a baseline `vkCreateDevice` error
(`VK_ERROR_INITIALIZATION_FAILED`); the subsequent eight complete experiments
required no retries. The interrupted files remain in the local results directory.

[Machine-readable iteration-two evidence](../evidence/vulkan-iteration2.json)
contains complete accepted timing reports, individual samples, cold-call and
device timings, paired-process ratios, isolated experiments, source/binary
hashes and validation records. This is the canonical record for this iteration;
[iteration-one evidence](../evidence/vulkan-optimization.json) is historical.

The earlier baseline source is preserved byte-for-byte against the hashes in
the first iteration's evidence:
[source archive](../evidence/vulkan-iteration1-source.zip). The archive contains
`BASELINE.json` with file hashes. The matched benchmark builder was built and
tested against that archive, using the current workload generator with the old
runtime and old shaders.

Validation includes native RTX 5090 correctness with synchronization validation,
forced fallback paths, an injected allocation failure, lavapipe's 8-lane subgroup
path, and matching LLVM 21.1.8 fixtures/oracle/semantic checks. Physical AMD/Intel
tests, a fresh LLVM 24 rebuild and whole-compiler GPU speedup remain unverified.
All six native CTest gates pass. Three large seeded RTX runs each pass 299,119
assertions, including both component-range representations across dispatch
tiles. LLVM passes all 17 fixtures with fast paths enabled and disabled,
8,192 oracle cases, and the 64-function/512-call semantic differential.

## Reproduce a matched comparison

Extract the archive, then build the current benchmark driver against it:

```sh
python3 -m zipfile -e evidence/vulkan-iteration1-source.zip build-baseline-source
cmake -S scripts/benchmark_baseline -B build-baseline -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DLV_BASELINE_SOURCE=/absolute/path/to/build-baseline-source
cmake --build build-baseline --target lv-bench-matched --parallel 4
```

Example module-graph comparison, using the same input generator for both:

```sh
python3 scripts/benchmark_compare.py --baseline build-baseline/lv-bench-matched \
  --candidate build/lv-bench --device gpu --device-name YOUR_GPU \
  --sizes 1024 --rounds 5 --repeat 11 --output results/mixed-comparison \
  --benchmark-arg=--workload --benchmark-arg=reachability \
  --benchmark-arg=--graph-shape --benchmark-arg=mixed \
  --benchmark-arg=--graph-nodes --benchmark-arg=128 \
  --benchmark-arg=--components --benchmark-arg=64 \
  --benchmark-arg=--large-nodes --benchmark-arg=2048
```

Here `--size` controls the unused array-input batch; the graph report records
the actual graph size. `--graph-nodes` is the small-component length in mixed
mode. `--large-shape chain`, `--large-components`, and `--graph-shape independent`
provide the contrasting cases. These component modes require the reachability
workload so an accidental dense dominator reference cannot allocate a quadratic
matrix for a large module.

External SPIR-V overrides must be rebuilt with the current source. The renamed
`*_rounds` and `reachable_components` kernels intentionally fail to load from an
old shader directory rather than silently mixing convergence/range ABIs.

## Remaining unmeasured opportunities

Work-efficient frontiers for very large graphs, multiple oversized components,
temporary shader-name allocations in dispatch, joint narrow/wide batches, and
long-lived compiler workers still warrant profiling. They are not presented as
implemented gains. Faster core calls do not establish faster application builds.
