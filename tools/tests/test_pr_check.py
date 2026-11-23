"""The pull request rules: the title AGENTS.md asks for and the 1:1 ratio of
added test lines to added code lines."""
import importlib.util
import io
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout

TOOLS = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('pr_check', TOOLS / 'pr_check.py')
pr_check = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pr_check)


class Title(unittest.TestCase):
    def test_the_agents_md_examples_pass(self):
        for title in ['feat(camera): add a --fov option for the field of view',
                      'fix(physics): stop water duplicating at column edges',
                      'feat(inventory)!: store item durability in the saved inventory',
                      'fix(save): reject columns with negative heights',
                      'ci: port the release workflow',
                      'refactor!: drop the old mesher']:
            self.assertIsNone(pr_check.title_problem(title), title)

    def test_identifiers_keep_their_case_after_the_first_word(self):
        self.assertIsNone(pr_check.title_problem('fix(renderer): guard Vulkan 1.4 headers'))
        self.assertIsNone(pr_check.title_problem('feat(save): read INV1 inventories'))

    def test_only_the_types_agents_md_allows(self):
        for kind in ['feat', 'fix', 'perf', 'refactor', 'test', 'docs', 'build', 'ci', 'style', 'chore', 'revert']:
            self.assertIsNone(pr_check.title_problem(f'{kind}: do a thing'), kind)
        for kind in ['bench', 'feature', 'bugfix', 'Fix', 'wip']:
            self.assertIsNotNone(pr_check.title_problem(f'{kind}: do a thing'), kind)

    def test_the_description_starts_lower_case(self):
        self.assertIsNotNone(pr_check.title_problem('fix: Stop water duplicating'))

    def test_no_final_full_stop_or_trailing_space(self):
        self.assertIsNotNone(pr_check.title_problem('fix: stop water duplicating.'))
        self.assertIsNotNone(pr_check.title_problem('fix: stop water duplicating '))

    def test_the_shape_of_the_header(self):
        for title in ['fix stop water', 'fix:stop water', 'fix(): stop water', 'fix(Physics): stop water',
                      'fix(physics) : stop water', 'fix:  stop water', 'stop water duplicating', '']:
            self.assertIsNotNone(pr_check.title_problem(title), title)

    def test_the_header_fits_in_72_columns(self):
        title = 'fix(save): ' + 'a' * (72 - len('fix(save): '))
        self.assertIsNone(pr_check.title_problem(title))
        problem = pr_check.title_problem(title + 'a')
        self.assertIn('73 characters', problem)


class Kind(unittest.TestCase):
    def test_sources_and_headers_are_code(self):
        self.assertEqual(pr_check.kind('src/physics.c'), 'code')
        self.assertEqual(pr_check.kind('src/physics.h'), 'code')

    def test_test_files_and_fuzz_targets_are_tests(self):
        self.assertEqual(pr_check.kind('tests/test_thermo.c'), 'test')
        self.assertEqual(pr_check.kind('tests/test_util.h'), 'test')
        self.assertEqual(pr_check.kind('tests/fuzz_save.c'), 'test')

    def test_tools_and_their_tests(self):
        self.assertEqual(pr_check.kind('tools/release.py'), 'code')
        self.assertEqual(pr_check.kind('tools/tests/test_release.py'), 'test')

    def test_window_gpu_and_audio_modules_count_for_neither_side(self):
        for path in ['src/main.c', 'src/renderer.c', 'src/audio_device.c', 'shaders/block.frag',
                     'shaders/push_vert.glsl']:
            self.assertIsNone(pr_check.kind(path), path)

    def test_docs_build_files_and_workflows_count_for_neither_side(self):
        for path in ['README.md', 'CMakeLists.txt', 'cmake/Packaging.cmake', '.github/workflows/ci.yml',
                     'tools/lint.sh', 'tools/release.sh']:
            self.assertIsNone(pr_check.kind(path), path)


class Comments(unittest.TestCase):
    def test_c_comments_and_blank_lines_are_skipped(self):
        for line in ['', '   ', '// why', '/* why', ' * more', ' */']:
            self.assertTrue(pr_check.comment_or_blank(line, python=False), repr(line))
        for line in ['int x = 0;', '}', 'x = a / b;']:
            self.assertFalse(pr_check.comment_or_blank(line, python=False), line)

    def test_python_comments_are_skipped(self):
        self.assertTrue(pr_check.comment_or_blank('    # why', python=True))
        self.assertFalse(pr_check.comment_or_blank('x = 1  # why', python=True))
        self.assertFalse(pr_check.comment_or_blank('// not a comment here', python=True))


class Counting(unittest.TestCase):
    def test_added_lines_follow_the_hunks(self):
        diff = ('diff --git a/src/a.c b/src/a.c\n--- a/src/a.c\n+++ b/src/a.c\n'
                '@@ -1,0 +2,3 @@\n+x\n+y\n+z\n@@ -9 +12 @@\n-old\n+new\n@@ -20,2 +23,0 @@\n-gone\n-gone\n'
                'diff --git a/gone.c b/gone.c\n--- a/gone.c\n+++ /dev/null\n@@ -1 +0,0 @@\n-x\n')
        self.assertEqual(pr_check.added_lines(diff), {'src/a.c': [2, 3, 4, 12]})
        # Nothing in it is only layout, so new_lines sees the same.
        self.assertEqual(pr_check.new_lines(diff), {'src/a.c': [2, 3, 4, 12]})

    @staticmethod
    def one_file(path, *hunks):
        return f'--- a/{path}\n+++ b/{path}\n' + ''.join(hunks)

    def test_reindenting_or_rewrapping_adds_nothing(self):
        indent = '@@ -3 +3 @@\n-  int a = 0;\n+    int a = 0;\n'
        join = '@@ -7,2 +7 @@\n-static int f(int a,\n-           int b) { return a + b; }\n' \
            '+static int f(int a, int b) { return a + b; }\n'
        split = '@@ -9 +9,4 @@\n-int g( void ){return 1;}\n+int g(void)\n+{\n+    return 1;\n+}\n'
        self.assertEqual(pr_check.new_lines(self.one_file('src/a.c', indent, join, split)), {})

    def test_whitespace_between_words_is_code(self):
        diff = self.one_file('src/a.c', '@@ -3 +3 @@\n-int ab;\n+int a b;\n')
        self.assertEqual(pr_check.new_lines(diff), {'src/a.c': [3]})

    def test_sorting_includes_adds_nothing(self):
        diff = self.one_file('src/a.h', '@@ -3 +2,0 @@\n-#include "mem.h"\n', '@@ -5,0 +5 @@\n+#include "mem.h"\n')
        self.assertEqual(pr_check.new_lines(diff), {})

    def test_a_line_moved_to_another_file_counts(self):
        diff = self.one_file('src/a.c', '@@ -3 +2,0 @@\n-int a = 0;\n') + \
            self.one_file('src/b.c', '@@ -1,0 +2 @@\n+int a = 0;\n')
        self.assertEqual(pr_check.new_lines(diff), {'src/b.c': [2]})

    def test_a_reindented_line_in_a_real_change_does_not_count(self):
        diff = self.one_file('src/a.c', '@@ -3,2 +3,3 @@\n-if (a)\n-  b();\n+if (a) {\n+    b();\n+    c();\n')
        self.assertEqual(pr_check.new_lines(diff), {'src/a.c': [3, 5]})

    def test_a_moved_line_cancels_once(self):
        diff = self.one_file('src/a.c', '@@ -3 +2,0 @@\n-    break;\n', '@@ -8,0 +8,2 @@\n+    break;\n+    break;\n')
        self.assertEqual(pr_check.new_lines(diff), {'src/a.c': [9]})

    def test_a_reformatted_hunk_cancels_no_moves(self):
        # The reformatted hunk's removed line is not there for another hunk to match.
        diff = self.one_file('src/a.c', '@@ -3 +3 @@\n-x( );\n+x();\n', '@@ -8,0 +9 @@\n+x();\n')
        self.assertEqual(pr_check.new_lines(diff), {'src/a.c': [9]})

    def test_python_layout_is_code(self):
        diff = self.one_file('tools/a.py', '@@ -3 +3 @@\n-x = 1\n+    x = 1\n', '@@ -6 +5,0 @@\n-import os\n',
                             '@@ -9,0 +9 @@\n+import os\n')
        self.assertEqual(pr_check.new_lines(diff), {'tools/a.py': [3, 9]})

    def test_count_sorts_lines_and_skips_comments(self):
        files = {
            'src/a.c': '// comment\nint a = 0;\n\nint b = 0;\n',
            'tests/test_a.c': 'CHECK(a == 0);\n/* c */\n',
            'src/renderer.c': 'vkCmdDraw(cb, 3, 1, 0, 0);\n',
            'README.md': 'words\n',
        }
        added = {'src/a.c': [1, 2, 3, 4], 'tests/test_a.c': [1, 2], 'src/renderer.c': [1], 'README.md': [1]}
        self.assertEqual(pr_check.count(added, files.__getitem__), (1, 2, 1))

    def test_lines_past_the_end_are_ignored(self):
        self.assertEqual(pr_check.count({'src/a.c': [5]}, lambda _: 'int a;\n'), (0, 0, 0))

    def test_the_ratio(self):
        self.assertIsNone(pr_check.ratio_problem(0, 0))
        self.assertIsNone(pr_check.ratio_problem(5, 5))
        self.assertIsNone(pr_check.ratio_problem(9, 5))
        self.assertIn('add 2 more lines of tests', pr_check.ratio_problem(3, 5))


class EndToEnd(unittest.TestCase):
    """main() on a throwaway repository, with the user's Git configuration
    (commit signing) shut out."""

    def setUp(self):
        self.root = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.root)
        self.env = {'GIT_CONFIG_GLOBAL': os.devnull, 'GIT_CONFIG_NOSYSTEM': '1', 'GIT_AUTHOR_NAME': 't',
                    'GIT_AUTHOR_EMAIL': 't@t', 'GIT_COMMITTER_NAME': 't', 'GIT_COMMITTER_EMAIL': 't@t'}
        self.saved = {key: os.environ.get(key) for key in self.env}
        os.environ.update(self.env)
        self.addCleanup(self.restore)
        self.git('init', '-q', '-b', 'main')
        self.write('src/a.c', 'int a = 0;\n')
        self.base = self.commit()

    def restore(self):
        for key, value in self.saved.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value

    def git(self, *args):
        return subprocess.run(['git', *args], cwd=self.root, check=True, capture_output=True, text=True).stdout

    def write(self, path, text):
        (self.root / path).parent.mkdir(parents=True, exist_ok=True)
        (self.root / path).write_text(text)

    def commit(self):
        self.git('add', '-A')
        self.git('commit', '-q', '-m', 'x')
        return self.git('rev-parse', 'HEAD').strip()

    def run_check(self, title):
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            status = pr_check.main(['--base', self.base, '--title', title], cwd=self.root)
        return status, out.getvalue(), err.getvalue()

    def test_code_with_as_many_test_lines_passes(self):
        self.write('src/a.c', 'int a = 0;\nint b = 1;\nint c = 2;\n')
        self.write('tests/test_a.c', 'CHECK(b == 1);\nCHECK(c == 2);\n')
        self.commit()
        self.assertEqual(self.run_check('feat(a): add b and c'), (0, '2 test lines, 2 code lines\n', ''))

    def test_a_moved_file_adds_no_code_lines(self):
        # Even where the user's Git doesn't detect renames on its own.
        self.git('config', 'diff.renames', 'false')
        (self.root / 'src/core').mkdir()
        self.git('mv', 'src/a.c', 'src/core/a.c')
        self.commit()
        self.assertEqual(self.run_check('refactor: move a'), (0, '0 test lines, 0 code lines\n', ''))

    def test_reformatting_adds_no_code_lines(self):
        self.write('src/a.c', 'int f(int x)\n{\n  return x;\n}\n')
        self.base = self.commit()
        self.write('src/a.c', 'int f(int x) { return x; }\n')
        self.commit()
        self.assertEqual(self.run_check('style: format a'), (0, '0 test lines, 0 code lines\n', ''))

    def test_code_without_tests_fails_and_says_how_many_are_missing(self):
        self.write('src/a.c', 'int a = 0;\nint b = 1;\n')
        self.commit()
        status, _, err = self.run_check('feat(a): add b')
        self.assertEqual(status, 1)
        self.assertIn('add 1 more lines of tests', err)

    def test_renderer_lines_are_reported_but_not_required_to_have_tests(self):
        self.write('src/renderer.c', 'void draw(void) {}\n')
        self.commit()
        status, out, _ = self.run_check('feat(renderer): draw')
        self.assertEqual(status, 0)
        self.assertIn('1 in modules only a window, GPU or audio device runs', out)

    def test_a_bad_title_fails_even_with_tests(self):
        self.write('tests/test_a.c', 'CHECK(1);\n')
        self.commit()
        status, _, err = self.run_check('Added a test.')
        self.assertEqual(status, 1)
        self.assertIn('not a Conventional Commit', err)


if __name__ == '__main__':
    unittest.main()
