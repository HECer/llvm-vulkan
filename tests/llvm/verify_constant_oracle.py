#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
"""Compare device constant folding with upstream LLVM InstCombine.

This oracle does not calculate expected answers with lv's CPU implementation.
It generates defined, unflagged scalar operations, asks an independent upstream
LLVM pass for each constant, and compares every returned bit pattern. Poison,
undef, unsafe shifts/division, vectors and flags have separate preservation
fixtures. This is a correctness test, never a compile-time performance claim.
"""
import argparse
import json
import os
from pathlib import Path
import random
import re
import subprocess
import tempfile


OPS = ['add', 'sub', 'mul', 'and', 'or', 'xor', 'shl', 'lshr', 'ashr',
       'udiv', 'sdiv', 'urem', 'srem', 'icmp eq', 'icmp ne', 'icmp ult',
       'icmp ule', 'icmp ugt', 'icmp uge', 'icmp slt', 'icmp sle',
       'icmp sgt', 'icmp sge']


def input_ir(cases: int, seed: int) -> str:
    rng = random.Random(seed)
    result = ['source_filename = "llvm_vulkan_constant_oracle"', '']
    for i in range(cases):
        op = OPS[i % len(OPS)]
        width = rng.choice([1, 2, 3, 7, 8, 16, 17, 31, 32, 33, 48, 63, 64])
        mask = (1 << width) - 1
        edges = [0, 1, mask, 1 << (width-1), (1 << (width-1))-1]
        a = rng.choice(edges) if i % 3 else rng.randrange(mask + 1)
        b = rng.choice(edges) if i % 5 else rng.randrange(mask + 1)
        if op in ['shl', 'lshr', 'ashr']:
            b %= width
        if op in ['udiv', 'sdiv', 'urem', 'srem']:
            b = b or 1
            if op in ['sdiv', 'srem'] and a == 1 << (width-1) and b == mask:
                a = 0
        out_width = 1 if op.startswith('icmp ') else width
        result += [f'define i{out_width} @f{i}() {{',
                   f'  %value = {op} i{width} {a}, {b}',
                   f'  ret i{out_width} %value', '}', '']
    return '\n'.join(result)


def returns(ir: str) -> dict[str, str]:
    found = {}
    for name, body in re.findall(r'define\s+\w+\s+@(f\d+)\([^)]*\)[^{]*\{(.*?)\}', ir, re.S):
        match = re.search(r'\bret\s+(i\d+\s+(?:-?\d+|true|false))\s*(?:\n|$)', body)
        if match:
            found[name] = match.group(1)
    return found


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--opt', required=True)
    parser.add_argument('--plugin', required=True)
    parser.add_argument('--backend', choices=['cpu', 'vulkan'], default='vulkan')
    parser.add_argument('--device', choices=['cpu', 'gpu', 'any'], default='any')
    parser.add_argument('--shader-dir')
    parser.add_argument('--cases', type=int, default=2048)
    parser.add_argument('--seed', type=int, default=5090)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if not 23 <= args.cases <= 1000000:
        parser.error('cases must be 23..1000000')
    env = dict(os.environ, LV_BACKEND=args.backend, LV_DEVICE=args.device,
               LV_VERIFY='0', LV_TRACE='1')
    if args.shader_dir:
        env['LV_SHADER_DIR'] = args.shader_dir
    with tempfile.TemporaryDirectory(prefix='lv-llvm-oracle-') as tmp:
        source = Path(tmp) / 'input.ll'
        source.write_text(input_ir(args.cases, args.seed), encoding='utf-8')
        common = [args.opt, '-verify-each', '-S', str(source), '-o', '-']
        oracle = subprocess.run(common + ['-passes=instcombine'],
                                capture_output=True, text=True, timeout=300)
        actual = subprocess.run(common + ['-load-pass-plugin=' + args.plugin,
                                           '-passes=lv-fold'], env=env,
                                capture_output=True, text=True, timeout=300)
    if oracle.returncode:
        raise RuntimeError('upstream LLVM oracle failed:\n' + oracle.stderr)
    if actual.returncode:
        raise RuntimeError('LLVM Vulkan failed:\n' + actual.stderr)
    expected, obtained = returns(oracle.stdout), returns(actual.stdout)
    if len(expected) != args.cases or len(obtained) != args.cases:
        raise AssertionError(f'not every case folded: oracle={len(expected)}, '
                             f'actual={len(obtained)}, requested={args.cases}')
    differences = [{ 'function': k, 'expected': v, 'actual': obtained.get(k)}
                   for k, v in expected.items() if obtained.get(k) != v]
    report = {'test': 'upstream-llvm-instcombine-constant-oracle',
              'cases': args.cases, 'seed': args.seed, 'backend': args.backend,
              'device_request': args.device, 'passed': not differences,
              'mismatches': differences[:20], 'trace': actual.stderr}
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(report, indent=2))
    if differences:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
