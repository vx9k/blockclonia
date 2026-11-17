"""The weekly release: versions, when a release is due, the changelog, and
release.sh run against a throwaway repository and a stand-in for gh."""
import datetime
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('release', TOOLS / 'release.py')
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)

CMAKE = 'cmake_minimum_required(VERSION 3.21)\nproject(blockclonia\n  VERSION {}\n  LANGUAGES C)\n'
DAY = datetime.date(2026, 11, 16)


class Version(unittest.TestCase):
    def test_same_month_increments_the_patch(self):
        self.assertEqual(release.next_version('26.11.3', DAY), '26.11.4')

    def test_a_new_month_or_year_starts_at_zero(self):
        self.assertEqual(release.next_version('26.10.4', DAY), '26.11.0')
        self.assertEqual(release.next_version('26.12.9', datetime.date(2027, 1, 4)), '27.1.0')

    def test_the_pre_calendar_version_moves_to_the_calendar(self):
        self.assertEqual(release.next_version('0.1.0', DAY), '26.11.0')

    def test_a_version_from_the_future_is_refused(self):
        with self.assertRaises(ValueError):
            release.next_version('27.1.0', DAY)


class Due(unittest.TestCase):
    week_ago = DAY - datetime.timedelta(days=7)

    def test_the_first_release_waits_for_a_scheduled_or_manual_run(self):
        self.assertTrue(release.due(None, 'schedule', None, DAY))
        self.assertTrue(release.due(None, 'workflow_dispatch', None, DAY))
        self.assertFalse(release.due(None, 'push', None, DAY))

    def test_the_weekly_run_releases_any_change(self):
        self.assertTrue(release.due(['docs: fix a typo'], 'schedule', self.week_ago, DAY))

    def test_nothing_but_bookkeeping_is_not_released(self):
        self.assertFalse(release.due([], 'schedule', self.week_ago, DAY))
        self.assertFalse(release.due(['chore(release): 26.11.0 after green CI'], 'schedule', self.week_ago, DAY))

    def test_a_push_waits_for_the_weekly_run(self):
        self.assertFalse(release.due(['feat: add boars'], 'push', self.week_ago, DAY))

    def test_a_security_fix_releases_at_once(self):
        self.assertTrue(release.due(['fix(security): bound packet lengths'], 'push', self.week_ago, DAY))
        self.assertFalse(release.due(['fix(net): bound packet lengths'], 'push', self.week_ago, DAY))

    def test_a_push_catches_up_a_missed_week(self):
        missed = DAY - datetime.timedelta(days=8)
        self.assertTrue(release.due(['feat: add boars'], 'push', missed, DAY))

    def test_pull_requests_never_release(self):
        long_ago = DAY - datetime.timedelta(days=30)
        self.assertFalse(release.due(['fix(security): x'], 'pull_request', long_ago, DAY))


class Notes(unittest.TestCase):
    def test_commits_are_grouped_by_type(self):
        commits = [
            'feat(worldgen): add caves and ores\n\nWhy it matters.\n',
            'fix: keep water from duplicating',
            'perf(mesher): skip empty sections',
            'docs(readme): describe the furnace',
            'chore(release): 26.11.0 after green CI\n\n[skip ci]\n\nBlockclonia-Release-For: abc\n',
            'feat(inventory)!: store item wear\n\nBody.\n\nBREAKING CHANGE: the inventory tag changes\nfrom INV1 to INV2.\nCo-Authored-By: Someone <a@b>\n',
            'Fix something without a type',
        ]
        self.assertEqual(release.notes(commits), '''### Breaking changes

- Store item wear (inventory). The inventory tag changes from INV1 to INV2.

### Added

- Add caves and ores (worldgen).

### Fixed

- Keep water from duplicating.

### Performance

- Skip empty sections (mesher).

### Maintenance

- Describe the furnace (readme).
- Fix something without a type.
''')

    def test_security_fixes_have_their_own_section(self):
        self.assertEqual(release.notes(['fix(security): bound packet lengths']),
                         '### Security\n\n- Bound packet lengths.\n')

    def test_notes_without_changes_say_so(self):
        self.assertEqual(release.notes(['chore(release): 26.11.0 after green CI']),
                         '- No changes besides release bookkeeping.\n')

    def test_the_release_notes_are_one_changelog_entry(self):
        changelog = '# Changelog\n\nIntro.\n\n## 26.11.1 — 2026-11-23\n\n- B.\n\n## 26.11.0 — 2026-11-16\n\n- A.\n'
        self.assertEqual(release.release_notes(changelog, '26.11.0'), '## 26.11.0 — 2026-11-16\n\n- A.\n')
        self.assertEqual(release.release_notes(changelog, '26.11.1'), '## 26.11.1 — 2026-11-23\n\n- B.\n')
        with self.assertRaises(ValueError):
            release.release_notes(changelog, '26.11.2')


class Prepare(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.root)

    def write(self, version, changelog):
        (self.root / 'CMakeLists.txt').write_text(CMAKE.format(version))
        (self.root / 'CHANGELOG.md').write_text(changelog)

    def test_the_first_release_points_at_the_history(self):
        self.write('0.1.0', '# Changelog\n\nIntro.\n')
        self.assertEqual(release.prepare(self.root, DAY, None), '26.11.0')
        self.assertIn('  VERSION 26.11.0\n', (self.root / 'CMakeLists.txt').read_text())
        self.assertEqual((self.root / 'CHANGELOG.md').read_text(),
                         '# Changelog\n\nIntro.\n\n## 26.11.0 — 2026-11-16\n\n' + release.FIRST_NOTES)

    def test_a_new_entry_goes_above_the_older_ones_below_the_intro(self):
        self.write('26.11.0', '# Changelog\n\nIntro.\n\n## 26.11.0 — 2026-11-09\n\n- A.\n')
        self.assertEqual(release.prepare(self.root, DAY, ['feat: add boars']), '26.11.1')
        self.assertEqual((self.root / 'CHANGELOG.md').read_text(),
                         '# Changelog\n\nIntro.\n\n## 26.11.1 — 2026-11-16\n\n### Added\n\n- Add boars.\n'
                         '\n## 26.11.0 — 2026-11-09\n\n- A.\n')

    def test_only_the_project_version_changes(self):
        self.write('26.11.0', '# Changelog\n')
        cmake = CMAKE.format('26.11.0') + 'cmake_policy(VERSION 3.21)\n'
        (self.root / 'CMakeLists.txt').write_text(cmake)
        release.prepare(self.root, DAY, [])
        self.assertEqual((self.root / 'CMakeLists.txt').read_text(),
                         cmake.replace('VERSION 26.11.0', 'VERSION 26.11.1'))

    def test_a_missing_project_version_stops_before_writing(self):
        (self.root / 'CMakeLists.txt').write_text('project(other VERSION 1.0.0)\n')
        (self.root / 'CHANGELOG.md').write_text('# Changelog\n')
        with self.assertRaises(ValueError):
            release.prepare(self.root, DAY, [])
        self.assertEqual((self.root / 'CHANGELOG.md').read_text(), '# Changelog\n')

    def test_the_real_project_has_a_version_and_a_changelog(self):
        root = TOOLS.parent
        self.assertRegex(release.current_version(root), r'^\d+\.\d+\.\d+$')
        self.assertTrue((root / 'CHANGELOG.md').read_text().startswith('# Changelog\n'))


def git(cwd, *args, env=None):
    return subprocess.run(['git', *args], cwd=cwd, check=True, capture_output=True, text=True, env=env).stdout


class Script(unittest.TestCase):
    """release.sh in a clone of a throwaway bare repository standing in for
    GitHub, with the user's own Git configuration (signing) shut out."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp)
        self.env = {**os.environ, 'GIT_CONFIG_GLOBAL': os.devnull, 'GIT_CONFIG_NOSYSTEM': '1',
                    'GIT_AUTHOR_NAME': 't', 'GIT_AUTHOR_EMAIL': 't@t', 'GIT_COMMITTER_NAME': 't',
                    'GIT_COMMITTER_EMAIL': 't@t', 'RUNNER_TEMP': str(self.tmp)}
        self.env.pop('GITHUB_OUTPUT', None)
        origin, self.work = self.tmp / 'origin.git', self.tmp / 'work'
        git(self.tmp, 'init', '-q', '--bare', '-b', 'main', str(origin), env=self.env)
        git(self.tmp, 'init', '-q', '-b', 'main', str(self.work), env=self.env)
        (self.work / 'tools').mkdir()
        for name in ['release.py', 'release.sh']:
            shutil.copy(TOOLS / name, self.work / 'tools' / name)
        (self.work / 'CMakeLists.txt').write_text(CMAKE.format('26.11.0'))
        (self.work / 'CHANGELOG.md').write_text('# Changelog\n')
        self.commit('feat: start')
        git(self.work, 'tag', 'v26.11.0', env=self.env)
        git(self.work, 'remote', 'add', 'origin', str(origin), env=self.env)
        git(self.work, 'push', '-q', 'origin', 'main', '--tags', env=self.env)

    def commit(self, message):
        git(self.work, 'add', '-A', env=self.env)
        git(self.work, 'commit', '-q', '--allow-empty', '-m', message, env=self.env)
        return git(self.work, 'rev-parse', 'HEAD', env=self.env).strip()

    def prepare(self, sha, event='schedule'):
        out = self.tmp / 'output'
        out.write_text('')
        env = {**self.env, 'GITHUB_SHA': sha, 'GITHUB_EVENT_NAME': event, 'GITHUB_OUTPUT': str(out)}
        run = subprocess.run(['bash', 'tools/release.sh', 'prepare'], cwd=self.work, env=env,
                             capture_output=True, text=True)
        self.assertEqual(run.returncode, 0, run.stderr)
        return out.read_text()

    def origin_log(self):
        return git(self.work, 'log', '--format=%s', 'origin/main', env=self.env).splitlines()

    def test_a_due_release_is_committed_tagged_and_pushed(self):
        sha = self.commit('feat: add boars')
        git(self.work, 'push', '-q', 'origin', 'main', env=self.env)
        self.assertEqual(self.prepare(sha), 'version=26.11.1\n')
        git(self.work, 'fetch', '-q', 'origin', '--tags', env=self.env)
        self.assertEqual(self.origin_log()[0], 'chore(release): 26.11.1 after green CI')
        body = git(self.work, 'log', '-1', '--format=%B', 'origin/main', env=self.env)
        self.assertIn('[skip ci]', body)
        self.assertIn(f'Blockclonia-Release-For: {sha}', body)
        self.assertEqual(git(self.work, 'rev-parse', 'v26.11.1^{commit}', env=self.env),
                         git(self.work, 'rev-parse', 'origin/main', env=self.env))

    def test_nothing_due_writes_an_empty_version(self):
        sha = self.commit('feat: add boars')
        git(self.work, 'push', '-q', 'origin', 'main', env=self.env)
        self.assertEqual(self.prepare(sha, event='push'), 'version=\n')
        self.assertEqual(self.origin_log()[0], 'feat: add boars')

    def test_a_run_for_an_older_commit_skips(self):
        old = self.commit('feat: add boars')
        self.commit('feat: add wolves')
        git(self.work, 'push', '-q', 'origin', 'main', env=self.env)
        git(self.work, 'checkout', '-q', '--detach', old, env=self.env)
        self.assertEqual(self.prepare(old), 'version=\n')
        self.assertEqual(self.origin_log()[0], 'feat: add wolves')

    def test_a_rerun_resumes_the_release_without_another_bump(self):
        sha = self.commit('feat: add boars')
        git(self.work, 'push', '-q', 'origin', 'main', env=self.env)
        self.assertEqual(self.prepare(sha), 'version=26.11.1\n')
        git(self.work, 'checkout', '-q', '--detach', sha, env=self.env)
        self.assertEqual(self.prepare(sha), 'version=26.11.1\n')
        self.assertEqual(self.origin_log().count('chore(release): 26.11.1 after green CI'), 1)


class Publish(unittest.TestCase):
    """release.sh publish with a stand-in gh that logs its calls."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp)
        self.work = self.tmp / 'work'
        (self.work / 'tools').mkdir(parents=True)
        (self.work / 'dist').mkdir()
        for name in ['release.py', 'release.sh']:
            shutil.copy(TOOLS / name, self.work / 'tools' / name)
        (self.work / 'CMakeLists.txt').write_text(CMAKE.format('26.11.1'))
        (self.work / 'CHANGELOG.md').write_text('# Changelog\n\n## 26.11.1 — 2026-11-16\n\n- Notes.\n')
        bin_dir = self.tmp / 'bin'
        bin_dir.mkdir()
        self.calls = self.tmp / 'calls'
        # Not RUNNER_TEMP/releases.json, which release.sh itself writes.
        self.releases = self.tmp / 'gh-releases.json'
        self.releases.write_text('[]')
        gh = bin_dir / 'gh'
        gh.write_text(f'#!/bin/sh\necho "$*" >> {self.calls}\n'
                      f'[ "$1" = api ] && cat {self.releases}\nexit 0\n')
        gh.chmod(0o755)
        self.env = {**os.environ, 'PATH': f'{bin_dir}:{os.environ["PATH"]}', 'VERSION': '26.11.1',
                    'GH_REPO': 'o/r', 'GH_TOKEN': 'x', 'RUNNER_TEMP': str(self.tmp)}

    def debs(self, *arches):
        for arch in arches:
            (self.work / 'dist' / f'blockclonia_26.11.1_{arch}.deb').write_text('')

    def tarballs(self, *arches):
        for arch in arches:
            (self.work / 'dist' / f'blockclonia-26.11.1-alpine-{arch}.tar.gz').write_text('')

    def every_package(self):
        self.debs('amd64', 'arm64')
        self.tarballs('x86_64', 'aarch64')

    def publish(self):
        return subprocess.run(['bash', 'tools/release.sh', 'publish'], cwd=self.work, env=self.env,
                              capture_output=True, text=True)

    def gh_calls(self):
        return self.calls.read_text().splitlines() if self.calls.exists() else []

    def test_a_new_release_is_drafted_filled_and_then_published(self):
        self.every_package()
        run = self.publish()
        self.assertEqual(run.returncode, 0, run.stderr)
        calls = self.gh_calls()
        self.assertTrue(calls[1].startswith('release create v26.11.1 --draft --verify-tag'))
        for name in ['blockclonia_26.11.1_amd64.deb', 'blockclonia_26.11.1_arm64.deb',
                     'blockclonia-26.11.1-alpine-x86_64.tar.gz', 'blockclonia-26.11.1-alpine-aarch64.tar.gz']:
            self.assertIn(name, calls[2])
        self.assertEqual(calls[3], 'release edit v26.11.1 --draft=false --latest')
        self.assertEqual((self.tmp / 'notes.md').read_text(), '## 26.11.1 — 2026-11-16\n\n- Notes.\n')

    def test_a_missing_package_stops_before_github_is_touched(self):
        self.debs('arm64')
        self.tarballs('x86_64', 'aarch64')
        self.assertNotEqual(self.publish().returncode, 0)
        self.assertEqual(self.gh_calls(), [])

    def test_the_debs_alone_are_not_a_release(self):
        # Alpine is a supported platform: a release without its tarballs
        # would leave it out.
        self.debs('amd64', 'arm64')
        self.tarballs('x86_64')
        run = self.publish()
        self.assertNotEqual(run.returncode, 0)
        self.assertIn('Alpine tarballs', run.stderr)
        self.assertEqual(self.gh_calls(), [])

    def test_another_versions_packages_do_not_count(self):
        self.debs('amd64', 'arm64')
        self.tarballs('x86_64')
        (self.work / 'dist' / 'blockclonia-26.11.0-alpine-aarch64.tar.gz').write_text('')
        self.assertNotEqual(self.publish().returncode, 0)
        self.assertEqual(self.gh_calls(), [])

    def test_a_rerun_fills_an_existing_draft_without_creating_another(self):
        self.every_package()
        self.releases.write_text('[{"tag_name": "v26.11.1", "draft": true}]')
        self.assertEqual(self.publish().returncode, 0)
        self.assertFalse(any(call.startswith('release create') for call in self.gh_calls()))
        self.assertIn('release edit v26.11.1 --draft=false --latest', self.gh_calls())

    def test_a_published_release_is_left_alone(self):
        self.every_package()
        self.releases.write_text('[{"tag_name": "v26.11.1", "draft": false}]')
        self.assertEqual(self.publish().returncode, 0)
        self.assertEqual(len(self.gh_calls()), 1)


if __name__ == '__main__':
    unittest.main()
