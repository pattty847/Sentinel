#!/usr/bin/env python3
"""Run alone through the owner's build queue. Refuses dirty trees; restores on every exit."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / 'build/mac-clang'
TEST = BUILD / 'tests/roller/test_kalshi_roll'


def run(args, timeout=180):
    return subprocess.run(args, cwd=ROOT, stdin=subprocess.DEVNULL, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout)


def build():
    result = run(['cmake', '--build', '--preset', 'mac-clang', '-j', '6', '--target', 'test_kalshi_roll'], 300)
    if result.returncode:
        raise RuntimeError(result.stdout)


def check(pattern):
    return run([str(TEST), '--gtest_filter=' + pattern])


MUTATIONS = [
    ('Coinbase bytes', 'Roller.cpp', '{"grace",5000}', '{"grace",5001}', 'CoinbaseRollBytes.*'),
    ('Kalshi snapshot wiring', 'Roller.cpp', 'if (input.venue == Venue::Kalshi) {',
     'if (false && input.venue == Venue::Kalshi) {', 'KalshiRoll.*'),
    ('absolute window', 'Grid.cpp', 'layer.absoluteRows = recording::AbsoluteRowBounds{0, int64_t(priceScale) / layer.rowTickUnits};',
     'layer.absoluteRows.reset();', 'KalshiRoll.GridTableAndMalformedPoliciesWriteNothing'),
    ('bounded midpoint', 'Grid.cpp', 'c.priceBounds = recording::PriceBounds{0,1};',
     'c.priceBounds = recording::PriceBounds{0,0.9};', 'KalshiRoll.OneSidedAndEmptyStartUseBoundsIncludingExtendedTickers'),
    ('instrument identity', 'Roller.cpp', 'if (!instrument.is_null()) j["instrument"] = instrument;',
     '', 'KalshiRoll.BandIdentityRejectsCheckpointAndVenueMismatch'),
    ('row policy hash', 'Roller.cpp', 'if (l.absoluteRows) layer.push_back({l.absoluteRows->low,l.absoluteRows->high});',
     '', 'KalshiRoll.BandIdentityRejectsCheckpointAndVenueMismatch'),
    ('price bounds hash', 'Roller.cpp', 'if (c.priceBounds) j["priceBounds"] = {c.priceBounds->lower,c.priceBounds->upper};',
     '', 'KalshiRoll.BandIdentityRejectsCheckpointAndVenueMismatch'),
    ('journal snapshot search', 'JournalReader.cpp', 'const bool kalshi = venueFromHeader(f->header) == Venue::Kalshi;',
     'const bool kalshi = false;', 'KalshiRoll.JournalAnchorSearchAcrossHoursRunsAndForeignSnapshots'),
    ('anchor instrument roundtrip', 'Roller.cpp', 'a.instrument = gridInstrument;',
     'a.instrument = nullptr;', 'KalshiRoll.MidnightAnchorAndOldJournalSnapshotHaveIdenticalNextDayBytes'),
    ('anchor installation', 'Roller.cpp', 'if (o.useAnchors && !o.dryRun) for (const auto& file : candidates)',
     'if (false) for (const auto& file : candidates)', 'KalshiRoll.MidnightAnchorAndOldJournalSnapshotHaveIdenticalNextDayBytes:KalshiRoll.CrashResumeMatchesUninterrupted'),
]


def main():
    status = run(['git', 'status', '--porcelain'])
    if status.returncode or status.stdout.strip():
        raise RuntimeError('mutation runner requires a clean tree')
    build()
    baseline = check('*')
    if baseline.returncode:
        raise RuntimeError('baseline failed:\n' + baseline.stdout)
    killed = 0
    for name, filename, old, new, pattern in MUTATIONS:
        path = ROOT / 'libs/core/roller' / filename
        original = path.read_text()
        if original.count(old) != 1:
            raise RuntimeError('nonunique mutation: ' + name)
        try:
            path.write_text(original.replace(old, new))
            build()
            result = check(pattern)
            if result.returncode == 0:
                raise RuntimeError('survived: ' + name)
            # A compilation failure never counts; require a test failure report.
            if '[  FAILED  ]' not in result.stdout:
                raise RuntimeError('not an assertion failure: ' + name + '\n' + result.stdout)
            print('KILLED ' + name, flush=True)
            for line in result.stdout.splitlines():
                if 'Failure' in line or '[  FAILED  ]' in line:
                    print(line, flush=True)
            killed += 1
        finally:
            path.write_text(original)
            build()
            restored = check(pattern)
            if restored.returncode:
                raise RuntimeError('restored check failed:\n' + restored.stdout)
            print('RESTORED ' + name, flush=True)
    print(f'mutations: {killed}/{len(MUTATIONS)} killed; all restored checks passed', flush=True)


if __name__ == '__main__':
    main()
