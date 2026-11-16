#!/usr/bin/env python3
"""Decide when blockclonia releases and prepare the release commit: the
calendar version (YY.M.PATCH) in CMakeLists.txt and the CHANGELOG.md entry.
Tagging and publishing live in release.sh."""
import argparse
import datetime
from pathlib import Path
import re
import subprocess

# Changelog sections for Conventional Commits types, in order. Other types
# go under Maintenance; release bookkeeping is left out.
SECTIONS = {'feat': 'Added', 'fix': 'Fixed', 'perf': 'Performance'}
BOOKKEEPING = {('chore', 'release')}
HEADER = re.compile(r'(?P<type>\w+)(?:\((?P<scope>[^)]*)\))?(?P<breaking>!)?: (?P<summary>.+)')
# The version in `project(blockclonia VERSION x.y.z ...)`.
PROJECT_VERSION = re.compile(r'(project\(blockclonia\s+VERSION\s+)(\d+\.\d+\.\d+)')
# Releases are weekly. A push releases at once only for a security fix, or
# when the weekly run was missed (lost a race to a merge, or CI was red).
SCHEDULED = {'schedule', 'workflow_dispatch'}
CATCH_UP = datetime.timedelta(days=8)
FIRST_NOTES = '- The first versioned release. Earlier changes are in the Git history.\n'


def parse_header(message):
    return HEADER.fullmatch(message.strip().split('\n', 1)[0].strip())


def security_fix(parsed):
    return parsed is not None and (parsed['type'], parsed['scope']) == ('fix', 'security')


def due(messages, event, last_release, today):
    """Whether a CI run for `event` releases the commits since `last_release`
    (None before the first release, which only a scheduled or manual run makes)."""
    if last_release is None:
        return event in SCHEDULED
    changes = [parsed for parsed in map(parse_header, messages)
               if not (parsed and (parsed['type'], parsed['scope']) in BOOKKEEPING)]
    if not changes:
        return False
    if event in SCHEDULED:
        return True
    if event != 'push':
        return False
    return any(map(security_fix, changes)) or today - last_release >= CATCH_UP


def next_version(current, today):
    year, month, patch = map(int, current.split('.'))
    period = (today.year % 100, today.month)
    if period < (year, month):
        raise ValueError('refusing to release a version older than the current one')
    return f'{period[0]}.{period[1]}.{patch + 1 if period == (year, month) else 0}'


def sentence(text, scope=None):
    text = text.strip().rstrip('.')
    text = text[:1].upper() + text[1:]
    return f'{text} ({scope}).' if scope else f'{text}.'


def notes(messages):
    """Changelog notes for the commit messages since the last release."""
    sections = {'Breaking changes': [], 'Security': [], **{name: [] for name in SECTIONS.values()}, 'Maintenance': []}
    for message in messages:
        subject = message.strip().split('\n', 1)[0].strip()
        header = HEADER.fullmatch(subject)
        if not header:
            sections['Maintenance'].append(sentence(subject))
            continue
        kind, scope = header['type'], header['scope']
        if (kind, scope) in BOOKKEEPING:
            continue
        entry = sentence(header['summary'], scope)
        # A footer runs until the next footer token or a blank line.
        footer = re.search(r'(?ms)^BREAKING[ -]CHANGE: (.+?)(?=\n\n|\n[\w-]+: |\Z)', message)
        if header['breaking'] or footer:
            explanation = sentence(' '.join(footer[1].split())) if footer else ''
            sections['Breaking changes'].append(f'{entry} {explanation}'.rstrip())
        elif security_fix(header):
            sections['Security'].append(sentence(header['summary']))
        else:
            sections[SECTIONS.get(kind, 'Maintenance')].append(entry)
    text = '\n'.join(f'### {name}\n\n' + ''.join(f'- {entry}\n' for entry in entries)
                     for name, entries in sections.items() if entries)
    return text or '- No changes besides release bookkeeping.\n'


def git(root, *args):
    return subprocess.run(['git', *args], cwd=root, check=True, capture_output=True, text=True).stdout


def last_release_tag(root):
    described = subprocess.run(['git', 'describe', '--tags', '--abbrev=0', '--match', 'v[0-9]*'],
                               cwd=root, capture_output=True, text=True)
    return described.stdout.strip() if described.returncode == 0 else None


def last_release_date(root):
    """The day the newest release tag's commit was made, or None before the first."""
    tag = last_release_tag(root)
    if tag is None:
        return None
    return datetime.date.fromisoformat(git(root, 'log', '-1', '--format=%cs', tag).strip())


def commits_since_last_release(root):
    """Messages of the commits after the newest release tag, oldest first;
    None before the first release. Merge commits are left out: pull requests
    merge with a merge commit, and their own commits carry the changes."""
    tag = last_release_tag(root)
    if tag is None:
        return None
    log = git(root, 'log', '--reverse', '--no-merges', '--format=%B%x00', f'{tag}..HEAD')
    return [message for message in log.split('\0') if message.strip()]


def current_version(root):
    match = PROJECT_VERSION.search((root / 'CMakeLists.txt').read_text())
    if not match:
        raise ValueError('project(blockclonia VERSION ...) was not found in CMakeLists.txt')
    return match[2]


def prepare(root, today, messages):
    """Write the next version into CMakeLists.txt and its notes at the top of
    CHANGELOG.md; `messages` is None for the first release. Returns the version."""
    cmake = (root / 'CMakeLists.txt').read_text()
    old = current_version(root)
    new = next_version(old, today)
    cmake, count = PROJECT_VERSION.subn(lambda m: m[1] + new, cmake, count=1)
    if count != 1:
        raise ValueError('project version was not found')
    # The new entry goes above the newest one, below the heading and intro.
    preamble, older, history = (root / 'CHANGELOG.md').read_text().partition('\n## ')
    body = FIRST_NOTES if messages is None else notes(messages)
    entry = f'## {new} — {today.isoformat()}\n\n{body}'
    (root / 'CMakeLists.txt').write_text(cmake)
    (root / 'CHANGELOG.md').write_text(preamble.rstrip('\n') + '\n\n' + entry + ('\n## ' + history if older else ''))
    return new


def release_notes(changelog, version):
    """The CHANGELOG.md entry of `version`, for the GitHub release."""
    for entry in changelog.split('\n## ')[1:]:
        if entry.startswith(f'{version} '):
            return '## ' + entry.rstrip('\n') + '\n'
    raise ValueError(f'CHANGELOG.md has no entry for {version}')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group()
    group.add_argument('--due', metavar='EVENT', help='exit 0 if a run for EVENT releases now')
    group.add_argument('--version', action='store_true', help='print the current version')
    group.add_argument('--notes', metavar='VERSION', help='print the changelog entry of VERSION')
    args = parser.parse_args(argv)
    root = Path(__file__).resolve().parents[1]
    today = datetime.datetime.now(datetime.timezone.utc).date()
    if args.due is not None:
        return 0 if due(commits_since_last_release(root), args.due, last_release_date(root), today) else 1
    if args.version:
        print(current_version(root))
    elif args.notes:
        print(release_notes((root / 'CHANGELOG.md').read_text(), args.notes), end='')
    else:
        print(prepare(root, today, commits_since_last_release(root)))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
