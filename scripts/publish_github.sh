#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
# Explicit publication helper for the owner's authenticated machine.
set -euo pipefail
lv_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd -- "${lv_root}"
lv_account="$(gh api user --jq .login)"
if [[ "${lv_account}" != HECer ]]; then
  printf '%s\n' 'Refusing publication: gh must be authenticated as HECer.' >&2
  exit 1
fi
if [[ ! -d .git ]]; then
  printf '%s\n' 'Use the provided Git bundle to recover the reviewed commit first; see PUBLISH.md.' >&2
  exit 1
fi
if [[ -n "$(git status --porcelain)" ]]; then
  printf '%s\n' 'Commit or review local changes before publishing; working tree is not clean.' >&2
  exit 1
fi
if git remote get-url origin >/dev/null 2>&1; then
  printf '%s\n' 'An origin already exists. Inspect it and publish explicitly; this helper does not overwrite remotes.' >&2
  exit 1
fi
gh repo create HECer/llvm-vulkan --public --source . --remote origin --push \
  --description 'Experimental GPU-assisted LLVM passes via Vulkan. Work in progress; physical GPU testing pending. No speedup claim.'
