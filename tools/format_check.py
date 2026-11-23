#!/usr/bin/env python3
"""The format check tools/lint.sh runs: clang-format on the lines of the C
sources and headers under src/ and tests/ that changed since a base commit.
Unlike git clang-format it follows renames, so a moved file is checked only
where its text changed, not as a new file from top to bottom."""
import argparse
from pathlib import Path
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from pr_check import added_lines  # noqa: E402 (the import needs the path above)

C_SUFFIXES = ('.c', '.h')


def runs(lines):
    """Line numbers as sorted (first, last) runs of consecutive lines."""
    out = []
    for n in sorted(set(lines)):
        if out and n == out[-1][1] + 1:
            out[-1][1] = n
        else:
            out.append([n, n])
    return [(first, last) for first, last in out]


def changed(base, cwd=None):
    """{path: [(first, last), ...]} for the C files changed since base."""
    diff = subprocess.run(['git', 'diff', '--find-renames', '--unified=0', '--no-color', '--no-ext-diff', base, '--',
                           'src', 'tests'], cwd=cwd, check=True, capture_output=True, text=True).stdout
    return {path: runs(lines) for path, lines in added_lines(diff).items() if path.endswith(C_SUFFIXES)}


def main(argv, cwd=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base', required=True, help='the commit to compare the working tree with')
    parser.add_argument('--binary', default='clang-format')
    args = parser.parse_args(argv)

    bad = []
    for path, lines in sorted(changed(args.base, cwd).items()):
        check = subprocess.run([args.binary, '--dry-run', '--Werror', *(f'--lines={a}:{b}' for a, b in lines), path],
                               cwd=cwd, capture_output=True, text=True)
        if check.returncode:
            sys.stdout.write(check.stderr)
            bad.append(path)
    if bad:
        print(f'{len(bad)} file(s) need formatting on the lines they change: {", ".join(bad)}')
        return 1
    print('clean')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
