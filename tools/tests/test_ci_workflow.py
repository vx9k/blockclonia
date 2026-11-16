"""Pin the parts of the workflows that other jobs, the ruleset and people
rely on."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]
WORKFLOWS = ROOT / '.github/workflows'
CI = (WORKFLOWS / 'ci.yml').read_text()
PACKAGE = (WORKFLOWS / 'package.yml').read_text()
PR = (WORKFLOWS / 'pr.yml').read_text()
# Jobs that run but may fail without blocking a merge.
EXPERIMENTAL = {'macos'}
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
        self.assertIn('ref: v${{ needs.release.outputs.version }}', package)
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
        self.assertEqual(PACKAGE.count('if [ -z "$REF" ]; then'), 2)
        self.assertEqual(PACKAGE.count('-DMC_PACKAGE_VERSION_SUFFIX="${MC_PACKAGE_VERSION_SUFFIX:-}"'), 2)
        cmake = (ROOT / 'cmake/Packaging.cmake').read_text()
        self.assertIn('set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}${MC_PACKAGE_VERSION_SUFFIX}")', cmake)
        self.assertLess(cmake.index('CPACK_PACKAGE_VERSION'), cmake.index('include(CPack)'))


class Packages(unittest.TestCase):
    def test_both_pi_packages_are_built_and_uploaded(self):
        self.assertEqual(set(jobs(PACKAGE)), {'pi-arm64', 'pi-armhf'})
        for name in ['pi-arm64', 'pi-armhf']:
            body = job(PACKAGE, name)
            self.assertIn(f'name: ${{{{ inputs.artifact-prefix }}}}-{name}', body)
            self.assertIn('if-no-files-found: error', body)
            self.assertIn('ref: ${{ inputs.ref }}', body)

    def test_ci_builds_them_for_every_run(self):
        self.assertIn('uses: ./.github/workflows/package.yml', job(CI, 'package'))


class Lint(unittest.TestCase):
    def test_the_lint_gate_runs_these_tests(self):
        self.assertIn('python3 -m unittest discover -s "$src/tools/tests"',
                      (ROOT / 'tools/lint.sh').read_text())
        self.assertIn('tools/lint.sh', job(CI, 'lint'))


if __name__ == '__main__':
    unittest.main()
