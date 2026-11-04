"""Gate on a GoogleTest JSON report (--gtest_output=json:<file>), never on an exit code.

Evidence drivers (mutation, baseline and red/green runs) call validate_report on the parsed
report. It requires complete named results: every case run and completed, consistent counts,
no errors or disabled cases, exactly the expected selection when one is given, and exactly
the expected set of failures (named failures for a run that must fail, none for a run that
must be green). Anything else raises RuntimeError. Checked by test_gtest_report.py (ctest).
"""


def require(condition, reason):
    if not condition:
        raise RuntimeError(reason)


def validate_report(document, expected=None, failures=()):
    """Return {'tests', 'names', 'failed'} for a report that meets every requirement above."""
    cases = {}
    failed = set()
    for suite in document['testsuites']:
        for test in suite['testsuite']:
            name = suite['name'] + '.' + test['name']
            require(name not in cases, f'duplicate test: {name}')
            require(test['status'] == 'RUN' and test['result'] == 'COMPLETED', f'incomplete/skipped test: {name}')
            cases[name] = test
            if test.get('failures'):
                failed.add(name)
    require(cases and document['tests'] == len(cases), 'missing tests or inconsistent test count')
    require(document['errors'] == 0 and document['disabled'] == 0, 'errors or disabled tests')
    require(document['failures'] == len(failed), 'inconsistent failure count')
    if expected is not None:
        require(set(cases) == set(expected), f'test selection differs: {set(cases) ^ set(expected)}')
    require(failed == set(failures), f'expected failures {set(failures)}, got {failed}')
    return {'tests': len(cases), 'names': sorted(cases), 'failed': sorted(failed)}
