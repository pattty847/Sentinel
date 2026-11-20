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
    ('changed header representability', 'Roller.cpp', 'if (input.venue == Venue::Kalshi && lastInstrumentHeader != input.instrument)',
     'if (input.venue == Venue::Kalshi && lastInstrumentHeader.is_null())', 'KalshiRecovery.MiddayUnrepresentableInstrumentPreservesPrefix'),
    ('canonical instrument metadata', 'Grid.cpp', 'return normalized;',
     'return instrument;', 'KalshiRecovery.EquivalentDecimalHeadersShareIdentityAndPersistNormalizedInstrument'),
    ('anchor journal header guard', 'Roller.cpp', 'throw std::runtime_error("anchor venue/instrument mismatch");',
     '(void)0;', 'KalshiRecovery.NoCheckpointAnchorAcceptsBandsAndRejectsVenue'),
    ('checkpoint instrument metadata', 'Roller.cpp', 'cp["days"][key]["instrumentHistory"] = instrumentHistory;',
     '', 'KalshiRecovery.EquivalentDecimalHeadersShareIdentityAndPersistNormalizedInstrument'),
    ('fixed near grid', 'Grid.cpp', 'Grid grid{1000,.001,.01,std::stod(spec.sizeIncrement)};',
     'Grid grid{1000,.01,.01,std::stod(spec.sizeIncrement)};', 'KalshiRoll.GridTableAndMalformedPoliciesWriteNothing'),
    ('millidollar policy limit', 'Grid.cpp', 'if (fraction > 3) throw std::runtime_error("band price/step must be an integer multiple of 0.001");',
     'if (fraction > 3) return int64_t(1);', 'KalshiRoll.GridTableAndMalformedPoliciesWriteNothing'),
    ('off-grid level admission', 'Roller.cpp', 'if (units % 10)',
     'if (false && units % 10)', 'KalshiRecovery.OffGridSnapshotAndDeltaStopBeforeQuantization'),
    ('anchor instrument history', 'Roller.cpp', 'if (gridVenue == Venue::Kalshi) feedState["instrumentHistory"] = instrumentHistory;',
     '', 'KalshiRecovery.BandChangesKeepGridCheckpointAnchorsAndDeterministicHistory'),
    ('history change entries', 'Roller.cpp', 'instrumentHistory.empty() || instrumentHistory.back().at("instrument") != gridInstrument',
     'instrumentHistory.empty()', 'KalshiRecovery.BandChangesKeepGridCheckpointAnchorsAndDeterministicHistory'),
    ('size floor identity', 'Roller.cpp', '{"floor",c.sizeScale.floor}',
     '{"floor",.01}', 'KalshiRoll.SizeIdentityRejectsCheckpointAndVenueMismatch'),
    ('Kalshi hourly rollup', 'Grid.cpp', 'layer.lowFrac = layer.highMult = 0;',
     'layer.lowFrac = layer.highMult = 0; layer.hourlyRollup = false;', 'KalshiRecovery.ThreeHourDeltasOneSidedMidnightAndHourCrashHaveIdenticalBytes'),
    ('restored delta integration', 'Roller.cpp', 'if (recorder) recorder->onUpdatesAt(o.product,t,local,std::move(levels));',
     'if (recorder && !restoreLocal) recorder->onUpdatesAt(o.product,t,local,std::move(levels));', 'KalshiRecovery.ThreeHourDeltasOneSidedMidnightAndHourCrashHaveIdenticalBytes'),
    ('Coinbase bytes', 'Roller.cpp', '{"grace",5000}', '{"grace",5001}', 'CoinbaseRollBytes.*'),
    ('Kalshi snapshot wiring', 'Roller.cpp', 'if (input.venue == Venue::Kalshi) {',
     'if (false && input.venue == Venue::Kalshi) {', 'KalshiRoll.*'),
    ('absolute window', 'Grid.cpp', 'layer.absoluteRows = recording::AbsoluteRowBounds{0, int64_t(priceScale) / layer.rowTickUnits};',
     'layer.absoluteRows.reset();', 'KalshiRoll.GridTableAndMalformedPoliciesWriteNothing'),
    ('bounded midpoint', 'Grid.cpp', 'c.priceBounds = recording::PriceBounds{0,1};',
     'c.priceBounds = recording::PriceBounds{0,0.9};', 'KalshiRoll.OneSidedAndEmptyStartUseBoundsIncludingExtendedTickers'),
    ('band-independent roller identity', 'Roller.cpp', 'hash = configHash(cfg);',
     'hash = configHash(cfg) ^ std::hash<std::string>{}(gridInstrument.dump());', 'KalshiRecovery.NoCheckpointAnchorAcceptsBandsAndRejectsVenue:KalshiRecovery.BandChangesKeepGridCheckpointAnchorsAndDeterministicHistory'),
    ('row policy hash', 'Roller.cpp', 'if (l.absoluteRows) layer.push_back({l.absoluteRows->low,l.absoluteRows->high});',
     '', 'KalshiRoll.SizeIdentityRejectsCheckpointAndVenueMismatch'),
    ('price bounds hash', 'Roller.cpp', 'if (c.priceBounds) j["priceBounds"] = {c.priceBounds->lower,c.priceBounds->upper};',
     '', 'KalshiRoll.SizeIdentityRejectsCheckpointAndVenueMismatch'),
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
