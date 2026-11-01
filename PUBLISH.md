# Public experimental repository

Public repository: [HECer/llvm-vulkan](https://github.com/HECer/llvm-vulkan),
owned by Hermann Fischer / HECer.

Description:

> Experimental GPU-assisted LLVM passes via Vulkan. Work in progress; physical GPU testing pending. No speedup claim.

## Get the source

Clone the public repository:

```sh
git clone https://github.com/HECer/llvm-vulkan.git
cd llvm-vulkan
```

Follow the [README](README.md) for dependencies, the pinned LLVM source build,
and Vulkan CPU-mode tests. [testing.md](docs/testing.md) describes the test
workflows and physical GPU review procedure.

## Experimental status

This is a work-in-progress implementation of selected Vulkan-backed compiler
calculations and LLVM passes. A complete GPU port of LLVM remains unfinished;
the scope and remaining opportunities are documented in
[coverage.md](docs/coverage.md).

Local validation includes CPU references, software Vulkan execution through
Mesa lavapipe, and integration with the pinned LLVM source build. See
[validation.md](docs/validation.md) and the checked-in evidence for the exact
results and limitations. Physical GPU testing, including RTX 5090 validation,
is still outstanding. No hardware performance or compilation speedup is claimed.

GitHub workflow definitions are included, but successful remote CI execution
has not been verified. Check the repository's
[Actions page](https://github.com/HECer/llvm-vulkan/actions) for current runs;
[STATUS.json](STATUS.json) records the verified project status.
