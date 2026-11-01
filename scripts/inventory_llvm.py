#!/usr/bin/env python3
"""Inventory an LLVM source tree without claiming that its passes are ported.

Records every top-level component, llvm/lib subdirectory, target backend, and
textual pass/analysis registration in the core new-pass-manager registry.
TableGen pass declaration files are listed separately; generated registrations,
legacy passes, target-specific pipelines and external projects are not equivalent
to entries in PassRegistry.def. This is an inventory, not a semantic audit.
"""

from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys


SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".inc", ".td", ".ll", ".mlir", ".s", ".S", ".py"}
IMPLEMENTATION_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".inc", ".td"}
REGISTRATION = re.compile(r'^([A-Z][A-Z0-9_]*(?:PASS|ANALYSIS)[A-Z0-9_]*)\s*\(\s*"([^"\n]+)"', re.MULTILINE)
TABLEGEN_PASS = re.compile(r"\bdef\s+([A-Za-z_][A-Za-z_0-9]*)\s*:\s*(?:Pass|InterfacePass|PassPipeline)\s*<")


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def brief(counter: Counter) -> dict:
    return dict(sorted(counter.items()))


def inventory(source: Path) -> dict:
    registry = source / "llvm/lib/Passes/PassRegistry.def"
    version = source / "cmake/Modules/LLVMVersion.cmake"
    if not registry.is_file() or not version.is_file():
        raise ValueError("--source must be a full LLVM monorepo root")
    provenance = source / ".llvm-vulkan-source.json"
    commit = None
    provenance_data = None
    if provenance.is_file():
        provenance_data = json.loads(provenance.read_text(encoding="utf-8"))
        commit = provenance_data.get("commit")
    elif (source / ".git").exists():
        command = subprocess.run(["git", "-C", str(source), "rev-parse", "HEAD"], capture_output=True, text=True, check=True)
        commit = command.stdout.strip()
    version_text = version.read_text(encoding="utf-8")
    parts = {key: re.search(r"set\(LLVM_VERSION_" + key + r"\s+([^\s)]+)", version_text) for key in ["MAJOR", "MINOR", "PATCH", "SUFFIX"]}
    version_string = ".".join(parts[key].group(1) if parts[key] else "?" for key in ["MAJOR", "MINOR", "PATCH"])
    if parts["SUFFIX"]:
        version_string += parts["SUFFIX"].group(1)
    components: dict[str, dict] = {}
    libraries: dict[str, Counter] = {}
    tablegen = []
    totals = Counter()
    # No symlink traversal: inventories files present in this checkout only.
    for path in sorted(source.rglob("*")):
        if not path.is_file() or path.is_symlink():
            continue
        relative = path.relative_to(source)
        if any(part.startswith(".") for part in relative.parts):
            continue
        name = relative.parts[0] if len(relative.parts) > 1 else "(root files)"
        current = components.setdefault(name, {"files": 0, "bytes": 0, "extensions": Counter(), "implementation_files": 0, "test_files": 0})
        current["files"] += 1
        current["bytes"] += path.stat().st_size
        current["extensions"][path.suffix or "(none)"] += 1
        is_test = any(p in {"test", "tests", "unittests"} for p in relative.parts)
        current["test_files"] += int(is_test)
        current["implementation_files"] += int(path.suffix in IMPLEMENTATION_SUFFIXES and not is_test)
        totals["files"] += 1
        totals["bytes"] += path.stat().st_size
        if len(relative.parts) >= 4 and relative.parts[:2] == ("llvm", "lib"):
            lib = libraries.setdefault(relative.parts[2], Counter())
            lib["files"] += 1
            lib["implementation_files"] += int(path.suffix in IMPLEMENTATION_SUFFIXES)
            lib["bytes"] += path.stat().st_size
        if path.suffix == ".td" and not is_test:
            text = path.read_text(encoding="utf-8", errors="replace")
            declarations = TABLEGEN_PASS.findall(text)
            # Only Pass/InterfacePass matches matter; generated templates and
            # aliases can evade this syntactic inventory and are documented.
            if declarations:
                tablegen.append({"path": relative.as_posix(), "declarations": declarations})
    for current in components.values():
        current["extensions"] = brief(current["extensions"])
    registrations = []
    text = registry.read_text(encoding="utf-8")
    for match in REGISTRATION.finditer(text):
        macro, name = match.groups()
        registrations.append({
            "macro": macro,
            "name": name,
            "source_line": text.count("\n", 0, match.start()) + 1,
            "upstream_pass_ported": False,
        })
    targets = sorted(p.name for p in (source / "llvm/lib/Target").iterdir() if p.is_dir())
    return {
        "schema_version": 1,
        "upstream_commit": commit,
        "version": version_string,
        "source_provenance": {k: v for k, v in (provenance_data or {}).items() if k not in {"source", "archive"}},
        "version_file_sha256": sha256(version),
        "scope": "Complete non-hidden regular-file inventory; core textual pass registrations; syntactic TableGen pass declaration inventory.",
        "limitations": [
            "Directory/file enumeration is not a line-by-line semantic code review.",
            "PassRegistry.def is not every legacy, generated, target-specific, MLIR or downstream pass.",
            "The Vulkan plugin implements scoped alternative transformations, not copies of the full upstream passes.",
            "No entry is marked ported merely because a similarly named GPU primitive exists.",
        ],
        "totals": brief(totals),
        "components": dict(sorted(components.items())),
        "llvm_libraries": {k: brief(v) for k, v in sorted(libraries.items())},
        "llvm_targets": targets,
        "core_registry": {
            "path": "llvm/lib/Passes/PassRegistry.def",
            "sha256": sha256(registry),
            "registrations": len(registrations),
            "macro_counts": brief(Counter(p["macro"] for p in registrations)),
            "entries": registrations,
        },
        "tablegen_declaration_files": tablegen,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    result = inventory(args.source.resolve())
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"output": str(args.output), "commit": result["upstream_commit"], "version": result["version"], "files": result["totals"]["files"], "components": len(result["components"]), "core_registrations": result["core_registry"]["registrations"], "targets": len(result["llvm_targets"]), "tablegen_declaration_files": len(result["tablegen_declaration_files"])}))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"inventory_llvm: {error}", file=sys.stderr)
        raise SystemExit(1)
