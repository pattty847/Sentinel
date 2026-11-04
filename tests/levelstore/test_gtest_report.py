"""The report gate must reject missing, skipped, crashed or unexpectedly passing tests."""
import copy
import unittest
from gtest_report import validate_report


def report(failed=True):
    test = {'name': 'Case', 'status': 'RUN', 'result': 'COMPLETED'}
    if failed:
        test['failures'] = [{'failure': 'expected assertion failed'}]
    return {'tests': 1, 'failures': int(failed), 'errors': 0, 'disabled': 0,
            'testsuites': [{'name': 'Suite', 'testsuite': [test]}]}


class ReportGate(unittest.TestCase):
    def test_named_assertion_failure_is_required(self):
        self.assertEqual(validate_report(report(), ['Suite.Case'], ['Suite.Case'])['failed'], ['Suite.Case'])
        with self.assertRaises(RuntimeError):
            validate_report(report(False), ['Suite.Case'], ['Suite.Case'])

    def test_no_missing_or_substituted_test(self):
        for expected in [['Suite.Other'], ['Suite.Case', 'Suite.Missing']]:
            with self.subTest(expected=expected), self.assertRaises(RuntimeError):
                validate_report(report(), expected, expected)

    def test_skipped_or_incomplete_is_not_a_failure(self):
        for field, value in [('status', 'NOTRUN'), ('result', 'SKIPPED'), ('result', 'SUPPRESSED')]:
            doc = report()
            doc['testsuites'][0]['testsuite'][0][field] = value
            with self.subTest(field=field, value=value), self.assertRaises(RuntimeError):
                validate_report(doc, ['Suite.Case'], ['Suite.Case'])

    def test_bad_counts_and_empty_results_are_rejected(self):
        for field, value in [('tests', 2), ('failures', 0), ('errors', 1), ('disabled', 1)]:
            doc = report()
            doc[field] = value
            with self.subTest(field=field), self.assertRaises(RuntimeError):
                validate_report(doc, ['Suite.Case'], ['Suite.Case'])
        doc = report(False)
        doc.update(tests=0, testsuites=[])
        with self.assertRaises(RuntimeError):
            validate_report(doc)

    def test_duplicate_names_are_rejected(self):
        doc = report()
        doc['testsuites'].append(copy.deepcopy(doc['testsuites'][0]))
        with self.assertRaises(RuntimeError):
            validate_report(doc, ['Suite.Case'], ['Suite.Case'])

    def test_restored_run_must_be_green(self):
        self.assertEqual(validate_report(report(False))['failed'], [])
        with self.assertRaises(RuntimeError):
            validate_report(report())

    def test_aborted_report_without_cases_is_rejected(self):
        with self.assertRaises(KeyError):
            validate_report({'errors': 1})


if __name__ == '__main__':
    unittest.main()
