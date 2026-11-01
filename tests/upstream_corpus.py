#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
"""Bounded structural/differential validation on pinned upstream LLVM IR.

Sample up to 120 verifier-valid .ll files from DCE, EarlyCSE, SimplifyCFG, and
InstCombine. Never execute the IR: upstream regression tests can contain UB or
depend on lit substitutions. This tests verifier preservation and CPU/Vulkan
IR parity, not transformation semantics or the entire upstream test suite.
"""

from __future__ import annotations

import argparse
from collections import Counter, deque
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time


SUITES = ('DCE', 'EarlyCSE', 'SimplifyCFG', 'InstCombine')


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def normalize(ir: str) -> str:
    return '\n'.join(line for line in ir.splitlines()
                     if not line.startswith('; ModuleID =')
                     and not line.startswith('source_filename ='))


def command(argv: list[str], env: dict[str, str], timeout: float):
    started = time.perf_counter()
    proc = subprocess.run(argv, env=env, capture_output=True, text=True,
                          timeout=timeout)
    return proc, time.perf_counter() - started


def source_identity(source: Path, lock_path: Path, channel: str) -> dict:
    pin = json.loads(lock_path.read_text(encoding='utf-8'))[channel]
    marker = source / '.llvm-vulkan-source.json'
    if marker.is_file():
        recorded = json.loads(marker.read_text(encoding='utf-8'))
        commit = recorded.get('commit')
        provenance = 'fetch_llvm source marker; sampled file hashes recorded below'
        if recorded.get('archive_sha256') != pin['archive_sha256']:
            raise ValueError('source marker archive digest differs from source lock')
    else:
        result = subprocess.run(['git', '-C', str(source), 'rev-parse', 'HEAD'],
                                capture_output=True, text=True, timeout=30)
        if result.returncode:
            raise ValueError('source needs fetch_llvm provenance or a Git checkout at the pin')
        commit = result.stdout.strip()
        provenance = 'Git HEAD; sampled file hashes recorded below'
    if commit != pin['commit']:
        raise ValueError(f'source commit {commit} differs from pinned {pin["commit"]}')
    version = source / 'cmake/Modules/LLVMVersion.cmake'
    if not version.is_file() or digest(version) != pin['version_file_sha256']:
        raise ValueError('source LLVMVersion.cmake differs from the pinned snapshot')
    return {'commit': commit, 'version': pin['version'], 'channel': channel,
            'locked_archive_sha256': pin['archive_sha256'],
            'version_file_sha256': pin['version_file_sha256'],
            'provenance': provenance}


def selection(source: Path, seed: int) -> dict[str, deque[Path]]:
    result = {}
    for suite in SUITES:
        directory = source / 'llvm/test/Transforms' / suite
        if not directory.is_dir():
            raise ValueError(f'missing upstream test directory: {directory}')
        candidates = list(directory.rglob('*.ll'))
        # SHA ordering avoids relying on filesystem iteration or PRNG versions.
        candidates.sort(key=lambda p: hashlib.sha256(
            (str(seed) + '\0' + p.relative_to(source).as_posix()).encode()).digest())
        result[suite] = deque(candidates)
    return result


def preflight_skip(source: str) -> tuple[str, str] | None:
    if re.search(r'^;\s*---+\s*\S+', source, re.MULTILINE) or re.search(
            r'^;\s*RUN:.*\bsplit-file\b', source, re.MULTILINE):
        return 'multi-module', 'split-file fixture requires the upstream lit harness'
    return None


def verifier_skip(stderr: str) -> tuple[str, str]:
    # Preserve the actual diagnostic; categories are only a readable summary.
    lower = stderr.lower()
    if any(word in lower for word in ('unable to get target', 'unknown target',
                                      'no available targets', 'unsupported target',
                                      'not a recognized feature', 'not a recognized processor')):
        category = 'target-feature'
    else:
        category = 'invalid-or-incompatible-ir'
    return category, stderr.strip()[:6000] or 'baseline verifier returned nonzero without a diagnostic'


def retain_failure(directory: Path, source: Path, outputs: list[Path], row: dict) -> str:
    location = directory / (source.stem + '-' + row['sha256'][:12])
    location.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, location / 'original.ll')
    for output in outputs:
        if output.exists():
            shutil.copy2(output, location / output.name)
    (location / 'failure.json').write_text(json.dumps(row, indent=2) + '\n', encoding='utf-8')
    return str(location.resolve())


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--opt', type=Path, required=True)
    parser.add_argument('--plugin', type=Path, required=True)
    parser.add_argument('--lock', type=Path,
                        default=Path(__file__).resolve().parents[1] / 'upstream-lock.json')
    parser.add_argument('--channel', choices=['main', 'release'], default='main')
    parser.add_argument('--seed', type=int, default=5090)
    parser.add_argument('--limit', type=int, default=120)
    parser.add_argument('--max-candidates', type=int, default=240)
    parser.add_argument('--max-bytes', type=int, default=2 * 1024 * 1024)
    parser.add_argument('--timeout', type=float, default=60)
    parser.add_argument('--device', choices=['cpu', 'gpu', 'any'], default='any')
    parser.add_argument('--device-name', default='')
    parser.add_argument('--shader-dir', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--keep-failure-dir', type=Path)
    args = parser.parse_args()
    if not 1 <= args.limit <= 120:
        parser.error('limit must be 1..120; this is a bounded sample, not the full suite')
    if not args.limit <= args.max_candidates <= 480:
        parser.error('max-candidates must be between limit and 480')
    if not 1 <= args.max_bytes <= 32 * 1024 * 1024 or not 1 <= args.timeout <= 300:
        parser.error('max-bytes must be 1..32MiB and timeout 1..300 seconds')
    for path in (args.opt, args.plugin, args.lock):
        if not path.is_file():
            parser.error(f'required file missing: {path}')
    source = args.source.resolve()
    pin = source_identity(source, args.lock, args.channel)
    queues = selection(source, args.seed)
    env = dict(os.environ, LV_AUTO_INSERT='0', LV_TRACE='1', LV_VERIFY='1',
               LV_DEVICE=args.device, LV_DEVICE_NAME=args.device_name)
    if args.shader_dir:
        env['LV_SHADER_DIR'] = str(args.shader_dir.resolve())
    version, _ = command([str(args.opt.resolve()), '--version'], env, args.timeout)
    if version.returncode:
        raise RuntimeError('cannot run selected opt: ' + version.stderr)
    report = {
        'schema_version': 1,
        'test': 'bounded-upstream-LLVM-IR-structural-corpus',
        'scope': 'baseline verifier, plugin verifier preservation, normalized CPU/Vulkan IR parity',
        'semantic_proof': False,
        'ir_executed': False,
        'whole_upstream_suite': False,
        'source_pin': pin,
        'llvm_version': version.stdout.strip(),
        'plugin_sha256': digest(args.plugin),
        'selection_seed': args.seed,
        'selection_algorithm': 'SHA256(seed + NUL + source-relative path), balanced round robin across suites',
        'requested_valid_files': args.limit,
        'candidate_cap': args.max_candidates,
        'maximum_fixture_bytes': args.max_bytes,
        'timeout_seconds_per_command': args.timeout,
        'device_requested': args.device,
        'device_name_requested': args.device_name,
        'suite_inventory': {suite: len(queue) for suite, queue in queues.items()},
        'tested': [], 'skipped': [], 'failures': [],
    }
    attempted, cursor = 0, 0
    with tempfile.TemporaryDirectory(prefix='lv-upstream-corpus-') as temporary:
        temporary = Path(temporary)
        env.setdefault('MESA_SHADER_CACHE_DIR', str(temporary / 'mesa-cache'))
        while len(report['tested']) < args.limit and attempted < args.max_candidates:
            active = [suite for suite in SUITES if queues[suite]]
            if not active:
                break
            suite = active[cursor % len(active)]
            cursor += 1
            fixture = queues[suite].popleft()
            attempted += 1
            row = {'path': fixture.relative_to(source).as_posix(), 'suite': suite,
                   'sha256': digest(fixture), 'bytes': fixture.stat().st_size}
            outputs = []
            if row['bytes'] > args.max_bytes:
                row.update(category='resource-limit', reason='fixture exceeds configured maximum bytes')
                report['skipped'].append(row)
                continue
            skip = preflight_skip(fixture.read_text(encoding='utf-8', errors='replace'))
            if skip:
                row.update(category=skip[0], reason=skip[1])
                report['skipped'].append(row)
                continue
            try:
                baseline, seconds = command([str(args.opt.resolve()), '-passes=verify',
                                             '-disable-output', str(fixture)], env, args.timeout)
            except subprocess.TimeoutExpired:
                row.update(category='baseline-resource-limit',
                           reason='baseline verifier exceeded the configured timeout')
                report['skipped'].append(row)
                continue
            row['baseline_verify_seconds'] = seconds
            if baseline.returncode:
                category, reason = verifier_skip(baseline.stderr)
                row.update(category=category, reason=reason,
                           baseline_returncode=baseline.returncode)
                report['skipped'].append(row)
                continue
            row['baseline_warnings'] = baseline.stderr[:6000]
            row['backends'] = {}
            texts = []
            try:
                for backend in ('cpu', 'vulkan'):
                    output = temporary / (backend + '.ll')
                    # Do not accidentally retain an earlier fixture as a failed output.
                    output.unlink(missing_ok=True)
                    outputs.append(output)
                    transformed, seconds = command([
                        str(args.opt.resolve()), '-load-pass-plugin=' + str(args.plugin.resolve()),
                        '-passes=lv-pipeline', '-verify-each', '-S', str(fixture), '-o', str(output)],
                        dict(env, LV_BACKEND=backend), args.timeout)
                    detail = {'returncode': transformed.returncode, 'seconds': seconds,
                              'trace': transformed.stderr[:16000]}
                    row['backends'][backend] = detail
                    if transformed.returncode:
                        raise RuntimeError(f'{backend} transformation/verification failed')
                    if not output.is_file():
                        raise RuntimeError(f'{backend} succeeded without its requested IR output')
                    text = normalize(output.read_text(encoding='utf-8'))
                    detail['normalized_ir_sha256'] = hashlib.sha256(text.encode()).hexdigest()
                    detail['dispatches'] = sum(int(n) for n in re.findall(
                        r'\bdispatches=(\d+)', transformed.stderr))
                    texts.append(text)
                if texts[0] != texts[1]:
                    raise RuntimeError('CPU/Vulkan normalized IR mismatch')
                row.update(passed=True, normalized_ir_equal=True)
                report['tested'].append(row)
                print(f'PASS {len(report["tested"]):03d} {row["path"]}', flush=True)
            except Exception as error:
                row.update(passed=False, failure=str(error))
                if args.keep_failure_dir:
                    row['retained_failure'] = retain_failure(args.keep_failure_dir, fixture, outputs, row)
                report['failures'].append(row)
                print(f'FAIL {row["path"]}: {error}', flush=True)
                break  # A transformation failure is fatal, never a skip.
    report['attempted_files'] = attempted
    report['tested_files'] = len(report['tested'])
    report['skipped_files'] = len(report['skipped'])
    report['failed_files'] = len(report['failures'])
    report['tested_by_suite'] = dict(Counter(row['suite'] for row in report['tested']))
    report['skipped_by_reason'] = dict(Counter(row['category'] for row in report['skipped']))
    report['vulkan_dispatches'] = sum(row['backends']['vulkan']['dispatches'] for row in report['tested'])
    report['coverage_target_met'] = len(report['tested']) == args.limit
    report['passed'] = bool(report['tested']) and not report['failures'] and report['vulkan_dispatches'] > 0
    report['stopping_reason'] = ('transformation-failure' if report['failures'] else
                                 'target-reached' if report['coverage_target_met'] else
                                 'candidate-cap' if attempted == args.max_candidates else 'source-exhausted')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({key: report[key] for key in (
        'passed', 'tested_files', 'skipped_files', 'failed_files', 'tested_by_suite',
        'skipped_by_reason', 'vulkan_dispatches', 'coverage_target_met', 'stopping_reason')}, indent=2))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
