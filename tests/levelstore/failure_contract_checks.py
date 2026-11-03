#!/usr/bin/env python3
"""E1b-1 failure-contract evidence. Run through the main checkout's FIFO build queue.

Modes: baseline checks each named failure regression against 6e76222b; mutations
reuses M1-M8 from the original evidence harness plus M37 (suspended-book clear).
Requires a clean committed worktree and a NEW evidence directory. Restores only
owned production files from the starting commit, touches them, rebuilds, and runs
the three focused suites after each attempt. Never reads or changes recordings.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

SUITES = {
    'test_level_recorder': 'levelstore/test_level_recorder',
    'test_level_roll': 'levelstore/test_level_roll',
    'test_recorder_sink': 'servermodel/test_recorder_sink',
}
BASELINE = {
    'test_level_recorder': [
        'Checkpoint.ARefusedRestorePermanentlyStopsTheAttempt',
        'Failure.ImageAndBlockFailuresStopBeforeAnyLaterPublication',
        'Failure.AClockRegressionFailsClosedAndForeignSymbolsAreIgnored',
        'Failure.AFailedClearMinuteCannotPublishLaterPhantomLevels',
        'Failure.ExplicitReplayFromPairedStateAndOutputPrefixMatchesUninterruptedHistory',
        'Failure.AnInconsistentMinutePermanentlyStopsOutput',
        'Failure.AllocationFailureStopsInsteadOfCreatingARepairClear',
    ],
    'test_level_roll': [
        'LevelRoll.OutputFailureAbortsWithoutReportingSuccessfulProgress',
        'LevelRoll.InternalFailureCannotBeSwallowedAsAnOptionalAnchorFailure',
        'LevelRoll.DurableResumeWithoutPairedCStateIsRefusedBeforeOutput',
    ],
}
WITNESSES = {
    'M1': ('test_level_recorder', 'MessageStates.ZeroDurationTinyAndShortLivedLevelsAndMessageAtomicPeaks'),
    'M2': ('test_level_recorder', 'UnsupportedPrice.EpisodeKeepsEarlierDataAndRecoversOnlyAtAValidExactSnapshot'),
    'M3': ('test_level_recorder', 'Checkpoint.BindsToTheRecorderClockAndStateNotToCsLastEvent'),
    'M4': ('test_level_roll', 'LevelRoll.DayRecreationAndInProcessRestoresMatchTheUninterruptedRun'),
    'M5': ('test_level_recorder', 'Lifecycle.WarmupBuildsTheBookAndTheDayStartsAtTheFloor'),
    'M6': ('test_level_recorder', 'Checkpoint.RestoreAfterEveryMessageIsExactlyTheUninterruptedRun'),
    'M7': ('test_level_recorder', 'Checkpoint.CorruptVersionedForeignAndUnpairedBlobsAreRefusedAndFailClosed'),
    'M8': ('test_level_recorder', 'UnsupportedPrice.EpisodeKeepsEarlierDataAndRecoversOnlyAtAValidExactSnapshot'),
    'M37': ('test_level_recorder', 'Lifecycle.SuspendedBookInvalidationPublishesAClearAcrossRestore'),
}
BASE_FILES = [f'libs/core/levelstore/{name}' for name in (
    'LevelRecorder.cpp', 'LevelRecorder.hpp', 'LevelTimeline.cpp',
    'LevelTimeline.hpp', 'MinuteSweep.cpp', 'MinuteSweep.hpp',
)] + ['libs/core/roller/Roller.cpp', 'libs/core/roller/Roller.hpp']


def require(condition, reason):
    if not condition:
        raise RuntimeError(reason)


def validate_report(document, expected=None, failures=()):
    """Require complete named test results, including every expected assertion failure."""
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


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


class Check:
    def __init__(self, args):
        self.root = args.worktree.resolve()
        self.main = args.main_checkout.resolve()
        self.branch = args.branch
        self.out = args.evidence.resolve()
        self.out.mkdir(parents=True, exist_ok=False)
        self.env = os.environ.copy()
        self.env['TMPDIR'] = str(self.out / 'tmp')
        Path(self.env['TMPDIR']).mkdir()
        self.env['CCACHE_DIR'] = '/tmp/sentinel-c-e1b1-recovery-ccache'
        require(not self.git('status', '--porcelain').strip(), 'worktree must be clean')
        self.tip = self.git('rev-parse', 'HEAD').strip()
        self.summary = {'tip': self.tip, 'mode': args.mode, 'attempts': [], 'pass': False}

    def git(self, *args):
        checked = subprocess.check_output([
            str(self.main / 'scripts/dev/agent-worktree.sh'), 'verify-gitdir', self.branch,
        ], cwd=self.main, text=True).strip()
        require(Path(checked).resolve() == self.root, 'verified worktree differs')
        return subprocess.check_output(['git', *args], cwd=self.root, text=True)

    def command(self, name, args, timeout):
        with (self.out / (name + '.log')).open('x') as log:
            return subprocess.run(args, cwd=self.root, env=self.env, stdout=log,
                                  stderr=subprocess.STDOUT, timeout=timeout).returncode

    def build(self, name):
        require(self.command(name, ['cmake', '--build', '--preset', 'mac-clang', '-j', '6',
                                    '--target', *SUITES], 1200) == 0, 'build failed: ' + name)

    def suite(self, label, target, expected=None, failures=()):
        binary = self.root / 'build/mac-clang/tests' / SUITES[target]
        report = self.out / (label + '.json')
        require(not report.exists(), 'refuse stale JSON: ' + str(report))
        command = [str(binary), '--gtest_output=json:' + str(report)]
        if expected is not None:
            command.append('--gtest_filter=' + ':'.join(expected))
        code = self.command(label, command, 180)
        require(code in (0, 1), f'{label}: abnormal exit {code}')
        result = validate_report(json.loads(report.read_text()), expected, failures)
        require(code == bool(result['failed']), 'exit status disagrees with structured results')
        return dict(result, exit=code, binarySha256=sha(binary))

    def save(self):
        (self.out / 'summary.json').write_text(json.dumps(self.summary, indent=2) + '\n')

    def attempt(self, name, description, patches=None):
        files = sorted({p[0] for p in patches}) if patches else BASE_FILES
        original = {f: sha(self.root / f) for f in files}
        result = {'id': name, 'description': description, 'originalSourceSha256': original}
        self.summary['attempts'].append(result)
        print('START ' + name, flush=True)
        try:
            if patches:
                for file, before, after in patches:
                    path = self.root / file
                    text = path.read_text()
                    require(text.count(before) == 1, f'{name}: mutation anchor absent/ambiguous in {file}')
                    path.write_text(text.replace(before, after))
            else:
                for file in files:
                    (self.root / file).write_text(self.git('show', '6e76222b:' + file))
            result['mutatedSourceSha256'] = {f: sha(self.root / f) for f in files}
            (self.out / (name + '.diff')).write_text(self.git('diff', '--', *files))
            self.build(name + '-build')
            cases = BASELINE if patches is None else {WITNESSES[name][0]: [WITNESSES[name][1]]}
            result['mutated'] = {target: self.suite(name + '-' + target, target, tests, tests)
                                 for target, tests in cases.items()}
        finally:
            # Restore from the committed tip, then force fresh mtimes (FM-132).
            self.git('restore', '--source=' + self.tip, '--', *files)
            for file in files:
                os.utime(self.root / file, None)
            result['restoredSourceSha256'] = {f: sha(self.root / f) for f in files}
            require(result['restoredSourceSha256'] == original, 'source restoration differs')
            require(not self.git('status', '--porcelain').strip(), 'tree not clean after restore')
            self.build(name + '-restored-build')
            result['restored'] = {t: self.suite(name + '-restored-' + t, t) for t in SUITES}
            self.save()
        result['pass'] = True
        self.save()
        print('PASS ' + name, flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=['baseline', 'mutations'])
    parser.add_argument('--worktree', type=Path, required=True)
    parser.add_argument('--main-checkout', type=Path, required=True)
    parser.add_argument('--branch', required=True)
    parser.add_argument('--evidence', type=Path, required=True)
    args = parser.parse_args()
    check = Check(args)
    if args.mode == 'baseline':
        check.attempt('baseline', 'every selected failure regression must fail against 6e76222b')
    else:
        for name, (description, patches) in MUTATIONS.items():
            check.attempt(name, description, patches)
    check.summary['pass'] = True
    check.save()
    print('ALL CHECKS PASS', flush=True)
MUTATIONS = {'M1': ('drop zero-duration states: a message in the same millisecond merges into the previous one',
        [('libs/core/levelstore/LevelRecorder.cpp',
          '    const auto message = static_cast<uint32_t>(open_.messages.size());',
          '    const bool sameMs = !open_.messages.empty() && open_.messages.back().timeMs == clock && !reset &&\n'
          '                        open_.messages.back().clear == ClearCause::None && '
          'open_.messages.back().observed == observed_;\n'
          '    const auto message = static_cast<uint32_t>(open_.messages.size() - (sameMs ? 1 : 0));'),
         ('libs/core/levelstore/LevelRecorder.cpp',
          '    open_.messages.push_back({clock, observed_, reset, ClearCause::None});',
          '    if (!sameMs) open_.messages.push_back({clock, observed_, reset, ClearCause::None});')]),
 'M2': ('round an off-scale decimal instead of failing (Finer accepted at the rounded key)',
        [('libs/core/levelstore/LevelRecorder.cpp',
          '        if (level.identity == DecimalIdentity::Exact) return; // the double',
          '        if (level.identity == DecimalIdentity::Exact || level.identity == DecimalIdentity::Finer) '
          'return; // the double'),
         ('libs/core/levelstore/LevelRecorder.cpp',
          '        if (level.identity != DecimalIdentity::Exact) { // counted',
          '        if (level.identity != DecimalIdentity::Exact && level.identity != DecimalIdentity::Finer) { // '
          'counted')]),
 'M3': ('export without accruing to the recorder clock (C checkpoints its own last event clock)',
        [('libs/core/levelstore/LevelRecorder.cpp',
          '    w.svarint(recorderClock.value_or(0));',
          '    w.svarint(lastClock_ == INT64_MIN ? recorderClock.value_or(0) : lastClock_);')]),
 'M4': ('import after replay resumes (C imports after the next journal record)',
        [('libs/core/roller/Roller.cpp',
          '            if (freshLevel) imported = '
          'freshLevel->importState(levelBytes,fresh->checkpointClock(o.product),recorderBytes);',
          '            if (freshLevel) deferredImport = [levelBytes, recorderBytes, &recorder, &level, &o] {\n'
          '                recorder->drain();\n'
          '                level->importState(levelBytes,recorder->checkpointClock(o.product),recorderBytes);\n'
          '            };'),
         ('libs/core/roller/Roller.cpp',
          '        const auto restoreInProcess = [&](const std::vector<uint8_t>& recorderBytes, int64_t boundary) '
          '{',
          '        std::function<void()> deferredImport;\n'
          '        const auto restoreInProcess = [&](const std::vector<uint8_t>& recorderBytes, int64_t boundary) '
          '{'),
         ('libs/core/roller/Roller.cpp',
          '            feed.apply(input); applied = input.pos; ++records;',
          '            feed.apply(input); applied = input.pos; ++records;\n'
          '            if (deferredImport) { deferredImport(); deferredImport = {}; }')]),
 'M5': ('lose warmup state at day recreation',
        [('libs/core/levelstore/LevelRecorder.cpp',
          'void LevelRecorder::startDay() {',
          'void LevelRecorder::startDay() {\n    clearBook();')]),
 'M6': ('ignore an invalid period on restore (the restored C is valid and observing)',
        [('libs/core/levelstore/LevelRecorder.cpp',
          '        valid_ = valid;\n'
          '        recorderObserving_ = recorderObserving;\n'
          '        observed_ = observed;',
          '        valid_ = true;\n'
          '        recorderObserving_ = true;\n'
          '        observed_ = unobserved == Unobserved::None;')]),
 'M7': ('accept a corrupt blob (no CRC check on import)',
        [('libs/core/levelstore/LevelRecorder.cpp',
          '        require(crc == sentinel::crc32(blob.data(), blob.size() - 4), "state CRC mismatch");',
          '        (void)crc;')]),
 'M8': ('recover at any snapshot, including invalid or imprecise snapshots',
        [('libs/core/levelstore/LevelRecorder.cpp',
          '    if (messageUnsupported_) {',
          '    if (messageUnsupported_ && !messageReset_) {'),
         ('libs/core/levelstore/LevelRecorder.cpp',
          '    } else if (invalidated) {',
          '    } else if (invalidated && !messageReset_) {'),
         ('libs/core/levelstore/LevelRecorder.cpp',
          '        if (messageReset_ && valid) {',
          '        if (messageReset_) {')]),
 'M37': ('omit the clear for a suspended book with retained levels',
         [('libs/core/levelstore/LevelRecorder.cpp',
           'dayStarted_ && (wasObserved || hadLevels)',
           'dayStarted_ && wasObserved')])}

if __name__ == '__main__':
    main()
