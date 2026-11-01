#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
"""Deterministic semantic differential test for the complete LLVM plugin.

The generated functions use runtime arguments, branches/PHIs, duplicated pure
operations, nsw guards, noundef arguments, freeze, unselected poison, llvm.assume,
observable stores, and unreachable side-effecting blocks. Every executed path
is defined. The oracle is execution of the original IR by the same LLVM lli;
the test compares full printed uint32 values, not only a truncated exit status.
"""
import argparse
import json
import os
from pathlib import Path
import random
import subprocess
import tempfile


def program(functions: int, calls: int, seed: int) -> str:
    rng = random.Random(seed)
    ir = ['source_filename = "lv_differential_semantics"',
          '@format = private constant [4 x i8] c"%u\\0A\\00"',
          '@observable = global i32 0',
          'declare i32 @printf(ptr, ...)',
          'declare void @llvm.assume(i1 noundef)', '']
    for f in range(functions):
        c = rng.randrange(1, 128)
        ir += [f'define noundef i32 @f{f}(i32 noundef %a, i32 noundef %b) {{',
               'entry:',
               '  %in_range = icmp ult i32 %a, 1024',
               '  call void @llvm.assume(i1 %in_range)',
               f'  %base = add i32 %a, {c}',
               f'  %base_dup = add i32 %a, {c}',
               '  %product = mul i32 %base, %b',
               '  %product_dup = mul i32 %base, %b',
               f'  %flagged = add nsw i32 %a, {c}',
               '  %frozen = freeze i32 %a',
               '  %condition = icmp ult i32 %a, %b',
               '  %selected = select i1 %condition, i32 %product, i32 poison',
               '  %dead0 = mul i32 %a, 4711',
               '  %dead1 = xor i32 %dead0, %b',
               '  br i1 %condition, label %left, label %right',
               'left:',
               '  %left0 = xor i32 %selected, %product_dup',
               '  %left1 = add i32 %left0, %flagged',
               '  %left2 = add i32 %left1, %frozen',
               '  br label %join',
               'right:',
               '  %right0 = add i32 %product_dup, %base_dup',
               '  %right1 = xor i32 %right0, %frozen',
               '  br label %join',
               'join:',
               '  %result = phi i32 [%left2, %left], [%right1, %right]',
               '  store volatile i32 %result, ptr @observable',
               '  ret i32 %result',
               'unreachable_effect:',
               '  store volatile i32 123456789, ptr @observable',
               '  ret i32 99', '}', '']
    ir += ['define i32 @main() {', 'entry:']
    for c in range(calls):
        fn = c % functions
        a, b = rng.randrange(256), rng.randrange(256)
        ir += [f'  %r{c} = call i32 @f{fn}(i32 {a}, i32 {b})',
               f'  %p{c} = call i32 (ptr, ...) @printf(ptr @format, i32 %r{c})']
    ir += ['  %last = load volatile i32, ptr @observable',
           '  %p_last = call i32 (ptr, ...) @printf(ptr @format, i32 %last)',
           '  ret i32 0', '}', '']
    return '\n'.join(ir)


def run(command: list[str], env: dict[str, str], timeout: int = 180) -> subprocess.CompletedProcess:
    result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=timeout)
    if result.returncode:
        raise RuntimeError(f'command failed ({result.returncode}): {command}\n{result.stderr}')
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--opt', required=True)
    parser.add_argument('--lli', required=True)
    parser.add_argument('--plugin', required=True)
    parser.add_argument('--device', choices=['cpu', 'gpu', 'any'], default='any')
    parser.add_argument('--shader-dir')
    parser.add_argument('--functions', type=int, default=64)
    parser.add_argument('--calls', type=int, default=512)
    parser.add_argument('--seed', type=int, default=5090)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--keep-dir', type=Path,
                        help='Keep generated and transformed IR for reproducing a failure')
    args = parser.parse_args()
    if not 1 <= args.functions <= 10000 or not 1 <= args.calls <= 100000:
        parser.error('functions must be 1..10000; calls must be 1..100000')
    base_env = dict(os.environ, LV_DEVICE=args.device, LV_TRACE='1', LV_VERIFY='0')
    if args.shader_dir:
        base_env['LV_SHADER_DIR'] = args.shader_dir
    temporary = None
    if args.keep_dir:
        work = args.keep_dir.resolve()
        work.mkdir(parents=True, exist_ok=True)
    else:
        temporary = tempfile.TemporaryDirectory(prefix='lv-llvm-fuzz-')
        work = Path(temporary.name)
    source = work / 'original.ll'
    source.write_text(program(args.functions, args.calls, args.seed), encoding='utf-8')
    run([args.opt, '-passes=verify', '-disable-output', str(source)], base_env)
    baseline = run([args.lli, str(source)], base_env).stdout
    report = {'test': 'LLVM-pipeline-semantic-differential',
              'seed': args.seed, 'functions': args.functions, 'calls': args.calls,
              'output_values': len(baseline.splitlines()), 'runs': []}
    transformed = []
    try:
        for backend in ['cpu', 'vulkan']:
            output = work / (backend + '.ll')
            env = dict(base_env, LV_BACKEND=backend)
            optimization = run([args.opt, '-load-pass-plugin=' + args.plugin,
                                '-passes=lv-pipeline', '-verify-each', '-S',
                                str(source), '-o', str(output)], env)
            actual = run([args.lli, str(output)], env).stdout
            if actual != baseline:
                raise AssertionError(f'{backend} output differs from unmodified IR; seed={args.seed}')
            transformed.append(output.read_text(encoding='utf-8'))
            report['runs'].append({'backend': backend, 'matches_baseline': True,
                                   'trace': optimization.stderr})
        if transformed[0] != transformed[1]:
            raise AssertionError('CPU and Vulkan emitted different IR')
        report['identical_transformed_ir'] = True
        report['passed'] = True
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
        print(json.dumps(report, indent=2))
    finally:
        if temporary:
            temporary.cleanup()


if __name__ == '__main__':
    main()
