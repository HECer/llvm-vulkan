#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
"""Generate an offline dashboard from the committed benchmark evidence."""
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LABELS = {'integer-fold': 'Integer fold · 32-bit', 'integer-fold64': 'Integer fold · 64-bit',
          'exclusive-scan': 'Exclusive scan', 'sort-keys': 'Sort keys', 'hash-records': 'Hash records',
          'reachability': 'Reachability · 257 nodes', 'dominators': 'Dominators · 257 nodes',
          'liveness': 'Liveness · 257 nodes'}
MODULES = {'independent': '64 chains of 128', 'mixed-32': '64 chains of 32 + star',
           'mixed-128': '64 chains of 128 + star', 'mixed-512': '64 chains of 512 + star',
           'mixed-long-large': '64 chains of 128 + chain',
           'low-local-coverage': 'One chain of 32 + 8,192-node star',
           'two-large': '64 chains of 128 + two stars', 'shallow-global': 'One node + star'}


def main():
    datasets = []
    for key, filename, label, left, right in [
        ('cpu', 'cpu-vs-gpu-rtx5090.json', 'CPU versus GPU', 'CPU reference', 'RTX 5090'),
        ('iteration1', 'vulkan-optimization.json', 'Vulkan iteration 1', 'Original Vulkan', 'Iteration 1'),
        ('iteration2', 'vulkan-iteration2.json', 'Vulkan iteration 2', 'Iteration 1', 'Iteration 2'),
    ]:
        path = ROOT / 'evidence' / filename
        source = json.loads(path.read_text(encoding='utf-8'))
        comparison = source if key == 'cpu' else (source['comparison'] if key == 'iteration1' else source['final_core_comparison'])
        rows = []

        def add(row, module=None, report=None):
            if row.get('skipped'):
                return
            a, b = ('cpu', 'gpu') if key == 'cpu' else ('baseline', 'candidate')
            size = int(row['case'][5:]) if key == 'cpu' and module is None else row.get('batch_size')
            rows.append({'label': MODULES[module] if module else LABELS[row['workload']],
                         'group': 'modules' if module else ('smallgraphs' if row['workload'] in ['reachability', 'dominators', 'liveness'] else 'arrays'),
                         'size': None if module else size, 'items': row.get('items', row[a].get('items')),
                         'left': row[a]['median_ms'], 'right': row[b]['median_ms'],
                         'left_p95': row[a]['p95_ms'], 'right_p95': row[b]['p95_ms'],
                         'samples': len(row[a]['samples_ms'])})

        for row in comparison['rows']:
            module = row['case'] if key == 'cpu' and not row['case'].startswith('core-') else None
            add(row, module)
        if key == 'iteration2':
            for module, report in source['final_graph_comparisons'].items():
                add(report['rows'][0], module, report)
        datasets.append({'key': key, 'label': label, 'left': left, 'right': right, 'rows': rows,
                         'source': '../../evidence/' + filename,
                         'sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
                         'meaning': ('The CPU is this project’s single-threaded C++ reference; sorting uses std::sort. This is not upstream LLVM or a tuned parallel CPU library.'
                                     if key == 'cpu' else 'Both sides use Vulkan. This comparison measures incremental improvements over the named earlier Vulkan implementation.')})
    template = (ROOT / 'scripts/templates/benchmarks.html').read_text(encoding='utf-8')
    icon = (ROOT / 'docs/assets/icon.svg').read_text(encoding='utf-8')
    output = template.replace('__ICON__', icon).replace('__BENCHMARK_DATA__', json.dumps(datasets, ensure_ascii=False))
    target = ROOT / 'docs/graphs/benchmarks.html'
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(output, encoding='utf-8', newline='\n')
    print(f'Generated {len(datasets)} comparisons / {sum(len(d["rows"]) for d in datasets)} rows.')


if __name__ == '__main__':
    main()
