#!/usr/bin/env python3
"""Generate large valid, executable IR to expose module-level batching.

This synthetic workload intentionally contains redundant scalar expressions.
It measures candidate transforms, not realistic whole-program compile speed.
The output checksum is independently derived in Python and checked by main.
"""

import argparse
from pathlib import Path


def generate(functions: int, width: int, dead: int) -> str:
    lines = ['; Generated synthetic LLVM Vulkan workload.', 'source_filename = "lv_synthetic"', '']
    expected = 0
    for f in range(functions):
        lines += [f'define i32 @work_{f}(i32 %input) {{', 'entry:']
        checksum = 0
        for i in range(width):
            a = (f * 104729 + i * 13) & 0xFFFFFFFF
            b = (i * 31 + 17) & 0xFFFFFFFF
            value = (a + b) & 0xFFFFFFFF
            checksum ^= value
            lines += [f'  %v{i} = add i32 {a}, {b}']
        for d in range(dead):
            operand = '%input' if d == 0 else f'%unused{d-1}'
            lines += [f'  %unused{d} = mul i32 {operand}, {d + 3}']
        if width == 0:
            lines += ['  ret i32 0']
        elif width == 1:
            lines += ['  ret i32 %v0']
        else:
            lines += ['  %sum1 = xor i32 %v0, %v1']
            for i in range(2, width):
                lines += [f'  %sum{i} = xor i32 %sum{i-1}, %v{i}']
            lines += [f'  ret i32 %sum{width-1}']
        lines += ['}', '']
        expected = (expected + checksum) & 0xFFFFFFFF
    lines += ['define i32 @main() {', 'entry:']
    for f in range(functions):
        lines += [f'  %call{f} = call i32 @work_{f}(i32 {f})']
        if f:
            previous = '%call0' if f == 1 else f'%total{f-1}'
            lines += [f'  %total{f} = add i32 {previous}, %call{f}']
    result = '%call0' if functions == 1 else f'%total{functions-1}'
    lines += [f'  %ok = icmp eq i32 {result}, {expected}',
              '  %exit = select i1 %ok, i32 0, i32 1', '  ret i32 %exit', '}', '']
    return '\n'.join(lines)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--functions', type=int, default=128)
    parser.add_argument('--width', type=int, default=256)
    parser.add_argument('--dead', type=int, default=16)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if not 1 <= args.functions <= 100000 or not 0 <= args.width <= 100000 or not 0 <= args.dead <= 100000:
        parser.error('functions must be 1..100000; width and dead must be 0..100000')
    if args.functions * (args.width + args.dead) > 10000000:
        parser.error('requested input exceeds ten million generated instructions')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(generate(args.functions, args.width, args.dead), encoding='utf-8')


if __name__ == '__main__':
    main()
