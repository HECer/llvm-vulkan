#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
"""Measure complete opt processes, optionally native code generation and linking.

The default compares upstream O2 to the same O2 with our CPU/Vulkan prepasses.
It measures incremental compilation cost, not equivalence to a full pass port.
Every produced program is checked against the unmodified input with lli.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import statistics
import subprocess
import tempfile
import time


def execute(command, env, timeout):
    start = time.perf_counter()
    proc = subprocess.run([str(x) for x in command], capture_output=True, env=env,
                          timeout=timeout)
    return proc, (time.perf_counter() - start) * 1000


def require(proc, label):
    if proc.returncode:
        raise RuntimeError(f"{label} failed ({proc.returncode}): {proc.stderr.decode(errors='replace')}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--opt", type=Path, required=True)
    parser.add_argument("--lli", type=Path, required=True)
    parser.add_argument("--plugin", type=Path, required=True)
    parser.add_argument("--input", type=Path, required=True,
                        help="Defined executable IR with main; exit/stdout/stderr are compared")
    parser.add_argument("--llc", type=Path, help="Also time same-version LLVM native codegen")
    parser.add_argument("--cc", default="cc", help="Native executable linker driver")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--device", choices=("gpu", "cpu", "any"), default="gpu")
    parser.add_argument("--device-name", default="")
    parser.add_argument("--repeat", type=int, default=7)
    parser.add_argument("--warmup", type=int, default=1)
    parser.add_argument("--timeout", type=float, default=300)
    parser.add_argument("--pipeline", default="default<O2>")
    args = parser.parse_args()
    if not (1 <= args.repeat <= 100) or not (0 <= args.warmup <= 10):
        parser.error("repeat must be 1..100; warmup 0..10")
    for path in [args.opt, args.lli, args.plugin, args.input] + ([args.llc] if args.llc else []):
        if not path.is_file():
            parser.error(f"Missing file: {path}")
    env = dict(os.environ, LV_AUTO_INSERT="0", LV_TRACE="1", LV_VERIFY="0",
               LV_DEVICE=args.device, LV_DEVICE_NAME=args.device_name, LV_VALIDATION="0")
    # A parent test session must not silently enable expensive validation in a
    # performance run. These are standard Vulkan layer selectors, not drivers.
    env.pop("VK_INSTANCE_LAYERS", None)
    env.pop("VK_LAYER_ENABLES", None)
    report = {"schema_version": 1, "status": "experimental", "hardware_device_requested": args.device == "gpu",
              "input_sha256": hashlib.sha256(args.input.read_bytes()).hexdigest(),
              "pipeline": args.pipeline, "device_requested": args.device,
              "device_name_requested": args.device_name,
              "measurement": "new opt process each sample, inclusive of IR parsing, packing, initialization, transfer, mutation, upstream pipeline and output serialization",
              "verification": "full stdout/stderr and return code versus original IR; excluded from timed region",
              "native_codegen_and_link_included": args.llc is not None,
              "warning": "plugin runs before the same upstream pipeline; scalar plugin is not the upstream baseline; no optimization-quality equivalence of individual pass replacements is assumed",
              "runs": []}
    version, _ = execute([args.opt, "--version"], env, args.timeout)
    require(version, "opt --version")
    report["llvm_version"] = version.stdout.decode().strip()
    original, _ = execute([args.lli, args.input], env, args.timeout)
    if original.returncode < 0:
        raise RuntimeError("Original executable terminated by a signal")
    signature = (original.returncode, original.stdout, original.stderr)
    report["baseline_execution"] = {"returncode": original.returncode,
                                    "stdout_sha256": hashlib.sha256(original.stdout).hexdigest(),
                                    "stderr_sha256": hashlib.sha256(original.stderr).hexdigest()}
    modes = [("upstream", None), ("plugin-cpu", "cpu"), ("plugin-vulkan", "vulkan")]
    with tempfile.TemporaryDirectory(prefix="llvm-vulkan-benchmark-") as temporary:
        tmp = Path(temporary)
        env.setdefault("MESA_SHADER_CACHE_DIR", str(tmp / "mesa-cache"))
        # Rotate mode ordering to reduce systematic order/temperature bias.
        for iteration in range(args.warmup + args.repeat):
            order = modes[iteration % len(modes):] + modes[:iteration % len(modes)]
            for label, backend in order:
                local = dict(env, LV_BACKEND=backend or "cpu")
                output = tmp / f"{label}.bc"
                command = [args.opt]
                pipeline = args.pipeline
                if backend:
                    command += [f"-load-pass-plugin={args.plugin.resolve()}"]
                    pipeline = "lv-pipeline," + pipeline
                command += [f"-passes={pipeline}", args.input, "-o", output]
                proc, opt_ms = execute(command, local, args.timeout)
                require(proc, label)
                checked, _ = execute([args.opt, "-passes=verify", "-disable-output", output], local, args.timeout)
                require(checked, "output verifier " + label)
                execution, _ = execute([args.lli, output], local, args.timeout)
                if (execution.returncode, execution.stdout, execution.stderr) != signature:
                    raise RuntimeError(f"Semantic difference for {label}; stopping benchmark")
                row = {"mode": label, "iteration": iteration - args.warmup,
                       "warmup": iteration < args.warmup, "opt_ms": opt_ms,
                       "total_ms": opt_ms, "bitcode_bytes": output.stat().st_size,
                       "execution_equal": True, "trace": proc.stderr.decode(errors="replace")}
                row["vulkan_dispatches"] = sum(int(n) for n in re.findall(r"\bdispatches=(\d+)", row["trace"]))
                if args.llc:
                    obj = tmp / f"{label}.o"
                    binary = tmp / f"{label}.exe"
                    codegen, codegen_ms = execute([args.llc, "-O=2", "-filetype=obj",
                                                  "-relocation-model=pic", output, "-o", obj], local, args.timeout)
                    require(codegen, "llc " + label)
                    link, link_ms = execute([args.cc, obj, "-o", binary], local, args.timeout)
                    require(link, "link " + label)
                    native, _ = execute([binary], local, args.timeout)
                    if (native.returncode, native.stdout, native.stderr) != signature:
                        raise RuntimeError(f"Native semantic difference for {label}")
                    row.update(codegen_ms=codegen_ms, link_ms=link_ms,
                               total_ms=opt_ms + codegen_ms + link_ms,
                               object_bytes=obj.stat().st_size, executable_bytes=binary.stat().st_size,
                               native_execution_equal=True)
                report["runs"].append(row)
                print(f"{label} iteration={row['iteration']} total={row['total_ms']:.3f}ms equal=true", flush=True)
    summaries = {}
    for label, _ in modes:
        values = [r["total_ms"] for r in report["runs"] if r["mode"] == label and not r["warmup"]]
        summaries[label] = {"median_ms": statistics.median(values), "min_ms": min(values),
                            "max_ms": max(values), "samples": len(values)}
    base = summaries["upstream"]["median_ms"]
    for value in summaries.values():
        value["speedup_vs_upstream"] = base / value["median_ms"]
    report["summary"] = summaries
    report["hardware_execution_confirmed"] = args.device == "gpu" and any(
        r["mode"] == "plugin-vulkan" and r["vulkan_dispatches"] > 0 for r in report["runs"])
    report["passed"] = True
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(summaries, indent=2))


if __name__ == "__main__":
    main()
