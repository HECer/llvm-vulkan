#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
set -euo pipefail
lv_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
lv_build="${LV_BUILD_DIR:-${lv_root}/build}"
lv_llvm="${LV_LLVM_BUILD:-${lv_root}/build-llvm}"
lv_results="${LV_RESULTS_DIR:-${lv_root}/results/rtx5090}"
lv_device_name="${LV_DEVICE_NAME:-5090}"
mkdir -p -- "${lv_results}"
"${lv_build}/lv-bench" --list-devices > "${lv_results}/devices.json"
python3 - "${lv_results}/devices.json" "${lv_device_name}" <<'PY'
import json, pathlib, sys
devices = json.loads(pathlib.Path(sys.argv[1]).read_text())
matching = [d for d in devices if not d['is_cpu'] and d['vendor_id'] == 0x10de
            and sys.argv[2].lower() in d['name'].lower()]
if not matching:
    raise SystemExit('No matching NVIDIA GPU; software Vulkan cannot stand in for an RTX 5090 test.')
if not all(d['supports_int64'] for d in matching):
    raise SystemExit('The complete RTX suite requires shaderInt64 on the selected device.')
PY
export LV_DEVICE_NAME="${lv_device_name}"
export LV_DEVICE=gpu
export LV_VALIDATION=1
export VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT
for lv_seed in 5090 1 12345; do
  "${lv_build}/lv-core-tests" --backend vulkan --device gpu \
    --device-name "${lv_device_name}" --cases 64 --seed "${lv_seed}" \
    --large "${LV_LARGE_TESTS:-0}" 2>&1 | tee "${lv_results}/correctness-${lv_seed}.log"
done
lv_plugin="${lv_build}/llvm/LLVMVulkan.so"
if [[ -f "${lv_plugin}" && -x "${lv_llvm}/bin/opt" && -x "${lv_llvm}/bin/lli" ]]; then
  python3 "${lv_root}/tests/run_llvm_tests.py" --opt "${lv_llvm}/bin/opt" \
    --lli "${lv_llvm}/bin/lli" --plugin "${lv_plugin}" --device gpu \
    --device-name "${lv_device_name}" --output "${lv_results}/llvm-fixtures.json"
  python3 "${lv_root}/tests/llvm/verify_constant_oracle.py" \
    --opt "${lv_llvm}/bin/opt" --plugin "${lv_plugin}" --device gpu --backend vulkan \
    --output "${lv_results}/constant-oracle.json"
  python3 "${lv_root}/tests/fuzz_llvm.py" --opt "${lv_llvm}/bin/opt" \
    --lli "${lv_llvm}/bin/lli" --plugin "${lv_plugin}" --device gpu \
    --output "${lv_results}/semantic-differential.json"
else
  printf '%s\n' 'Core tests completed; LLVM integration skipped because matching plugin/LLVM tools are absent.'
fi
export LV_VALIDATION=0
unset VK_LAYER_ENABLES VK_INSTANCE_LAYERS
for lv_size in 1024 16384 262144 1048576; do
  "${lv_build}/lv-bench" --backend cpu --size "${lv_size}" --repeat 7 \
    --json "${lv_results}/cpu-${lv_size}.json"
  "${lv_build}/lv-bench" --backend vulkan --device gpu --device-name "${lv_device_name}" \
    --size "${lv_size}" --repeat 7 --json "${lv_results}/vulkan-${lv_size}.json"
done
if [[ -f "${lv_plugin}" && -x "${lv_llvm}/bin/llc" && -x "${lv_llvm}/bin/lli" ]]; then
  python3 "${lv_root}/examples/generate_ir.py" --functions 128 --width 256 \
    --output "${lv_results}/workload.ll"
  python3 "${lv_root}/scripts/benchmark_pipeline.py" --opt "${lv_llvm}/bin/opt" \
    --lli "${lv_llvm}/bin/lli" --llc "${lv_llvm}/bin/llc" --plugin "${lv_plugin}" \
    --input "${lv_results}/workload.ll" --device gpu --device-name "${lv_device_name}" \
    --output "${lv_results}/pipeline.json"
fi
printf '%s\n' "Reports: ${lv_results}. Core timings alone do not establish whole-compiler speedups."
