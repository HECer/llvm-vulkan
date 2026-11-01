#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
"""Verify IR and executable equivalence of CPU/Vulkan integrated passes."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import time


def run(command, env, timeout=120):
    start = time.perf_counter()
    result = subprocess.run([str(c) for c in command], env=env, capture_output=True,
                            text=True, timeout=timeout)
    return result, time.perf_counter() - start


def normalize(ir):
    return "\n".join(line for line in ir.splitlines()
                     if not line.startswith("; ModuleID =") and not line.startswith("source_filename ="))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--opt", type=Path, required=True)
    parser.add_argument("--lli", type=Path, required=True)
    parser.add_argument("--plugin", type=Path, required=True)
    parser.add_argument("--fixtures", type=Path, default=Path(__file__).parent / "llvm")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--device", choices=("any", "cpu", "gpu"), default="any")
    parser.add_argument("--device-name", default="")
    parser.add_argument("--shader-dir", type=Path)
    args = parser.parse_args()
    fixtures = sorted(args.fixtures.glob("*.ll"))
    if not fixtures:
        parser.error("No LLVM fixtures found")
    for tool in (args.opt, args.lli, args.plugin):
        if not tool.is_file():
            parser.error(f"Required file missing: {tool}")
    env = os.environ.copy()
    env.update(LV_DEVICE=args.device, LV_DEVICE_NAME=args.device_name,
               LV_TRACE="1", LV_VERIFY="1", LV_AUTO_INSERT="0")
    if args.shader_dir:
        env["LV_SHADER_DIR"] = str(args.shader_dir.resolve())
    version, _ = run([args.opt, "--version"], env)
    report = {"schema_version": 1, "llvm_version": version.stdout.strip(),
              "device_requested": args.device, "device_name_requested": args.device_name,
              "vulkan_is_required": True, "fixtures": []}
    failures = []
    with tempfile.TemporaryDirectory(prefix="llvm-vulkan-tests-") as tmp:
        tmp = Path(tmp)
        env.setdefault("MESA_SHADER_CACHE_DIR", str(tmp / "mesa-cache"))
        for fixture in fixtures:
            row = {"name": fixture.name, "backends": {}}
            try:
                source = fixture.read_text()
                values = lambda key: re.findall(r"^;\s*LV-" + key + r":\s*(.+)$", source, re.MULTILINE)
                passes = (values("PASSES") or ["lv-pipeline"])[0]
                expects, rejects, exits = values("EXPECT"), values("REJECT"), values("EXIT")
                baseline, _ = run([args.opt, "-passes=verify", "-disable-output", fixture], env)
                if baseline.returncode:
                    raise RuntimeError("Invalid fixture: " + baseline.stderr)
                if exits:
                    result, _ = run([args.lli, fixture], env)
                    if result.returncode != int(exits[0]):
                        raise RuntimeError(f"Baseline exit {result.returncode}, expected {exits[0]}: {result.stderr}")
                    row["baseline_exit"] = result.returncode
                outputs = {}
                for backend in ("cpu", "vulkan"):
                    local_env = dict(env, LV_BACKEND=backend)
                    destination = tmp / f"{fixture.stem}-{backend}.ll"
                    command = [args.opt, f"-load-pass-plugin={args.plugin.resolve()}",
                               f"-passes={passes}", "-verify-each", "-S", fixture, "-o", destination]
                    result, seconds = run(command, local_env)
                    detail = {"success": result.returncode == 0, "seconds": seconds,
                              "trace": result.stderr}
                    row["backends"][backend] = detail
                    if result.returncode:
                        raise RuntimeError(f"{backend} opt failed: {result.stderr}")
                    ir = destination.read_text()
                    for expected in expects:
                        if expected not in ir:
                            raise RuntimeError(f"{backend} missing expected IR: {expected}")
                    for rejected in rejects:
                        if rejected in ir:
                            raise RuntimeError(f"{backend} retained forbidden IR: {rejected}")
                    if exits:
                        executable, _ = run([args.lli, destination], local_env)
                        detail["exit"] = executable.returncode
                        if executable.returncode != int(exits[0]):
                            raise RuntimeError(f"{backend} execution exit {executable.returncode}: {executable.stderr}")
                    outputs[backend] = normalize(ir)
                if outputs["cpu"] != outputs["vulkan"]:
                    raise RuntimeError("CPU and Vulkan produce different normalized IR")
                row["ir_equal"] = True
                row["passed"] = True
                print(f"PASS {fixture.name}")
            except Exception as exc:
                row["passed"] = False
                row["error"] = str(exc)
                failures.append(fixture.name)
                print(f"FAIL {fixture.name}: {exc}")
            report["fixtures"].append(row)
    report["passed"] = len(fixtures) - len(failures)
    report["failed"] = len(failures)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(f"{report['passed']}/{len(fixtures)} fixtures passed with required Vulkan execution")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
