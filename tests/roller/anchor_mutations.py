#!/usr/bin/env python3
"""Run through build-queue. Each mutant must fail assertions, then restore/rebuild/pass."""
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
os.chdir(ROOT)
os.environ.setdefault("CCACHE_DIR", "/tmp/sentinel-ccache")
TARGET = "test_journal_anchors"
STORE = "libs/core/roller/AnchorStore.cpp"
RECORDER = "libs/core/servermodel/BookRecorder.cpp"
SEEK = "libs/core/roller/JournalReader.cpp"
ROLLER = "libs/core/roller/Roller.cpp"
FEED = "libs/core/roller/JournalFeed.cpp"
MUTATIONS = [
    ("sidecar CRC", STORE,
     'require(get(bytes,bytes.size()-4) == hmcol::crc32(bytes.data(),bytes.size()-4), "CRC mismatch");',
     '', 'AnchorStore.RejectsCorruptionVersionsIdentityAndFuturePositions'),
    ("sidecar policy and range", STORE,
     'require(a.identity == expected, "policy/range identity mismatch");',
     '', 'AnchorStore.RejectsCorruptionVersionsIdentityAndFuturePositions'),
    ("lateness tail", RECORDER,
     'v.pending.push_back(std::move(out));',
     '', 'RecorderState.SyntheticContinuationPreservesEveryFieldAndOutput'),
    ("hour accumulator", RECORDER,
     'l.hourMinutes.push_back(std::move(out));',
     '', 'RecorderState.SyntheticContinuationPreservesEveryFieldAndOutput'),
    ("lossless floating point", RECORDER,
     'std::setprecision(std::numeric_limits<long double>::max_digits10)',
     'std::setprecision(6)', 'RecorderState.AccumulatorsRoundTripWithoutRounding'),
    ("envelope clock offset", RECORDER,
     'j.at("offset").get_to(v.offset);',
     'v.offset = 0;', 'RecorderState.SyntheticContinuationPreservesEveryFieldAndOutput'),
    ("no synthetic resync", RECORDER,
     'j.at("flags").get_to(v.flags);\n    j.at("lastPublish").get_to(v.lastPublish);',
     'j.at("flags").get_to(v.flags); v.flags |= kResynced;\n    j.at("lastPublish").get_to(v.lastPublish);',
     'RecorderState.SyntheticContinuationPreservesEveryFieldAndOutput'),
    ("bounded record decoding", SEEK,
     'start_ ? std::optional<uint64_t>(start_->block) : std::nullopt',
     'std::nullopt', 'JournalSeek.IndexedAndUnsealedDecodeOnlySelectedBlockAndSuffix'),
    ("hour boundary persisted-minute coverage", RECORDER,
     '(e.coveredMs == 0 || e.coveredMs == v.observedMs)',
     '(e.coveredMs == 0)', 'AnchorReplay.EveryQuarterBoundedResumeAndIndependentMidnight'),
    ("feed continuity", FEED,
     'sequence_ = sequence; anchored_ = anchored;',
     'sequence_.reset(); anchored_ = false;', 'FeedState.ImportHasNoCallbacksAndPreservesSequenceAndInvalidation'),
    ("quarter production and midnight provenance", ROLLER,
     'const auto written = AnchorStore::write(anchorRoot,a);',
     'const auto written = AnchorStore::WriteStats{};', 'AnchorReplay.EveryQuarterBoundedResumeAndIndependentMidnight'),
    ("bounded restart uses cached recorder", ROLLER,
     'if (o.useAnchors && !o.dryRun) for',
     'if (false) for', 'AnchorReplay.EveryQuarterBoundedResumeAndIndependentMidnight'),
    ("newest qualifying anchor", STORE,
     'std::sort(out.rbegin(),out.rend());',
     'std::sort(out.begin(),out.end());', 'AnchorReplay.EveryQuarterBoundedResumeAndIndependentMidnight'),
    ("bounded sidecar decoding", ROLLER,
     'if (!time.isValid() || day + time.msecsSinceStartOfDay() > targetBoundary) continue;',
     'if (!time.isValid()) continue;', 'AnchorReplay.EveryQuarterBoundedResumeAndIndependentMidnight'),
    ("late-record midnight independence", ROLLER,
     'if (recorder && nextBoundary == day && input.record.time.systemNs / 1\'000\'000 >= day)',
     'if (false)', 'AnchorReplay.LateFirstRecordKeepsMidnightIndependent'),
    ("bounded lateness tail at range end", ROLLER,
     'if (minute != lastFence || tailFence)',
     'if (minute != lastFence)', 'AnchorReplay.EveryQuarterBoundedResumeAndIndependentMidnight'),
    ("damaged committed overlap falls back", ROLLER,
     '!present || r.gapBefore || r.pos.run != checkpoint->run ||',
     '!present || r.pos.run != checkpoint->run ||', 'AnchorReplay.DamagedCommittedOverlapUsesSnapshotFallback'),
    ("checkpoint ceiling", ROLLER,
     'if (cursorPos.run != checkpoint->run || std::pair(cursorPos.block,cursorPos.record) >\n                            std::pair(checkpoint->block,checkpoint->record)) continue;',
     'checkpoint = cursorPos;', 'AnchorReplay.AnchorAheadOfCheckpointOrHandshakeIsNotInstalled'),
    ("handshake ceiling", ROLLER,
     'if (o.anchorAllowed && !o.anchorAllowed(cursorPos)) continue;',
     ';', 'AnchorReplay.AnchorAheadOfCheckpointOrHandshakeIsNotInstalled'),
    ("fallback on incompatible policy", ROLLER,
     'if (a.identity != expected) throw std::runtime_error("anchor policy/range mismatch");',
     ';', 'AnchorReplay.CorruptMissingAndAheadCandidatesFallbackIdentically'),
    ("retention uses actual durable time", ROLLER,
     'AnchorStore::prune(anchorRoot,o.product,receiveThrough);',
     'AnchorStore::prune(anchorRoot,o.product,end);', 'AnchorReplay.CancelledRangeDoesNotPruneFutureTime'),
    ("midnight retention", STORE,
     'name == "0000.anchor" ||',
     '', 'AnchorStore.RetentionKeepsMidnightAndTouchesOnlyExpiredIntraday'),
    ("rebuild produces sidecars", 'libs/core/roller/RollCli.cpp',
     'o.useAnchors = false; o.writeAnchors = true;',
     'o.useAnchors = false; o.writeAnchors = false;', 'AnchorReplay.CliRebuildCreatesOnlySidecarsAndMatchesNormalExport'),
    ("cache write failure never aborts primary", ROLLER,
     'if (o.anchorFailuresFatal) throw std::runtime_error(',
     'if (true) throw std::runtime_error(', 'AnchorReplay.UnwritableLiveFixtureCacheDoesNotAbort'),
    ("cache failure metric", ROLLER,
     'anchorFailureCounter->inc();', ';', 'AnchorReplay.UnwritableLiveFixtureCacheDoesNotAbort'),
    ("cache failure warning", ROLLER,
     'if (warn) {', 'if (false) {', 'AnchorReplay.UnwritableLiveFixtureCacheDoesNotAbort'),
    ("degraded export is skipped", RECORDER,
     'if (src.diskErrors || src.queueDrops) return {};',
     'stateRequire(!src.diskErrors && !src.queueDrops, "cannot anchor failed recorder");',
     'AnchorReplay.ExportExceptionsAndDegradedStateAreCacheMisses'),
    ("degraded warning only once", ROLLER,
     'const bool warn = !degraded || !warnedDegraded;',
     'const bool warn = true;', 'AnchorReplay.ExportExceptionsAndDegradedStateAreCacheMisses'),
    ("rebuild cache failures are fatal", 'libs/core/roller/RollCli.cpp',
     'o.anchorFailuresFatal = true;', 'o.anchorFailuresFatal = false;', 'AnchorReplay.WriteTimingAndStrictRebuildFailures'),
    ("boundary timing visibility", ROLLER,
     ' exportMs=', ' omittedMs=', 'AnchorReplay.WriteTimingAndStrictRebuildFailures'),
    ("throwing overlap stops older retries", ROLLER,
     'rejectedPrefix = true; // any reader/retention exception makes older candidates redundant',
     '; // injected unbounded older-candidate retries', 'AnchorReplay.ThrowingOverlapReaderRejectsOlderCandidates'),
    ("prune errors remain observable", ROLLER,
     'if (error) throw std::system_error(error,"anchor prune");',
     ';', 'AnchorReplay.PruneFailureIsNonFatalUnlessRebuilding'),
    ("abandoned QSaveFile cleanup", STORE,
     "if (name.size() <= 12 || name[11] != '.') continue;",
     'continue;', 'AnchorStore.PruneOnlyStaleRecognizedTemporariesAndReportErrors'),
    ("fresh QSaveFile preservation", STORE,
     'if (ec || modified >= staleBefore) continue;',
     'if (ec) continue;', 'AnchorStore.PruneOnlyStaleRecognizedTemporariesAndReportErrors'),


]

def run(args):
    return subprocess.run(args, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)


def build():
    p = run(['cmake', '--build', '--preset', 'mac-clang', '-j', '2', '--target', TARGET])
    if p.returncode:
        raise RuntimeError('Mutation build failed (not a kill):\n' + p.stdout[-8000:])


def test(case, killed=False):
    p = run([str(ROOT/'build/mac-clang/tests/roller'/TARGET), '--gtest_filter='+case])
    if killed:
        if p.returncode != 1 or '[  FAILED  ]' not in p.stdout:
            raise RuntimeError('Mutant survived or failed outside assertions:\n' + p.stdout[-8000:])
    elif p.returncode:
        raise RuntimeError('Restored test failed:\n' + p.stdout[-8000:])


for name, file, before, after, case in MUTATIONS:
    path = ROOT/file
    original = path.read_text()
    if original.count(before) != 1:
        raise RuntimeError(f'{name}: mutation anchor not unique')
    try:
        path.write_text(original.replace(before, after, 1))
        build()
        test(case, killed=True)
        print(f'KILLED: {name}: assertion failure', flush=True)
    finally:
        path.write_text(original)
        build()
    test(case)
    print(f'RESTORED: {name}: PASS', flush=True)
print(f'ANCHOR_MUTATIONS: {len(MUTATIONS)}/{len(MUTATIONS)} killed; restored checks passed', flush=True)
