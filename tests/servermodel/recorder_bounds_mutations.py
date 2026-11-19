#!/usr/bin/env python3
"""Run through the build queue from the checkout root; no concurrent builds.
Each mutation must compile, fail its named regression, then pass after restoration.
Only the two owned recorder/store source files are temporarily changed.
"""
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
BOOK = ROOT / 'libs/core/servermodel/BookRecorder.cpp'
STORE = ROOT / 'libs/core/servermodel/Hmc2Store.cpp'
BUILD = ROOT / 'build/mac-clang'
ENV = dict(os.environ, CCACHE_DIR=str(BUILD / 'ccache'))


def run(args, expected=0):
    result = subprocess.run(args, cwd=ROOT, env=ENV, stdin=subprocess.DEVNULL,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, timeout=240)
    if (expected == 0 and result.returncode != 0) or (expected != 0 and result.returncode == 0):
        print(result.stdout, flush=True)
        raise RuntimeError(f'unexpected status {result.returncode}: {args}')
    return result.stdout


def build():
    run(['cmake', '--build', '--preset', 'mac-clang', '-j', '6', '--target', 'test_book_recorder'])


def test(name, expected=0):
    return run([str(BUILD / 'tests/servermodel/test_book_recorder'), '--gtest_filter=RecorderTest.' + name], expected)


mutations = [
    ('absolute-window', BOOK,
     'if (layer.absoluteRows) return {layer.absoluteRows->low, layer.absoluteRows->high};',
     'if (layer.absoluteRows) return {1, 0};', 'AbsoluteWindowCoversEveryRowAtLowMiddleAndHighMids'),
    ('bounded-mid', BOOK, 'if (cfg.priceBounds) {\n        // Only actual',
     'if (false) {\n        // Only actual',
     'BoundedOneSidedAndEmptySnapshotsObserveWithoutSuspectWait:RecorderTest.BoundedTransitionsIntegrateAllThreeIntervals'),
    ('empty-snapshot', BOOK, 'if (!Bounded && m.kind == Kind::Snapshot &&',
     'if (m.kind == Kind::Snapshot &&', 'BoundedOneSidedAndEmptySnapshotsObserveWithoutSuspectWait'),
    ('zero-price', BOOK, 'const double scaled = price * cfg.priceScale;',
     'if (price == 0) return {};\n        const double scaled = price * cfg.priceScale;',
     'AbsoluteWindowCoversEveryRowAtLowMiddleAndHighMids:RecorderTest.BoundedEndpointMidsKeepZeroOpenAndMinimum'),
    ('zero-mid-sentinel', BOOK, 'cfg.priceBounds ? !s.midSeen : !s.midMin',
     '!s.midMin', 'BoundedEndpointMidsKeepZeroOpenAndMinimum'),
    ('crossed-mid', BOOK,
     'if (!s.bids.empty() && !s.asks.empty() && s.bids.rbegin()->first >= s.asks.begin()->first)',
     'if (false)', 'BoundedCrossedUpdatesSuspendAndCrossedSnapshotsInvalidate'),
    ('out-of-bounds', BOOK,
     '!std::isfinite(price) || price < cfg.priceBounds->lower || price > cfg.priceBounds->upper',
     '!std::isfinite(price)', 'BoundedOutOfRangeBatchInvalidatesAtomicallyUntilSnapshot'),
    ('legacy-grace', BOOK, 'target - s.unobservedSince < cfg.oneSidedSuspectMs',
     'target - s.unobservedSince < cfg.oneSidedSuspectMs + 1',
     'UnboundedEmptyBooksRetainThirtySecondSuspectAndSnapshotRejection'),
    ('ambiguous-window', BOOK, 'l.lowFrac == 0 && l.highMult == 0 && l.absoluteRows->low >= 0',
     'l.absoluteRows->low >= 0', 'BoundedPolicyValidationRejectsAmbiguityAndInvalidBounds'),
    ('absolute-hash', STORE, 'if (absoluteRows) {', 'if (false) {',
     'PolicyHashChangesAlwaysStartNewGenerationInBothDirections'),
    ('bounded-hash', STORE, 'if (priceBounds) {', 'if (false) {',
     'PolicyHashChangesAlwaysStartNewGenerationInBothDirections'),
    ('legacy-hash-drift', STORE, 'put(b, high);', 'put(b, high); put(b, uint32_t{1});',
     'BaseCompatibilityBytes'),
    ('restored-observation', BOOK, 'v.observing = v.valid && mid.has_value();',
     'v.observing = v.valid && mid.has_value() && !v.bids.empty() && !v.asks.empty();',
     'BoundedOneSidedStateAndLeadContinueIdentically'),
    ('restored-mid-validation', BOOK,
     'stateRequire(!v.observing || (v.midSeen && v.mid == *mid && v.unobservedSince == -1), "bounded midpoint state");',
     '', 'BoundedStateRejectsCorruptPolicyPriceAndMidBeforeInstallation'),
]

build()
run(['ctest', '--test-dir', str(BUILD), '--output-on-failure', '-R', '^BookRecorderTests$'])
for label, path, old, new, case in mutations:
    original = path.read_text()
    assert original.count(old) == 1, (label, original.count(old))
    try:
        path.write_text(original.replace(old, new))
        build()
        output = test(case, 1)
        assert '[  FAILED  ]' in output, output
        print(f'KILLED {label}: {case}', flush=True)
        for line in output.splitlines():
            if 'Failure' in line or '[  FAILED  ]' in line:
                print('  ' + line, flush=True)
    finally:
        path.write_text(original)
        build()
    test(case)
    print(f'RESTORED {label}: PASS', flush=True)
print(f'mutations: {len(mutations)}/{len(mutations)} killed; all restored checks passed', flush=True)
