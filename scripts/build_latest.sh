#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
set -euo pipefail
# Run from any directory. Dependencies are explicit in README.md; this script
# never installs packages or modifies the system toolchain.
lv_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
lv_jobs="${LV_BUILD_JOBS:-4}"
lv_source="${LV_LLVM_SOURCE:-${lv_root}/vendor/llvm-project}"
lv_llvm_build="${LV_LLVM_BUILD:-${lv_root}/build-llvm}"
lv_build="${LV_BUILD_DIR:-${lv_root}/build}"
if [[ -d "${lv_source}" ]]; then
  python3 - "${lv_root}/upstream-lock.json" "${lv_source}" <<'PY'
import hashlib, json, pathlib, sys
pin = json.loads(pathlib.Path(sys.argv[1]).read_text())["main"]
source = pathlib.Path(sys.argv[2])
stamp = json.loads((source / ".llvm-vulkan-source.json").read_text())
version = source / "cmake/Modules/LLVMVersion.cmake"
if stamp["commit"] != pin["commit"] or hashlib.sha256(version.read_bytes()).hexdigest() != pin["version_file_sha256"]:
    raise SystemExit("Existing source does not match upstream-lock.json; choose a different LV_LLVM_SOURCE")
print("Reusing pinned source; local edits outside its version file are preserved.")
PY
else
  python3 "${lv_root}/scripts/fetch_llvm.py" --destination "${lv_source}"
fi
cmake -S "${lv_source}/llvm" -B "${lv_llvm_build}" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DLLVM_TARGETS_TO_BUILD=X86 \
  -DLLVM_ENABLE_ASSERTIONS=ON -DLLVM_BUILD_LLVM_DYLIB=ON -DLLVM_LINK_LLVM_DYLIB=ON \
  -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF \
  -DLLVM_PARALLEL_LINK_JOBS=1
cmake --build "${lv_llvm_build}" --parallel "${lv_jobs}" --target opt lli llc llvm-config
cmake -S "${lv_root}" -B "${lv_build}" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DLLVM_DIR="${lv_llvm_build}/lib/cmake/llvm"
cmake --build "${lv_build}" --parallel "${lv_jobs}"
printf '%s\n' "Build complete. Use the opt/lli/llc from ${lv_llvm_build}/bin with this plugin."
