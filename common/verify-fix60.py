#!/usr/bin/env python3
"""Check actual Fix60 helpers, then optionally run historical tests on a copy.

The old Debug Tools guards intentionally freeze renderer behavior. They must
not be weakened to accept a core optimization. --baseline-phase makes a private
source copy, reverses ONLY Fix60 with git apply, and runs those unchanged guards
there. The caller's source and build output are never modified.
Requires Python 3, git, a C compiler; baseline native tests also need C++.
No Vulkan device or complete emulator build is exercised by these tests.
"""
from __future__ import annotations
import argparse
import pathlib
import shutil
import subprocess
import sys
import tempfile


def run(command: list[str], cwd: pathlib.Path) -> None:
    print('+ ' + ' '.join(command), flush=True)
    subprocess.run(command, cwd=cwd, check=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', required=True, type=pathlib.Path,
                        help='Reconstructed XEMU source with Fix60 already applied')
    parser.add_argument('--patch', type=pathlib.Path,
                        default=pathlib.Path(__file__).resolve().parents[1] /
                        'FIX/60-NV2A-Vulkan-Vertex-Snapshots.patch')
    parser.add_argument('--cc', default='cc')
    parser.add_argument('--cxx', default='c++')
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--ndebug', action='store_true')
    parser.add_argument('--baseline-phase', action='append', default=[],
                        choices=('static', 'native'),
                        help='Unchanged historical tests on a Fix60-reversed copy')
    parser.add_argument('--test', action='append', default=[],
                        help='Optional historical test glob; repeatable')
    parser.add_argument('--jobs', type=int, default=4)
    args = parser.parse_args()
    source = args.source.resolve()
    patch = args.patch.resolve()
    test = source / 'tests/unit/test-nv2a-vk-vertex-snapshot.py'
    if not test.is_file() or not patch.is_file():
        parser.error('Fix60 test script and patch must both exist')
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    command = [sys.executable, str(test), '--root', str(source),
               '--compiler', args.cc, '--negative']
    if args.sanitize:
        command.append('--sanitize')
    if args.ndebug:
        command.append('--ndebug')
    print('=== Actual Fix60 planner / renderer helpers / lifecycle ===', flush=True)
    run(command, source)
    if args.baseline_phase:
        print('=== Historical guards: reverse only Fix60 IN A TEMPORARY COPY ===',
              flush=True)
        with tempfile.TemporaryDirectory(prefix='xemu-fix60-baseline-') as directory:
            baseline = pathlib.Path(directory) / 'source'
            shutil.copytree(source, baseline, symlinks=True,
                            ignore=shutil.ignore_patterns('.git', '__pycache__',
                                                         '*.pyc'))
            for check in (True, False):
                command = ['git', 'apply', '--reverse', '--whitespace=nowarn']
                if check:
                    command.append('--check')
                run(command + [str(patch)], baseline)
            runner = baseline / 'ui/xui/debug-tools/tests/v287-run-regression-tests.py'
            if not runner.is_file():
                parser.error('Baseline phases require the optional Debug Tools source')
            for phase in args.baseline_phase:
                command = [sys.executable, str(runner), '--root', str(baseline),
                           '--phase', phase, '--compiler', args.cxx,
                           '--jobs', str(args.jobs)]
                for pattern in args.test:
                    command += ['--test', pattern]
                run(command, baseline)
    print('PASS: requested Fix60 checks. No full emulator/GPU/game run performed.',
          flush=True)
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, subprocess.CalledProcessError) as error:
        print(f'FAIL: {error}', file=sys.stderr)
        raise SystemExit(1)
