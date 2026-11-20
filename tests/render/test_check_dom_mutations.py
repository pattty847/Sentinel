#!/usr/bin/env python3
"""Unit tests for the subprocess timeout of check_dom_mutations.py (no build, no source edits)."""
import importlib.util
import sys
import time
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location('check_dom_mutations', Path(__file__).with_name('check_dom_mutations.py'))
runner = importlib.util.module_from_spec(spec)
sys.argv = [sys.argv[0]]  # the runner reads argv[1] as the build directory
spec.loader.exec_module(runner)


def py(code):
    return [sys.executable, '-I', '-c', code]


class MutantTimeout(unittest.TestCase):
    def test_hung_test_times_out_and_counts_as_killed(self):
        start = time.monotonic()
        returncode, text = runner.run_behavior_test(py('import time; print("started", flush=True); time.sleep(60)'), 0.5)
        self.assertLess(time.monotonic() - start, 15, 'the runner waited for the hung child')
        self.assertIsNone(returncode)
        self.assertIn('TIMEOUT', text)
        self.assertIn('started', text, 'output printed before the hang is kept')
        self.assertTrue(runner.mutant_killed(returncode, text))

    def test_child_gets_eof_on_stdin(self):
        start = time.monotonic()
        returncode, text = runner.run_behavior_test(py('import sys; sys.stdin.read(); print("eof")'), 30)
        self.assertLess(time.monotonic() - start, 15, 'the child waited on the runner stdin')
        self.assertEqual(returncode, 0)
        self.assertIn('eof', text)

    def test_failing_gtest_is_killed(self):
        returncode, text = runner.run_behavior_test(py('import sys; print("[  FAILED  ] X.y"); sys.exit(1)'), 30)
        self.assertEqual(returncode, 1)
        self.assertTrue(runner.mutant_killed(returncode, text))

    def test_passing_test_is_a_survivor(self):
        returncode, text = runner.run_behavior_test(py('print("[  PASSED  ] 1 test.")'), 30)
        self.assertEqual(returncode, 0)
        self.assertFalse(runner.mutant_killed(returncode, text))

    def test_crash_or_exit_one_without_failed_line_is_not_killed(self):
        returncode, text = runner.run_behavior_test(py('import sys; sys.exit(1)'), 30)
        self.assertFalse(runner.mutant_killed(returncode, text), 'a bare exit 1 may be a broken harness, not a rejection')


if __name__ == '__main__':
    unittest.main()
