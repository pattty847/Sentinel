#!/usr/bin/env python3
"""Run via scripts/dev/build-queue.sh --label lt-astra/roller-a -- python3 tests/roller/check_mutations.py.
Every restoration writes fresh source, touches it and rebuilds (FM-132).
No logs/files beyond build products are written; output is a concise audit.
"""
import os
import sys
from pathlib import Path
import subprocess

os.environ['CCACHE_DISABLE'] = '1'
root = Path(__file__).resolve().parents[2]
os.chdir(root)

def command(args, expected=0):
    p = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if p.returncode != expected:
        print(p.stdout[-12000:], flush=True)
        raise RuntimeError(f'{args}: expected {expected}, got {p.returncode}')
    return p.stdout

def build():
    command(['cmake', '--build', '--preset', 'mac-clang', '-j', '4', '--target', 'test_roller', 'test_roller_live_fixture'])

def test(name, expect_failure=False):
    target = 'test_roller_live_fixture' if name.startswith('RollerLiveFixture.') else 'test_roller'
    output = command([f'build/mac-clang/tests/roller/{target}', '--gtest_filter=' + name], 1 if expect_failure else 0)
    if expect_failure and '[  FAILED  ]' not in output:
        raise RuntimeError('mutation failed outside the assertion harness')

review_mutations = [
 ('range boundary', 'libs/core/roller/Roller.cpp',
  'feed.apply(input); applied = input.pos;',
  "if (input.record.time.systemNs / 1'000'000 >= end + 60'000) break; feed.apply(input); applied = input.pos;",
  'Roller.SilenceAcrossMidnightCommitsMinuteAndHour'),
 ('unowned HMC2 root', 'libs/core/roller/Roller.cpp',
  'if (fs::exists(dir) && !fs::is_regular_file(dir/"roller.json"))', 'if (false)',
  'Roller.RefusesUnownedHmc2AndConfiguredRecorderRoots'),
 ('configured live root', 'libs/core/roller/RollCli.cpp',
  'if (dest.starts_with(live) || live.starts_with(dest))', 'if (false)',
  'Roller.RefusesUnownedHmc2AndConfiguredRecorderRoots'),
 ('sequence recovery', 'libs/core/roller/JournalFeed.cpp',
  'sequence_ = seq; // Re-anchor sequence tracking', 'if (!discontinuity) sequence_ = seq; // Re-anchor sequence tracking',
  'Roller.SequenceErrorAllowsFollowingSnapshotWithoutTransportUp'),
 ('stop broadcast', 'libs/core/servermodel/BookRecorder.cpp',
  'if (cfg.blockingQueue) space.notify_all();', 'if (cfg.blockingQueue) space.notify_one();',
  'Roller.StopWakesBlockedProducerBeforeWorkerDrains'),
 ('large delta resume', 'libs/core/roller/Grid.cpp',
  'c.blockingQueue = c.deterministicResume = true;', 'c.blockingQueue = true; c.deterministicResume = false;',
  'Roller.RealJournalCrashResumeRestoresLargeDeltaBase'),
 ('delta histogram', 'libs/core/roller/Diff.cpp',
  'std::to_string(it->second.first-value.first)', 'std::to_string(value.first-it->second.first)',
  'Roller.DiffQualifiesMasksLateAndDetectsContent'),
 ('total TWAP delta', 'libs/core/roller/Diff.cpp',
  'const auto delta=totalB-totalA;', 'const auto delta=totalA-totalB;',
  'Roller.DiffQualifiesMasksLateAndDetectsContent'),
 ('controlled fixture bands', 'libs/core/roller/JournalFeed.cpp',
  'levels.push_back({l.isBid,l.price,l.quantity});', 'levels.push_back({l.isBid,l.price,l.quantity*1.01});',
  'RollerLiveFixture.ControlledTickScheduleStaysWithinBands'),
]
mutations = [
 ('superseding run', 'libs/core/roller/JournalReader.cpp',
  'files_[i].superseded = i + 1 < files_.size();', 'files_[i].superseded = false;', 'Roller.PendingFramingDeferredUntilSealedOrSuperseded'),
 ('daily dry-run grid', 'libs/core/roller/Roller.cpp',
  'if (!gridReady)', 'if (!recorder)', 'Roller.DryRunAndEofDoNotInventTime'),
 ('derived grid', 'libs/core/roller/Grid.cpp',
  'tick = std::ceil(std::max(tick, quote) / quote - 1e-9) * quote;', 'tick = quote;', 'Roller.GridsAndClamping'),
 ('record position', 'libs/core/capture/RawCapture.cpp',
  'result.recordIndex = i;', 'result.recordIndex = 0;', 'Roller.ReaderVersionsPositionsAndOpenPolling'),
 ('pending tail', 'libs/core/capture/RawCapture.cpp',
  '!pendingTailAllowed && hasFollowingFraming', 'hasFollowingFraming', 'Roller.PendingFramingDeferredUntilSealedOrSuperseded'),
 ('record ticks', 'libs/core/roller/JournalFeed.cpp',
  'if (onTick) onTick(local);', 'if (false && onTick) onTick(local);', 'Roller.FeedFiltersProductsClocksAndLifecycle'),
 ('deterministic resume', 'libs/core/roller/Grid.cpp',
  'c.blockingQueue = c.deterministicResume = true;', 'c.blockingQueue = true; c.deterministicResume = false;', 'Roller.RunIdentityCrashResumeAndIdempotence'),
 ('checkpoint', 'libs/core/roller/Roller.cpp',
  'checkpoint(cpPath,cp);', '// checkpoint disabled by mutation', 'Roller.RunIdentityCrashResumeAndIdempotence'),
 ('dry run', 'libs/core/roller/Roller.cpp',
  'if (o.dryRun) return;', 'if (false) return;', 'Roller.DryRunAndEofDoNotInventTime'),
 ('blocking admission', 'libs/core/roller/Grid.cpp',
  'c.blockingQueue = c.deterministicResume = true;', 'c.blockingQueue = false; c.deterministicResume = true;', 'Roller.BlockingQueueAdmitsAtomicOversizedSnapshot'),
 ('commit floor', 'libs/core/servermodel/BookRecorder.cpp',
  '<= cfg.commitFloorMs', '<= 0', 'Roller.CommitFloorAndCeiling'),
 ('lateness mask', 'libs/core/roller/Diff.cpp',
  'r.flags & ~recording::kLateEvents', 'r.flags', 'Roller.DiffQualifiesMasksLateAndDetectsContent'),
 ('shared parsing/live golden', 'libs/core/marketdata/dispatch/BookParser.hpp',
  'return std::isfinite(price)', 'quantity *= 1.01; return std::isfinite(price)', 'RollerLiveFixture.RecordedInputPreservesLiveBytes'),
 ('timestamp fallback', 'libs/core/Cpp20Utils.hpp',
  'return fallback ? *fallback : std::chrono::system_clock::now();',
  'return std::chrono::system_clock::now();', 'Roller.FeedFiltersProductsClocksAndLifecycle'),
]
mutations += review_mutations
if len(sys.argv) > 1:
    mutations = review_mutations if sys.argv[1] == '--review' else [m for m in mutations if m[0] == sys.argv[1]]
    if not mutations: raise RuntimeError('unknown mutation')
build()
command(['ctest','--test-dir','build/mac-clang','-R','^Roller','--output-on-failure'])
for label, filename, before, after, case in mutations:
    path = Path(filename)
    original = path.read_text()
    if before not in original:
        raise RuntimeError('mutation seam missing: ' + label)
    try:
        path.write_text(original.replace(before,after))
        os.utime(path,None)
        build()
        test(case,True)
        print('FAIL-WITHOUT: ' + label + ' -> ' + case, flush=True)
    finally:
        path.write_text(original)
        os.utime(path,None)
        build()
    test(case)
    print('RESTORED PASS: ' + label, flush=True)
print(command(['ctest','--test-dir','build/mac-clang','-R','^Roller','--output-on-failure']), flush=True)
print(f'{len(mutations)}/{len(mutations)} mutations failed as expected; restored suites green',flush=True)
