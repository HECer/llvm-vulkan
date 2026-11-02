#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
"""Build the repository's offline project presentation from recorded evidence."""
import html
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    cpu = json.loads((ROOT / 'evidence/cpu-vs-gpu-rtx5090.json').read_text(encoding='utf-8'))
    first = json.loads((ROOT / 'evidence/vulkan-optimization.json').read_text(encoding='utf-8'))
    second = json.loads((ROOT / 'evidence/vulkan-iteration2.json').read_text(encoding='utf-8'))
    labels = {'integer-fold': 'Integer fold · 32-bit', 'integer-fold64': 'Integer fold · 64-bit',
              'exclusive-scan': 'Exclusive scan', 'sort-keys': 'Sort keys', 'hash-records': 'Hash records'}
    rows = [r for r in cpu['rows'] if r['case'] == 'core-1048576' and r['workload'] in labels]
    table = ''.join(f'<tr><td>{labels[r["workload"]]}</td><td>{r["cpu"]["median_ms"]:.3f}</td><td>{r["gpu"]["median_ms"]:.3f}</td><td>{r["cpu_over_gpu"]:.2f}×</td></tr>' for r in rows)
    first_rows = [r for r in first['comparison']['rows'] if r['batch_size'] == 1048576 and r['workload'] in ['integer-fold', 'exclusive-scan', 'sort-keys']]
    first_table = ''.join(f'<tr><td>{labels[r["workload"]]}</td><td>{r["baseline"]["median_ms"]:.3f}</td><td>{r["candidate"]["median_ms"]:.3f}</td><td>{r["speedup"]:.2f}×</td></tr>' for r in first_rows)
    graphs = []
    for case, label in [('mixed-128', '64 chains of 128 + star'), ('mixed-512', '64 chains of 512 + star'), ('mixed-long-large', '64 chains of 128 + chain')]:
        r = second['final_graph_comparisons'][case]['rows'][0]
        graphs.append(f'<div class="stat"><strong>{r["speedup"]:.2f}×</strong><span>{html.escape(label)}</span></div>')
    template = (ROOT / 'scripts/templates/project-presentation.html').read_text(encoding='utf-8')
    replacements = {'__ICON__': (ROOT / 'docs/assets/icon.svg').read_text(encoding='utf-8'),
                    '__CPU_TABLE__': table, '__FIRST_TABLE__': first_table,
                    '__SECOND_GRAPH_STATS__': ''.join(graphs)}
    for key, value in replacements.items():
        template = template.replace(key, value)
    (ROOT / 'docs/project-presentation.html').write_text(template, encoding='utf-8', newline='\n')
    print('Built twelve-slide project presentation.')


if __name__ == '__main__':
    main()
