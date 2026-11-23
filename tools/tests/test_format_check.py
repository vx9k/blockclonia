"""The rename-aware format check (tools/format_check.py)."""
import contextlib
import io
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import format_check  # noqa: E402

# Two statements keep the body on its own lines in the project's style.
GOOD = 'int f(int a)\n{\n    int b = a + 1;\n    return b;\n}\n'
# Formatted the project's way except for one line.
UGLY_LINE = 'int    g( int a ){return a;}\n'


class Runs(unittest.TestCase):
    def test_consecutive_lines_merge(self):
        self.assertEqual(format_check.runs([3, 1, 2, 7, 8, 10]), [(1, 3), (7, 8), (10, 10)])

    def test_duplicates_and_nothing(self):
        self.assertEqual(format_check.runs([5, 5, 5]), [(5, 5)])
        self.assertEqual(format_check.runs([]), [])


@unittest.skipUnless(shutil.which('clang-format'), 'needs clang-format')
class Check(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.dir)
        shutil.copy(ROOT / '.clang-format', self.dir / '.clang-format')
        self.git('init', '-q')
        # An old file that was never formatted on every line.
        self.write('src/a.c', GOOD + UGLY_LINE)
        self.write('README.md', 'notes\n')
        self.git('add', '-A')
        self.git('commit', '-q', '-m', 'base')
        self.base = self.git('rev-parse', 'HEAD').strip()

    def git(self, *args):
        return subprocess.run(['git', '-c', 'user.name=t', '-c', 'user.email=t@example.com', '-c',
                               'commit.gpgsign=false', *args], cwd=self.dir, check=True, capture_output=True,
                              text=True).stdout

    def write(self, path, text):
        (self.dir / path).parent.mkdir(parents=True, exist_ok=True)
        (self.dir / path).write_text(text)

    def check(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = format_check.main(['--base', self.base], cwd=self.dir)
        return code, out.getvalue()

    def test_nothing_changed_is_clean(self):
        self.assertEqual(self.check(), (0, 'clean\n'))

    def test_a_moved_file_is_not_checked_as_new(self):
        # git clang-format would check all of src/core/a.c and flag the old line.
        (self.dir / 'src/core').mkdir()
        self.git('mv', 'src/a.c', 'src/core/a.c')
        self.assertEqual(self.check(), (0, 'clean\n'))

    def test_a_moved_file_is_checked_where_it_changed(self):
        (self.dir / 'src/core').mkdir()
        self.git('mv', 'src/a.c', 'src/core/a.c')
        self.write('src/core/a.c', GOOD + UGLY_LINE + 'int    h( void ){return 0;}\n')
        code, out = self.check()
        self.assertEqual(code, 1)
        self.assertIn('src/core/a.c', out)

    def test_a_well_formatted_change_next_to_old_code_passes(self):
        self.write('src/a.c', GOOD + UGLY_LINE + '\nint h(void)\n{\n    int z = 0;\n    return z;\n}\n')
        self.assertEqual(self.check()[0], 0)

    def test_a_new_file_is_checked_whole(self):
        self.write('tests/t.h', 'int    k ;\n')
        self.git('add', 'tests/t.h')
        code, out = self.check()
        self.assertEqual(code, 1)
        self.assertIn('tests/t.h', out)

    def test_only_c_files_under_src_and_tests(self):
        self.write('README.md', 'notes   ,   changed\n')
        self.write('tools/x.c', 'int    k ;\n')
        self.write('src/notes.txt', 'int    k ;\n')
        self.git('add', '-A')
        self.assertEqual(self.check(), (0, 'clean\n'))


if __name__ == '__main__':
    unittest.main()
