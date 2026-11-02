#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
"""Compare this project's C++ CPU reference and Vulkan on matching inputs."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import statistics
import subprocess


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def bounded(value, maximum):
    n = int(value)
    if not 1 <= n <= maximum:
        raise argparse.ArgumentTypeError(f"expected 1..{maximum}")
    return n


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True,
                        help="New directory; existing results are never overwritten")
    parser.add_argument("--device-name", default="")
    parser.add_argument("--sizes", type=lambda s: bounded(s, 16777216), nargs="+",
                        default=[1024, 16384, 262144, 1048576])
    parser.add_argument("--graph-nodes", type=lambda s: bounded(s, 8192), default=257)
    parser.add_argument("--rounds", type=lambda s: bounded(s, 20), default=5)
    parser.add_argument("--repeat", type=lambda s: bounded(s, 1000), default=11)
    parser.add_argument("--seed", type=int, default=5090)
    parser.add_argument("--timestamps", choices=(0, 1), type=int, default=1)
    parser.add_argument("--include-graphs", action="store_true")
    parser.add_argument("--timeout", type=float, default=300)
    args = parser.parse_args()
    if not args.binary.is_file():
        parser.error("binary does not exist")
    if not 0 <= args.seed <= 0xffffffff or args.timeout <= 0:
        parser.error("seed must be uint32 and timeout must be positive")
    args.output.mkdir(parents=True, exist_ok=False)
    binary = args.binary.resolve()
    binary_hash = sha256(binary)
    env = dict(os.environ, LV_VALIDATION="0")
    for key in ("VK_INSTANCE_LAYERS", "VK_LAYER_ENABLES"):
        env.pop(key, None)
    cases = [{"name": f"core-{n}", "arguments": ["--size", str(n),
              "--graph-nodes", str(args.graph_nodes)], "workload": "all"}
             for n in dict.fromkeys(args.sizes)]
    if args.include_graphs:
        for name, shape, small, large_shape in [
            ("independent", "independent", 128, "star"),
            ("mixed-32", "mixed", 32, "star"),
            ("mixed-128", "mixed", 128, "star"),
            ("mixed-512", "mixed", 512, "star"),
            ("mixed-long-large", "mixed", 128, "chain"),
        ]:
            cases.append({"name": name, "workload": "reachability", "arguments": [
                "--size", "1024", "--workload", "reachability", "--graph-shape", shape,
                "--graph-nodes", str(small), "--components", "64",
                "--large-components", "1", "--large-nodes", "2048",
                "--large-shape", large_shape]})
    records = []
    gpu_signature = None
    try:
        for round_index in range(args.rounds):
            for case_index, case in enumerate(cases):
                order = ["cpu", "gpu"] if (round_index + case_index) % 2 == 0 else ["gpu", "cpu"]
                for side in order:
                    stem = f"{case['name']}-{side}-{round_index}"
                    report_path = args.output / f"{stem}.json"
                    command = [str(binary), "--backend", "cpu" if side == "cpu" else "vulkan",
                               "--repeat", str(args.repeat), "--seed", str(args.seed),
                               "--timestamps", str(args.timestamps), "--fast-paths", "1",
                               "--json", str(report_path.resolve())] + case["arguments"]
                    if side == "gpu":
                        command += ["--device", "gpu"]
                        if args.device_name:
                            command += ["--device-name", args.device_name]
                    proc = subprocess.run(command, env=env, capture_output=True, timeout=args.timeout)
                    (args.output / f"{stem}.log").write_bytes(proc.stdout + proc.stderr)
                    if proc.returncode:
                        raise RuntimeError(f"{stem} failed: {proc.stderr.decode(errors='replace')}")
                    report = json.loads(report_path.read_text(encoding="utf-8-sig"))
                    expected_backend = "cpu" if side == "cpu" else "vulkan"
                    if report["backend"] != expected_backend or report["seed"] != args.seed:
                        raise RuntimeError("Wrong backend or seed")
                    if side == "gpu":
                        signature = (report["device"], report["vendor_id"], report["device_id"])
                        if report["is_cpu_device"] or (gpu_signature is not None and signature != gpu_signature):
                            raise RuntimeError("Physical GPU selection changed or failed")
                        gpu_signature = signature
                    elif report["device"] != "C++ scalar reference" or not report["is_cpu_device"]:
                        raise RuntimeError("CPU reference backend was not selected")
                    workloads = report["workloads"]
                    names = [w["name"] for w in workloads]
                    expected_names = (["integer-fold", "integer-fold64", "reachability", "dominators",
                                       "liveness", "exclusive-scan", "sort-keys", "hash-records"]
                                      if case["workload"] == "all" else [case["workload"]])
                    if names != expected_names:
                        raise RuntimeError("Missing, duplicated or unexpected workloads")
                    for w in workloads:
                        if w.get("skipped"):
                            if side != "gpu" or w["name"] != "integer-fold64":
                                raise RuntimeError("Unexpected workload skip")
                        elif not w.get("correct") or len(w["samples_ms"]) != args.repeat or any(
                                not isinstance(v, (float, int)) or not 0 < v < float("inf")
                                for v in w["samples_ms"]):
                            raise RuntimeError("Incomplete, invalid or incorrect benchmark samples")
                    records.append({"case": case["name"], "side": side, "round": round_index,
                                    "command": command, "report_sha256": sha256(report_path),
                                    "report": report})
                    print(f"{case['name']} {side} round={round_index} correct=true", flush=True)
        if sha256(binary) != binary_hash:
            raise RuntimeError("Executable changed during measurement")
        rows = []
        for case in cases:
            matching = [r for r in records if r["case"] == case["name"]]
            graph_identity = {(r["report"]["graph_shape"], r["report"]["graph_nodes"],
                               r["report"]["graph_components"]) for r in matching}
            if len(graph_identity) != 1:
                raise RuntimeError("Graph inputs changed between processes")
            for name in [w["name"] for w in matching[0]["report"]["workloads"]]:
                row = {"case": case["name"], "workload": name}
                selected = {side: [next(w for w in r["report"]["workloads"] if w["name"] == name)
                                   for r in sorted(matching, key=lambda r: r["round"]) if r["side"] == side]
                            for side in ["cpu", "gpu"]}
                if any(w.get("skipped") for w in selected["gpu"]):
                    if not all(w.get("skipped") for w in selected["gpu"]):
                        raise RuntimeError("GPU capability changed")
                    row.update(skipped=True, reason="GPU shaderInt64 unavailable")
                    rows.append(row)
                    continue
                if len({w["items"] for ws in selected.values() for w in ws}) != 1:
                    raise RuntimeError("Input sizes changed")
                row["items"] = selected["cpu"][0]["items"]
                for side, ws in selected.items():
                    samples = sorted(x for w in ws for x in w["samples_ms"])
                    row[side] = {"median_ms": statistics.median(samples),
                                 "p95_ms": samples[(95 * len(samples) + 99) // 100 - 1],
                                 "samples_ms": samples,
                                 "process_medians_ms": [statistics.median(w["samples_ms"]) for w in ws]}
                ratios = [c / g for c, g in zip(row["cpu"]["process_medians_ms"],
                                               row["gpu"]["process_medians_ms"])]
                row.update(cpu_over_gpu=row["cpu"]["median_ms"] / row["gpu"]["median_ms"],
                           paired_process_ratios=ratios, gpu_faster_pairs=sum(v > 1 for v in ratios))
                rows.append(row)
        result = {"schema_version": 1, "recorded_at_utc": datetime.now(timezone.utc).isoformat(),
                  "scope": "Inclusive warm core API time; not whole-compiler acceleration",
                  "cpu_baseline": "This project's single-threaded C++ reference; sort uses std::sort; not upstream LLVM or a tuned parallel CPU library",
                  "ratio_definition": "cpu median / gpu median; above 1 favors GPU",
                  "ordering": "CPU/GPU process order alternates by round and case",
                  "cold_calls": "Separate in raw reports; excluded from warm rows",
                  "gpu_signature": gpu_signature, "binary_sha256": binary_hash,
                  "runner_sha256": sha256(Path(__file__)), "seed": args.seed,
                  "rounds": args.rounds, "repeat": args.repeat,
                  "timestamps": bool(args.timestamps), "validation": False,
                  "cases": cases, "rows": rows, "raw_process_reports": records}
        (args.output / "comparison.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        print("Wrote", args.output / "comparison.json")
    except Exception as exc:
        (args.output / "failure.json").write_text(json.dumps({"error": str(exc),
            "completed_processes": len(records), "summary_accepted": False}, indent=2) + "\n", encoding="utf-8")
        raise


if __name__ == "__main__":
    main()
