#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
"""Install, relocate and consume the package without its source/build includes."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for key in ['build-dir', 'cmake', 'generator']:
        parser.add_argument('--' + key, required=True)
    for key in ['config', 'platform', 'toolset']:
        parser.add_argument('--' + key, default='')
    args = parser.parse_args()
    build = Path(args.build_dir).resolve(strict=True)
    env = dict(os.environ)
    env.pop('LV_SHADER_DIR', None)
    with tempfile.TemporaryDirectory(prefix='install-test-', dir=build) as work:
        workspace = Path(work).resolve()
        assert workspace.is_relative_to(build)
        prefix = workspace / 'original'
        relocated = workspace / 'relocated'
        install = [args.cmake, '--install', str(build), '--prefix', str(prefix)]
        if args.config:
            install += ['--config', args.config]
        subprocess.run(install, env=env, check=True)
        prefix.rename(relocated)
        consumer = workspace / 'consumer'
        command = [args.cmake, '-S', str(Path(__file__).parent / 'install_consumer'),
                   '-B', str(consumer), '-G', args.generator,
                   '-DCMAKE_PREFIX_PATH=' + str(relocated), '-DCMAKE_BUILD_TYPE=Release']
        if args.platform:
            command += ['-A', args.platform]
        if args.toolset:
            command += ['-T', args.toolset]
        subprocess.run(command, env=env, check=True)
        compile_command = [args.cmake, '--build', str(consumer)]
        if args.config:
            compile_command += ['--config', args.config]
        subprocess.run(compile_command, env=env, check=True)
        executable = consumer / ('lv-installed-smoke.exe' if os.name == 'nt' else 'lv-installed-smoke')
        if not executable.exists() and args.config:
            executable = consumer / args.config / executable.name
        subprocess.run([str(executable)], cwd=workspace, env=env, check=True)


if __name__ == '__main__':
    main()
