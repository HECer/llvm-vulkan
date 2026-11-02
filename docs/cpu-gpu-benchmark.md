# Fresh CPU versus RTX 5090 benchmark

Measured on **2026-11-02** with the final second-iteration executable. This is
a comparison of this project's single-threaded C++ CPU reference and inclusive
Vulkan API calls. Sorting uses `std::sort`. It is not a comparison with upstream
LLVM, a tuned parallel/vectorized CPU library, or complete application builds.

The GPU wins larger folding, sorting and hashing batches. The CPU wins every
tested graph case, all 1,024-element workloads, and scan at all measured sizes.
Improving Vulkan over its earlier implementation does not imply it beats the CPU.

## Environment and method

- Windows 11 Pro, Ryzen 9 5950X (16 cores/32 threads), RTX 5090, driver 617.42.
- MSVC 19.44.35225 x64 Release; same executable, seed 5090 and inputs for both backends.
- PCIe 4.0 x8. Other desktop/GPU work remained active; clocks were not locked.
- Five alternating CPU/GPU process pairs per case, 11 warm calls per process:
  55 samples per backend/row, 90 processes and 4,070 accepted timed warm calls.
- All timed outputs are checked against the C++ reference outside the interval.
  Input generation and reference construction are also outside timing.
- Vulkan validation disabled for timing; optional device timestamps enabled.
  Upload, dispatch, convergence, host allocation and readback remain included.
- All GPU warm calls report zero new Vulkan memory allocations.
- No failed or retried measurement processes. Initialization/cold calls are
  retained separately in raw reports. Median GPU initialization was 173.575 ms
  (range 164.235–242.259 ms), excluded from the warm ratios below.

## Million-element arrays

Ratio means CPU median / GPU median: above one favors the GPU. Pair counts
show how many of the five paired process medians favored the GPU.

| Workload | CPU median (ms) | GPU median (ms) | CPU / GPU | GPU-faster pairs |
| --- | ---: | ---: | ---: | ---: |
| Integer fold, 32-bit | 12.6573 | 4.1664 | 3.04x | 5/5 |
| Integer fold, 64-bit | 16.1779 | 7.1027 | 2.28x | 5/5 |
| Exclusive scan | 1.2805 | 1.4689 | 0.87x | 1/5 |
| Sort keys | 55.9636 | 1.6659 | 33.59x | 5/5 |
| Hash records | 3.3305 | 2.6456 | 1.26x | 4/5 |

## All measured core sizes

Array size applies to fold, scan, sort and hash. Reachability, dominators and
liveness use **257 nodes**, regardless of array size; liveness has two words
per node. Thus the graph rows below are not million-node graph measurements.

| Array size | Workload | CPU median (ms) | GPU median (ms) | CPU / GPU | GPU-faster pairs |
| ---: | --- | ---: | ---: | ---: | ---: |
| 1024 | Integer fold, 32-bit | 0.0064 | 0.1055 | 0.061x | 0/5 |
| 1024 | Integer fold, 64-bit | 0.0078 | 0.1018 | 0.077x | 0/5 |
| 1024 | Reachability | 0.0020 | 0.1427 | 0.014x | 0/5 |
| 1024 | Dominators | 0.2396 | 0.4835 | 0.496x | 0/5 |
| 1024 | Liveness | 0.0234 | 0.1313 | 0.178x | 0/5 |
| 1024 | Exclusive scan | 0.0007 | 0.1138 | 0.006x | 0/5 |
| 1024 | Sort keys | 0.0088 | 0.1686 | 0.052x | 0/5 |
| 1024 | Hash records | 0.0027 | 0.1087 | 0.025x | 0/5 |
| 16384 | Integer fold, 32-bit | 0.1682 | 0.1590 | 1.058x | 4/5 |
| 16384 | Integer fold, 64-bit | 0.2002 | 0.1886 | 1.062x | 4/5 |
| 16384 | Reachability | 0.0021 | 0.1372 | 0.015x | 0/5 |
| 16384 | Dominators | 0.2411 | 0.4848 | 0.497x | 0/5 |
| 16384 | Liveness | 0.0287 | 0.1312 | 0.219x | 0/5 |
| 16384 | Exclusive scan | 0.0095 | 0.1333 | 0.071x | 0/5 |
| 16384 | Sort keys | 0.5760 | 0.2196 | 2.623x | 5/5 |
| 16384 | Hash records | 0.0388 | 0.1400 | 0.277x | 0/5 |
| 262144 | Integer fold, 32-bit | 3.1127 | 1.0448 | 2.979x | 5/5 |
| 262144 | Integer fold, 64-bit | 3.9873 | 1.7123 | 2.329x | 5/5 |
| 262144 | Reachability | 0.0022 | 0.1290 | 0.017x | 0/5 |
| 262144 | Dominators | 0.2385 | 0.4950 | 0.482x | 0/5 |
| 262144 | Liveness | 0.0282 | 0.1317 | 0.214x | 0/5 |
| 262144 | Exclusive scan | 0.3242 | 0.4409 | 0.735x | 0/5 |
| 262144 | Sort keys | 12.3779 | 0.6171 | 20.058x | 5/5 |
| 262144 | Hash records | 0.8448 | 0.6573 | 1.285x | 5/5 |
| 1048576 | Integer fold, 32-bit | 12.6573 | 4.1664 | 3.038x | 5/5 |
| 1048576 | Integer fold, 64-bit | 16.1779 | 7.1027 | 2.278x | 5/5 |
| 1048576 | Reachability | 0.0023 | 0.1478 | 0.016x | 0/5 |
| 1048576 | Dominators | 0.2453 | 0.4896 | 0.501x | 0/5 |
| 1048576 | Liveness | 0.0234 | 0.1317 | 0.178x | 0/5 |
| 1048576 | Exclusive scan | 1.2805 | 1.4689 | 0.872x | 1/5 |
| 1048576 | Sort keys | 55.9636 | 1.6659 | 33.594x | 5/5 |
| 1048576 | Hash records | 3.3305 | 2.6456 | 1.259x | 4/5 |

At 16,384 elements the fold medians are only about 1.06x in favor of the GPU
(4/5 faster pairs); this is a marginal crossover. Larger measured folds and
sorts improve in every paired run. Hashing at 1M improves in 4/5 pairs.
These observations do not calibrate a general offload threshold.

## Independent and mixed module graphs

Each small component is a chain; 42 of the 64 have a root. The mixed cases add
one rooted 2,048-node star or chain. The CPU reachability implementation uses
a work list; Vulkan uses local convergence and/or global Jacobi rounds.

| Graph | Nodes | CPU median (ms) | GPU median (ms) | CPU / GPU | GPU-faster pairs |
| --- | ---: | ---: | ---: | ---: | ---: |
| 64 chains of 128 | 8192 | 0.0469 | 0.2046 | 0.2292x | 0/5 |
| 64 chains of 32 + star | 4096 | 0.0200 | 0.2876 | 0.0695x | 0/5 |
| 64 chains of 128 + star | 10240 | 0.0713 | 0.3598 | 0.1982x | 0/5 |
| 64 chains of 512 + star | 34816 | 0.2058 | 0.9658 | 0.2131x | 0/5 |
| 64 chains of 128 + chain | 10240 | 0.0642 | 9.7100 | 0.0066x | 0/5 |

The long-chain mixed case remains about **151x slower on Vulkan than the CPU
reference**. Earlier batching improvements are real against the prior Vulkan
solver, but the CPU work-list algorithm remains much cheaper. Work-efficient
GPU frontiers and a measured offload policy are higher-priority follow-ups than
further fixed-round tuning for these inputs. The automatic offload policy is
still opt-in and uncalibrated; no new automatic selection rule is introduced.

## Reproduce and inspect

Build the Release executable as described in [integration.md](integration.md),
then run (use a new output directory):

```sh
python3 scripts/benchmark_cpu_gpu.py --binary build/lv-bench \
  --device-name 5090 --sizes 1024 16384 262144 1048576 \
  --rounds 5 --repeat 11 --include-graphs --output results/cpu-gpu-fresh
```

On a Visual Studio multi-config build, use `build/Release/lv-bench.exe`.
Select your actual GPU name for another device. The runner requires a physical
GPU, rejects mismatched devices/backends/inputs, keeps raw logs, and stops on
any correctness or process failure. It does not overwrite existing results or
silently retry failed pairs. Wide folding remains an explicit GPU capability skip.

[Machine-readable evidence](../evidence/cpu-vs-gpu-rtx5090.json) includes all
samples, paired-process ratios, p95 values, initialization/cold-call timings,
GPU counters, exact command lines and source/binary hashes. The measured binary
has the same SHA-256 as the final [second iteration](performance-iteration2.md).
Absolute latencies from the different sessions must not be combined into new
optimization ratios; desktop/GPU load differed.

Physical AMD/Intel GPUs, complete compiler speedup, and realistic build-time
crossovers remain unverified. CPU differential checks are regression evidence,
not an independent semantic proof of every C++ reference operation.
