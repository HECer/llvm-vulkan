#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
"""Rotate old/new core benchmark processes and preserve inclusive raw samples."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import statistics
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--device-name", default="")
    parser.add_argument("--device", choices=("gpu", "cpu", "any"), default="gpu")
    parser.add_argument("--sizes", type=int, nargs="+", default=[1024, 16384, 262144, 1048576])
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--repeat", type=int, default=9)
    parser.add_argument("--timeout", type=float, default=300)
    parser.add_argument("--benchmark-arg", action="append", default=[],
                        help="Extra argument passed identically to both binaries; use --benchmark-arg=--flag")
    args = parser.parse_args()
    if not (1 <= args.rounds <= 20 and 1 <= args.repeat <= 1000):
        parser.error("rounds must be 1..20 and repeat must be 1..1000")
    if any(n < 1 or n > 16777216 for n in args.sizes):
        parser.error("sizes must be 1..16777216")
    for binary in (args.baseline, args.candidate):
        if not binary.is_file():
            parser.error(f"Missing executable: {binary}")
    args.output.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, LV_VALIDATION="0")
    for name in ("VK_INSTANCE_LAYERS", "VK_LAYER_ENABLES"):
        env.pop(name, None)
    reports = []
    device_signature = None
    modes = [("baseline", args.baseline), ("candidate", args.candidate)]
    for round_index in range(args.rounds):
        for size_index, size in enumerate(args.sizes):
            order = modes if (round_index + size_index) % 2 == 0 else modes[::-1]
            for label, binary in order:
                output = args.output / f"{label}-{size}-{round_index}.json"
                command = [str(binary.resolve()), "--backend", "vulkan", "--device", args.device,
                           "--size", str(size), "--repeat", str(args.repeat), "--json", str(output)]
                if args.device_name:
                    command += ["--device-name", args.device_name]
                command += args.benchmark_arg
                proc = subprocess.run(command, env=env, capture_output=True, timeout=args.timeout)
                (args.output / f"{label}-{size}-{round_index}.log").write_bytes(proc.stdout + proc.stderr)
                if proc.returncode:
                    raise RuntimeError(f"{label} failed: {proc.stderr.decode(errors='replace')}")
                report = json.loads(output.read_text(encoding="utf-8-sig"))
                signature = (report["device"], report["vendor_id"], report["device_id"])
                if device_signature is not None and signature != device_signature:
                    raise RuntimeError("Device changed between comparison processes")
                device_signature = signature
                if report["backend"] != "vulkan" or (args.device == "gpu" and report["is_cpu_device"]):
                    raise RuntimeError("Requested Vulkan backend/device was not used")
                if any(not w.get("skipped") and
                       (not w.get("correct") or len(w["samples_ms"]) != args.repeat)
                       for w in report["workloads"]):
                    raise RuntimeError("Incomplete or incorrect benchmark output")
                reports.append((label, size, report))
                print(f"{label} size={size} round={round_index} correct=true", flush=True)
    rows = []
    for size in args.sizes:
        names = [w["name"] for label, n, r in reports if label == "baseline" and n == size
                 for w in r["workloads"]]
        for name in dict.fromkeys(names):
            row = {"batch_size": size, "workload": name}
            matching = [w for _, n, r in reports if n == size for w in r["workloads"]
                        if w["name"] == name]
            if any(w.get("skipped") for w in matching):
                if not all(w.get("skipped") for w in matching):
                    raise RuntimeError(f"Inconsistent capability skip for {name}")
                row.update(skipped=True, reason=matching[0].get("reason", "unsupported"))
                rows.append(row)
                continue
            for label, _ in modes:
                workloads = [w for mode, n, r in reports if mode == label and n == size
                             for w in r["workloads"] if w["name"] == name]
                samples = sorted(x for w in workloads for x in w["samples_ms"])
                row[label] = {"median_ms": statistics.median(samples),
                              "p95_ms": samples[max(0, (95 * len(samples) + 99) // 100 - 1)],
                              "samples_ms": samples, "items": workloads[0]["items"],
                              "last_call_counters": {k: v for k, v in workloads[-1].items()
                                                     if k.endswith("_per_call")}}
            row["speedup"] = row["baseline"]["median_ms"] / row["candidate"]["median_ms"]
            rows.append(row)
    result = {"recorded_at_utc": datetime.now(timezone.utc).isoformat(),
              "measurement": "inclusive core API host wall time, warm calls; not compiler speedup",
              "ordering": "process order alternates by round and size; cold calls excluded from summary",
              "device_requested": args.device, "device_name_requested": args.device_name,
              "device_names": sorted({r["device"] for _, _, r in reports}),
              "rounds": args.rounds, "repeat": args.repeat, "validation": False,
              "benchmark_arguments": args.benchmark_arg,
              "binary_sha256": {label: hashlib.sha256(binary.read_bytes()).hexdigest()
                                 for label, binary in modes},
              "rows": rows}
    (args.output / "comparison.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print("Wrote", args.output / "comparison.json")


if __name__ == "__main__":
    main()
