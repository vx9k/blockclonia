#!/usr/bin/env python3
"""Checks a pull request: a Conventional Commits title in the form AGENTS.md
asks for (it becomes the merge's subject on main and the changelog line), and
at least one added line of tests for every added line of code."""
import argparse
from pathlib import PurePosixPath
import re
import subprocess
import sys

# The types AGENTS.md (Git, Commit messages) allows.
TYPES = 'feat|fix|perf|refactor|test|docs|build|ci|style|chore|revert'
# The description starts lower case and has no final full stop.
TITLE = re.compile(rf'({TYPES})(\([a-z0-9._/-]+\))?!?: (?![A-Z\s])(?!.*[.\s]$).+')
TITLE_WIDTH = 72
RATIO = 1
HUNK = re.compile(r'@@ -\S+ \+(\d+)(?:,(\d+))? @@')
# The modules AGENTS.md keeps out of the headless build: only a window, a GPU
# or an audio device runs them, so no unit test can reach their lines, and
# they count for neither side. The shaders run only on the GPU.
UNTESTABLE = {'src/game/main.c', 'src/render/renderer.c', 'src/audio/audio_device.c'}
C_SUFFIXES = ('.c', '.h')


def title_problem(title):
    if len(title) > TITLE_WIDTH:
        return f'title is {len(title)} characters long; at most {TITLE_WIDTH}'
    if TITLE.fullmatch(title):
        return None
    return f'title {title!r} is not a Conventional Commit: type(scope): description, ' \
        'with a type AGENTS.md allows, a lower-case description and no final full stop'


def kind(path):
    """'test', 'code', or None for a file that counts for neither side."""
    parts = PurePosixPath(path).parts
    if path in UNTESTABLE or parts[0] == 'shaders':
        return None
    if path.endswith('.py'):
        return 'test' if 'tests' in parts else 'code'
    if not path.endswith(C_SUFFIXES):
        return None
    if parts[0] == 'tests':
        return 'test'
    if parts[0] == 'src':
        return 'code'
    return None


def comment_or_blank(line, python):
    stripped = line.strip()
    if not stripped:
        return True
    if python:
        return stripped.startswith('#')
    # Line comments and the lines of a block comment.
    return stripped.startswith(('//', '/*', '*'))


def added_lines(diff):
    """The new line numbers added to each file by a `git diff --unified=0`."""
    added, path = {}, None
    for line in diff.splitlines():
        if line.startswith('+++ '):
            path = line[6:] if line.startswith('+++ b/') else None
        elif (hunk := HUNK.match(line)) and path:
            start, length = int(hunk[1]), int(hunk[2] or 1)
            if length:
                added.setdefault(path, []).extend(range(start, start + length))
    return added


# Whitespace matters in C only between two parts of a word (`int a` is not
# `inta`); everywhere else, layout is free.
SPACE = re.compile(r'\s+')
LOOSE_SPACE = re.compile(r'(?<!\w) | (?!\w)')


def squeeze(text):
    """C text without its layout: what clang-format may change."""
    return LOOSE_SPACE.sub('', SPACE.sub(' ', text))


def new_lines(diff):
    """Like added_lines, without the lines of C files that only change layout:
    a hunk that only reformats (reindents, joins or splits lines), and lines
    that only move within their file (includes put in order). Python's
    indentation is its syntax, so Python lines always count."""
    hunks, path, current = [], None, None
    for line in diff.splitlines():
        if line.startswith('+++ '):
            path = line[6:] if line.startswith('+++ b/') else None
            current = None
        elif line.startswith('--- '):
            continue
        elif (hunk := HUNK.match(line)) and path:
            current = (path, int(hunk[1]), [], [])
            hunks.append(current)
        elif current and line[:1] in '+-':
            current[2 if line[0] == '-' else 3].append(line[1:])

    def reformatted(path, old, new):
        return path.endswith(C_SUFFIXES) and old and squeeze('\n'.join(old)) == squeeze('\n'.join(new))

    hunks = [hunk for hunk in hunks if not reformatted(hunk[0], hunk[2], hunk[3])]
    removed = {}
    for path, _, old, _ in hunks:
        removed.setdefault(path, []).extend(squeeze(text) for text in old)
    added = {}
    for path, start, _, new in hunks:
        layout = path.endswith(C_SUFFIXES)
        for number, text in enumerate(new, start):
            if layout and squeeze(text) in removed[path]:
                removed[path].remove(squeeze(text))
                continue
            added.setdefault(path, []).append(number)
    return added


def count(added, read):
    """(test lines, code lines, untestable lines) among the added lines,
    skipping blank lines and comments; `read(path)` gives a file's new text."""
    tests = code = untestable = 0
    for path, numbers in added.items():
        side = kind(path)
        counted = side is not None or path in UNTESTABLE
        if not counted:
            continue
        lines = read(path).splitlines()
        python = path.endswith('.py')
        for number in numbers:
            if number > len(lines) or comment_or_blank(lines[number - 1], python):
                continue
            if side == 'test':
                tests += 1
            elif side == 'code':
                code += 1
            else:
                untestable += 1
    return tests, code, untestable


def ratio_problem(tests, code):
    if tests >= RATIO * code:
        return None
    return f'{tests} test lines for {code} lines of code; the rule is {RATIO}:1, ' \
        f'so add {RATIO * code - tests} more lines of tests'


def main(argv, cwd=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base', required=True, help='the commit the pull request is based on')
    parser.add_argument('--title', required=True)
    args = parser.parse_args(argv)

    def git(*a):
        return subprocess.run(['git', *a], cwd=cwd, check=True, capture_output=True, text=True).stdout

    # A moved file adds no lines, whatever the user's diff.renames says.
    diff = git('diff', '--find-renames', '--unified=0', '--no-color', f'{args.base}...HEAD')
    root = PurePosixPath(git('rev-parse', '--show-toplevel').strip())

    def read(path):
        with open(root / path, encoding='utf-8', errors='replace') as file:
            return file.read()

    tests, code, untestable = count(new_lines(diff), read)
    exempt = f', {untestable} in modules only a window, GPU or audio device runs' if untestable else ''
    print(f'{tests} test lines, {code} code lines{exempt}')
    problems = [p for p in (title_problem(args.title), ratio_problem(tests, code)) if p]
    for problem in problems:
        print(f'error: {problem}', file=sys.stderr)
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
