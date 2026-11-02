#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
"""Check offline documentation values against every original benchmark row."""
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import tempfile
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]


def main():
    document = (ROOT / 'docs/graphs/benchmarks.html').read_text(encoding='utf-8')
    data = json.loads(re.search(r'const datasets=(.*?);let selected=', document).group(1))
    assert [len(d['rows']) for d in data] == [37, 32, 40]
    assert sum(len(d['rows']) for d in data) == 109
    for dataset in data:
        path = (ROOT / 'docs/graphs' / dataset['source']).resolve()
        assert path.is_relative_to(ROOT / 'evidence')
        assert hashlib.sha256(path.read_bytes()).hexdigest() == dataset['sha256']
        for row in dataset['rows']:
            assert row['left'] > 0 and row['right'] > 0
            assert row['left_p95'] >= row['left'] and row['right_p95'] >= row['right']
    cpu = data[0]['rows']
    large = {r['label']: r for r in cpu if r['size'] == 1048576 and r['group'] == 'arrays'}
    assert large['Exclusive scan']['left'] < large['Exclusive scan']['right']
    assert large['Sort keys']['left'] > large['Sort keys']['right']
    assert all(r['left'] < r['right'] for r in cpu if r['group'] == 'modules')
    assert not re.search(r'<(?:script|link|img)[^>]+(?:src|href)=["\']https?://', document)
    # Rebuild in an isolated tree to catch stale generated assets without
    # modifying the source checkout during tests.
    spec = importlib.util.spec_from_file_location('graphs', ROOT / 'scripts/build_benchmark_graphs.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    with tempfile.TemporaryDirectory() as directory:
        temporary = Path(directory)
        assert temporary.resolve().is_relative_to(Path(tempfile.gettempdir()).resolve())
        for relative in ['scripts/templates/benchmarks.html', 'docs/assets/icon.svg']:
            target = temporary / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes((ROOT / relative).read_bytes())
        (temporary / 'evidence').mkdir()
        for dataset in data:
            path = ROOT / 'docs/graphs' / dataset['source']
            (temporary / 'evidence' / path.name).write_bytes(path.read_bytes())
        with patch.object(module, 'ROOT', temporary):
            module.main()
        assert (temporary / 'docs/graphs/benchmarks.html').read_text(encoding='utf-8') == document
    print('PASS all 109 documented benchmark rows, source hashes, CPU losses and offline regeneration')


if __name__ == '__main__':
    main()
