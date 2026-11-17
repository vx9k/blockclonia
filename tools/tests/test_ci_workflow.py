"""Pin the parts of the workflows that other jobs, the ruleset and people
rely on."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
WORKFLOWS = ROOT / '.github/workflows'
CI = (WORKFLOWS / 'ci.yml').read_text()
PACKAGE = (WORKFLOWS / 'package.yml').read_text()
PR = (WORKFLOWS / 'pr.yml').read_text()
# Jobs that run but may fail without blocking a merge: none, since every
# supported platform (Ubuntu, Alpine, macOS) is required.
EXPERIMENTAL = set()
# Jobs that run after the gate.
AFTER_GATE = {'ci-passed', 'nightly', 'release', 'release-package', 'release-publish'}


def jobs(workflow):
    return re.findall(r'(?m)^  ([a-z0-9-]+):$', workflow.split('\njobs:\n', 1)[1])


def job(workflow, name):
    """The lines of the job called `name`, up to the next job."""
    match = workflow.split(f'\n  {name}:\n', 1)
    if len(match) != 2:
        raise AssertionError(f'no job {name!r}')
    body = []
    for line in match[1].splitlines(keepends=True):
        if line.strip() and len(line) - len(line.lstrip()) <= 2:
            break
        body.append(line)
    return ''.join(body)


def needs(workflow, name):
    match = re.search(r'(?m)^    needs: \[([^]]*)\]$', job(workflow, name))
    if not match:
        raise AssertionError(f'{name} has no needs')
    return {need.strip() for need in match[1].split(',')}


def trigger(name):
    triggers = CI.split('\non:\n', 1)[1].split('\n\n', 1)[0]
    match = re.search(rf'(?m)^  {name}:\n((?:    .*\n)*)', triggers + '\n')
    if not match:
        raise AssertionError(f'no trigger {name!r}')
    return match[1]


class Lookups(unittest.TestCase):
    def test_unknown_jobs_and_triggers_are_errors(self):
        with self.assertRaises(AssertionError):
            job(CI, 'no-such-job')
        with self.assertRaises(AssertionError):
            trigger('release')

    def test_a_job_ends_at_the_next_one(self):
        self.assertNotIn('nightly:', job(CI, 'ci-passed'))
        self.assertIn('tools/release.sh prepare', job(CI, 'release'))


class CiPassed(unittest.TestCase):
    """The ruleset requires `CI passed`, which stands for every other job."""

    def test_it_waits_for_every_job_but_the_experimental_ones(self):
        # A new job left out here would not be required by the ruleset.
        self.assertEqual(needs(CI, 'ci-passed'), set(jobs(CI)) - AFTER_GATE - EXPERIMENTAL)

    def test_experimental_jobs_cannot_fail_the_run(self):
        for name in EXPERIMENTAL:
            self.assertIn('    continue-on-error: true\n', job(CI, name))

    def test_every_supported_platform_is_required(self):
        self.assertTrue({'package', 'macos'} <= needs(CI, 'ci-passed'))
        self.assertNotIn('\n    continue-on-error: true\n', job(CI, 'macos'))
        # Only the MoltenVK render on a GPU-less VM may fail.
        self.assertEqual(job(CI, 'macos').count('continue-on-error: true'), 1)
        self.assertLess(job(CI, 'macos').index('ctest'), job(CI, 'macos').index('continue-on-error: true'))

    def test_it_is_the_name_the_ruleset_requires(self):
        self.assertRegex(job(CI, 'ci-passed'), r'(?m)^    name: CI passed$')

    def test_it_runs_when_a_job_fails_and_then_fails_itself(self):
        # Without always() a failed job would skip it, and a skipped required
        # check does not block a merge.
        body = job(CI, 'ci-passed')
        self.assertIn('    if: always()\n', body)
        self.assertIn("${{ join(needs.*.result, ' ') }}", body)
        self.assertIn('[ "$result" = success ] || exit 1', body)

    def test_nightly_and_release_wait_for_it(self):
        self.assertEqual(needs(CI, 'nightly'), {'ci-passed'})
        self.assertEqual(needs(CI, 'release'), {'ci-passed'})


class Triggers(unittest.TestCase):
    def test_pushes_run_only_on_main_so_pull_requests_run_once(self):
        self.assertEqual(trigger('push').strip(), 'branches: [main]')
        self.assertIsNotNone(trigger('pull_request'))

    def test_ci_runs_every_monday_morning_and_by_hand(self):
        self.assertEqual(trigger('schedule').strip(), '- cron: "0 6 * * 1"')
        self.assertIsNotNone(trigger('workflow_dispatch'))

    def test_pull_requests_cancel_their_older_run_but_main_never_does(self):
        # Cancelling a run on main could stop a release halfway.
        self.assertIn("group: ci-${{ github.event_name }}-${{ github.event_name == 'pull_request' && github.ref "
                      "|| github.sha }}\n", CI)
        self.assertIn("cancel-in-progress: ${{ github.event_name == 'pull_request' }}", CI)
        self.assertNotIn('cancel-in-progress: true', CI)


class Security(unittest.TestCase):
    def test_the_default_token_is_read_only(self):
        for workflow in [CI, PACKAGE, PR]:
            top = workflow.split('\njobs:', 1)[0]
            self.assertIn('permissions:\n  contents: read\n', top)

    def test_only_the_publishing_jobs_may_write_or_sign(self):
        writers = {name for name in jobs(CI) if 'contents: write' in job(CI, name)}
        self.assertEqual(writers, {'nightly', 'release', 'release-publish'})
        self.assertEqual(CI.count('id-token: write'), 1)
        self.assertIn('id-token: write', job(CI, 'release-publish'))

    def test_checkouts_keep_the_token_out_of_git_unless_they_push(self):
        # Only the release job pushes, with the deploy key.
        for workflow in [CI, PACKAGE, PR]:
            for name in jobs(workflow):
                body = job(workflow, name)
                if 'actions/checkout@' not in body:
                    continue
                if name == 'release':
                    self.assertIn('ssh-key: ${{ secrets.RELEASE_DEPLOY_KEY }}', body)
                else:
                    self.assertIn('persist-credentials: false', body, name)

    def test_the_title_reaches_the_check_through_the_environment(self):
        self.assertIn('TITLE: ${{ github.event.pull_request.title }}', PR)
        self.assertIn('--title "$TITLE"', PR)
        self.assertNotIn('run: python3 tools/pr_check.py --base HEAD^1 --title "${{', PR)

    def test_every_job_has_a_timeout(self):
        for workflow in [CI, PACKAGE, PR]:
            for name in jobs(workflow):
                body = job(workflow, name)
                if '    uses: ./' in body:
                    continue  # a reusable workflow's own jobs carry theirs
                self.assertRegex(body, r'(?m)^    timeout-minutes: \d+$', name)


class Release(unittest.TestCase):
    def test_only_main_releases_and_never_from_a_pull_request(self):
        self.assertIn("if: github.ref == 'refs/heads/main' && github.event_name != 'pull_request'\n",
                      job(CI, 'release'))

    def test_the_packages_are_built_from_the_tag_and_kept_apart_from_the_nightly(self):
        package = job(CI, 'release-package')
        self.assertIn("if: needs.release.outputs.version != ''", package)
        self.assertIn('release: ${{ needs.release.outputs.version }}', package)
        self.assertIn('artifact-prefix: release', package)
        self.assertIn('pattern: blockclonia-*', job(CI, 'nightly'))
        self.assertIn('pattern: release-*', job(CI, 'release-publish'))

    def test_the_packages_are_attested_before_publication(self):
        publish = job(CI, 'release-publish')
        self.assertIn('uses: actions/attest-build-provenance@v4', publish)
        self.assertLess(publish.index('attest-build-provenance'), publish.index('tools/release.sh publish'))

    def test_the_release_job_hands_its_version_on(self):
        release = job(CI, 'release')
        self.assertIn('version: ${{ steps.prepare.outputs.version }}', release)
        self.assertIn('id: prepare', release)
        self.assertIn('cancel-in-progress: false', release)

    def test_release_packages_keep_the_plain_version(self):
        # The suffix goes only on packages built from a commit, not a tag.
        self.assertEqual(PACKAGE.count('if [ -z "$RELEASE" ]; then'), 2)
        self.assertEqual(PACKAGE.count('-DMC_PACKAGE_VERSION_SUFFIX="${MC_PACKAGE_VERSION_SUFFIX:-}"'), 1)
        self.assertIn('-e MC_PACKAGE_VERSION_SUFFIX="${MC_PACKAGE_VERSION_SUFFIX:-}"', PACKAGE)
        self.assertIn('-DMC_PACKAGE_VERSION_SUFFIX="$MC_PACKAGE_VERSION_SUFFIX"', PACKAGE)
        cmake = (ROOT / 'cmake/Packaging.cmake').read_text()
        self.assertIn('set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}${MC_PACKAGE_VERSION_SUFFIX}")', cmake)
        self.assertLess(cmake.index('CPACK_PACKAGE_VERSION'), cmake.index('include(CPack)'))


def matrix_arches(name):
    return re.findall(r'- \{ arch: ([a-z0-9_]+), runner: (ubuntu-26\.04(?:-arm)?),', job(PACKAGE, name))


class Packages(unittest.TestCase):
    def test_ubuntu_and_alpine_are_built_and_uploaded(self):
        self.assertEqual(set(jobs(PACKAGE)), {'ubuntu', 'alpine'})
        for name in ['ubuntu', 'alpine']:
            body = job(PACKAGE, name)
            self.assertIn(f'name: ${{{{ inputs.artifact-prefix }}}}-{name}-${{{{ matrix.arch }}}}', body)
            self.assertIn('if-no-files-found: error', body)
            self.assertIn('fail-fast: false', body)
            self.assertIn("ref: ${{ inputs.release != '' && format('refs/tags/v{0}', inputs.release) || '' }}",
                          body)

    def test_each_is_built_natively_on_x86_64_and_arm64(self):
        self.assertEqual(matrix_arches('ubuntu'), [('amd64', 'ubuntu-26.04'), ('arm64', 'ubuntu-26.04-arm')])
        self.assertEqual(matrix_arches('alpine'), [('x86_64', 'ubuntu-26.04'), ('aarch64', 'ubuntu-26.04-arm')])
        self.assertNotIn('armhf', PACKAGE)
        self.assertNotIn('qemu', PACKAGE)

    def test_packages_are_tested_before_they_are_packed(self):
        ubuntu, alpine = job(PACKAGE, 'ubuntu'), job(PACKAGE, 'alpine')
        self.assertLess(ubuntu.index('ctest --preset ci'), ubuntu.index('cpack -G DEB'))
        self.assertLess(alpine.index('ctest --preset release'), alpine.index('cpack -G TGZ'))
        self.assertIn('-DMC_PGO=ON -DMC_BOLT=ON', ubuntu)
        self.assertIn('-DMC_WERROR=ON', alpine)

    def test_the_release_attests_both_kinds_of_package(self):
        publish = job(CI, 'release-publish')
        self.assertIn('dist/blockclonia_*.deb', publish)
        self.assertIn('dist/blockclonia-*.tar.gz', publish)

    def test_no_job_builds_for_a_dropped_platform(self):
        for workflow in [CI, PACKAGE]:
            for gone in ['old-vulkan', 'bookworm', 'ubuntu-24.04', 'pi-arm64', 'pi4-']:
                self.assertNotIn(gone, workflow)

    def test_ci_builds_them_for_every_run(self):
        self.assertIn('uses: ./.github/workflows/package.yml', job(CI, 'package'))


def step_script(workflow, name, step):
    """The run script of the step called `step` in job `name`, dedented."""
    body = job(workflow, name).split(f'- name: {step}\n', 1)[1]
    script = body.split('run: |\n', 1)[1]
    lines = []
    for line in script.splitlines():
        if line.strip() and not line.startswith(' ' * 10):
            break
        lines.append(line[10:])
    return '\n'.join(lines) + '\n'


def version_script(name):
    return step_script(PACKAGE, name, 'Version the package')


class ContainerShell(unittest.TestCase):
    """Steps in a container job run under `sh -e`, which is dash on Debian."""

    BASHISMS = [r'\$\{\w+:[-0-9]*:', r'\$\{\w+/', r'\[\[', r'\$\{\w+\^', r'(?m)^\s*function ']

    def test_container_jobs_use_no_bash_only_syntax(self):
        for workflow in [CI, PACKAGE]:
            for name in jobs(workflow):
                body = job(workflow, name)
                if '    container:' not in body or 'shell: bash' in body:
                    continue
                for bashism in self.BASHISMS:
                    self.assertNotRegex(body, bashism, f'{name} runs under sh')

    def test_the_bashism_check_catches_a_substring(self):
        self.assertRegex('${GITHUB_SHA::7}', self.BASHISMS[0])

    @unittest.skipUnless(shutil.which('dash'), 'dash is not installed')
    def test_dash_writes_the_suffix_for_a_commit_and_none_for_a_tag(self):
        for name in ['ubuntu', 'alpine']:
            with tempfile.TemporaryDirectory() as tmp:
                out = Path(tmp) / 'env'
                env = {'PATH': os.environ['PATH'], 'GITHUB_ENV': str(out), 'GITHUB_SHA': 'abcdef0123456789'}
                for release, expected in [('', r'^MC_PACKAGE_VERSION_SUFFIX=\+git\.\d{8}\.abcdef0\n$'), ('26.11.0', '^$')]:
                    out.write_text('')
                    run = subprocess.run(['dash', '-e', '-c', version_script(name)], env={**env, 'RELEASE': release},
                                         capture_output=True, text=True)
                    self.assertEqual(run.returncode, 0, run.stderr)
                    self.assertRegex(out.read_text(), expected)


# Stands in for `gh release`: $STATE/releases holds one tag per line, a tag
# listed in $STATE/immutable cannot be deleted, and every call is logged.
FAKE_GH = """#!/bin/bash
echo "$*" >> "$STATE/log"
case "$2" in
  view) grep -qxF "$3" "$STATE/releases" ;;
  create) echo "$3" >> "$STATE/releases" ;;
  list) cat "$STATE/releases" ;;
  delete)
    grep -qxF "$3" "$STATE/immutable" && exit 1
    grep -vxF "$3" "$STATE/releases" > "$STATE/rest"
    mv "$STATE/rest" "$STATE/releases" ;;
esac
"""


class Nightly(unittest.TestCase):
    SHA = 'abcdef0123456789'

    def publish(self, releases, immutable=()):
        """Run the publish step against a fake gh; return (run, releases, log)."""
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            (tmp / 'bin').mkdir()
            (tmp / 'bin/gh').write_text(FAKE_GH)
            (tmp / 'bin/gh').chmod(0o755)
            (tmp / 'dist').mkdir()
            (tmp / 'dist/blockclonia_1_arm64.deb').write_text('')
            (tmp / 'releases').write_text(''.join(f'{r}\n' for r in releases))
            (tmp / 'immutable').write_text(''.join(f'{r}\n' for r in immutable))
            (tmp / 'log').write_text('')
            env = {'PATH': f"{tmp / 'bin'}:{os.environ['PATH']}", 'STATE': str(tmp), 'GITHUB_SHA': self.SHA}
            script = step_script(CI, 'nightly', 'Publish the nightly release')
            run = subprocess.run(['bash', '-e', '-c', script], cwd=tmp, env=env, capture_output=True, text=True)
            return run, (tmp / 'releases').read_text().split(), (tmp / 'log').read_text().splitlines()

    def test_each_commit_gets_its_own_tag(self):
        # An immutable release's tag can never be reused, so a fixed tag fails.
        run, releases, log = self.publish([])
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(len(releases), 1)
        self.assertRegex(releases[0], r'^nightly-\d{8}-abcdef0$')
        create = next(line for line in log if line.startswith('release create'))
        self.assertIn(f'--target {self.SHA}', create)
        self.assertIn('--prerelease', create)
        self.assertIn('--latest=false', create)
        self.assertIn('dist/blockclonia_1_arm64.deb', create)

    def test_older_nightlies_go_and_releases_stay(self):
        run, releases, log = self.publish(['v26.11.0', 'nightly', 'nightly-20261101-1234567'])
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(releases[0], 'v26.11.0')
        self.assertEqual(len(releases), 2)
        self.assertTrue(releases[1].startswith('nightly-'))
        self.assertIn('release delete nightly --cleanup-tag --yes', log)
        self.assertNotIn('release delete v26.11.0 --cleanup-tag --yes', log)

    def test_the_new_nightly_is_published_before_the_old_ones_go(self):
        _, _, log = self.publish(['nightly-20261101-1234567'])
        verbs = [line.split()[1] for line in log]
        self.assertLess(verbs.index('create'), verbs.index('delete'))

    def test_a_rerun_does_not_publish_twice(self):
        _, first, _ = self.publish([])
        run, releases, log = self.publish(first)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(releases, first)
        self.assertFalse(any(line.startswith('release create') for line in log))

    def test_an_undeletable_old_nightly_only_warns(self):
        run, releases, _ = self.publish(['nightly-20261101-1234567'], immutable=['nightly-20261101-1234567'])
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertIn('::warning::could not fully delete nightly-20261101-1234567', run.stdout)
        self.assertIn('nightly-20261101-1234567', releases)


class Lint(unittest.TestCase):
    def test_the_lint_gate_runs_these_tests(self):
        self.assertIn('python3 -m unittest discover -s "$src/tools/tests"',
                      (ROOT / 'tools/lint.sh').read_text())
        self.assertIn('tools/lint.sh', job(CI, 'lint'))


if __name__ == '__main__':
    unittest.main()
