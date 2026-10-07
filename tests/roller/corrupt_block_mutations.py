#!/usr/bin/env python3
"""Mac dev tool. Run inside build-queue.sh; restore/rebuild each mutant before proceeding."""
import os
from pathlib import Path
import subprocess
import sys

os.chdir(Path(__file__).resolve().parents[2])
os.environ['CCACHE_DIR'] = '/tmp/sentinel-corrupt-skip-ccache'
# Do not reuse an object from a future-dated restoration (FM-132).
os.environ['CCACHE_DISABLE'] = '1'
raw = 'libs/core/capture/RawCapture.cpp'
journal = 'libs/core/roller/JournalReader.cpp'
mutations = [
    ('CRC recovery', raw, 'if (damage) {',
     'if (damage && std::string_view(damage) == "block CRC mismatch") fail(damage);\n        if (damage) {',
     'test_roller_corrupt_blocks', '*GapRecoveryMetricAuditAndShadowParity/0:*RealHourCopyRecoversAfterExchangeSnapshot/0'),
    ('zstd recovery', raw, 'if (damage) {',
     'if (damage && std::string_view(damage).find("zstd") != std::string_view::npos) fail(damage);\n        if (damage) {',
     'test_roller_corrupt_blocks', '*GapRecoveryMetricAuditAndShadowParity/1:*RealHourCopyRecoversAfterExchangeSnapshot/1'),
    ('gap invalidation', journal, '                gap_ = true;', '                gap_ = false;',
     'test_roller_corrupt_blocks', '*GapRecoveryMetricAuditAndShadowParity/0'),
    ('corruption counter', journal, '        state->count.inc();', '        // mutation: omit counter',
     'test_roller_corrupt_blocks', '*GapRecoveryMetricAuditAndShadowParity/0'),
    ('daily anchor recovery', journal,
     '                reportCorruption(product_, *f, block, reason);',
     '                throw std::runtime_error(reason);',
     'test_roller_corrupt_blocks', '*FirstDayCompletesAndNextDayRolls/0'),
    ('shadow parity/progress', raw, 'if (damage) {',
     'if (damage) fail(damage);\n        if (damage) {',
     'test_roller_corrupt_blocks', 'CorruptBlockPolicy.ShadowWorkerCrossesDamagedDayWithoutRetry'),
    ('segment gap carry', journal,
     'gap_ = gap_ || result.tornTail', 'gap_ = result.tornTail',
     'test_roller_corrupt_blocks', '*GapSurvivesSegmentBoundaryAndLostCursor/0'),
    ('strict audit', raw,
     '            if (!onCorruptBlock) fail(damage);\n            onCorruptBlock(entry, damage);',
     '            if (onCorruptBlock) onCorruptBlock(entry, damage);',
     'test_roller_corrupt_blocks', '*GapRecoveryMetricAuditAndShadowParity/0'),
    ('pending tail', raw,
     '!pendingTailAllowed && hasFollowingFraming', 'hasFollowingFraming',
     'test_roller', 'Roller.PendingFramingDeferredUntilSealedOrSuperseded'),
    ('header recovery', raw, '            if (headerDamaged) {',
     '            if (headerDamaged && magic == "BLK1") fail("mutated header recovery");\n            if (headerDamaged) {',
     'test_roller_corrupt_blocks', 'CorruptFraming.HeaderDamageCompletesDay'),
    ('magic recovery', raw, '            if (headerDamaged) {',
     '            if (headerDamaged && magic != "BLK1" && magic != "IDX1") fail("mutated magic recovery");\n            if (headerDamaged) {',
     'test_roller_corrupt_blocks', 'CorruptFraming.MagicDamageCompletesDay'),
    ('index recovery', raw, '            if (headerDamaged) {',
     '            if (headerDamaged && magic == "IDX1") fail("mutated index recovery");\n            if (headerDamaged) {',
     'test_roller_corrupt_blocks', 'CorruptFraming.IndexDamageCompletesDay'),
    ('last header recovery', raw, '            if (headerDamaged) {',
     '            if (headerDamaged && indexed && result.index.size()+1 == sealed->entries.size()) fail("mutated last header recovery");\n            if (headerDamaged) {',
     'test_roller_corrupt_blocks', 'CorruptFraming.LastHeaderDamageCompletesDay'),
    ('exact successor ordinal', raw, 'if (!recoveryLimits(e) || e.ordinal != ordinal) return {};',
     'if (!recoveryLimits(e)) return {};',
     'test_roller_corrupt_blocks', 'CorruptFraming.SuccessorRequiresCrcLimitsAndExactOrdinal'),
    ('skipped payload extent', raw,
     'result.index.push_back(entry); ++nextOrdinal;\n            result.validBytes = file.pos();\n            continue;',
     'result.index.push_back(entry); ++nextOrdinal;\n            continue;',
     'test_roller_corrupt_blocks', 'CorruptFraming.SkippedPayloadAdvancesValidBytes'),
    ('idempotent registration', journal,
     'if (registry.hasSeries("sentinel_roller_journal_corrupt_blocks_total", {{"product", product}})) return;',
     'if (false) return;',
     'test_roller_corrupt_blocks', 'CorruptBlockPolicy.MetricRegistrationSurvivesRepeatedAndReusedRegistries'),
    ('self resolving alert', 'ops/monitoring/grafana/provisioning/alerting/rules.yaml',
     'sum by (product) (increase(sentinel_roller_journal_corrupt_blocks_total{job="sentinel-server"}[2h]))',
     'max by (product) (sentinel_roller_journal_corrupt_blocks_total{job="sentinel-server"})',
     'test_roller_corrupt_blocks', 'CorruptBlockPolicy.AlertResolvesAfterTwoHoursWithoutNewCorruption'),
]
if len(sys.argv) > 1:
    mutations = [m for m in mutations if m[0] == sys.argv[1]]
    if not mutations:
        raise SystemExit('unknown mutation')

def run(args):
    return subprocess.run(args, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

def build(target):
    result = run(['cmake', '--build', '--preset', 'mac-clang', '-j', '2', '--target', target])
    if result.returncode:
        print(result.stdout[-10000:], flush=True)
        raise RuntimeError('build failed')

for label, filename, before, after, target, case in mutations:
    path = Path(filename)
    original = path.read_text()
    assert original.count(before) == 1, (label, original.count(before))
    try:
        path.write_text(original.replace(before, after))
        build(target)
        result = run([f'build/mac-clang/tests/roller/{target}', '--gtest_filter='+case])
        if result.returncode != 1 or '[  FAILED  ]' not in result.stdout:
            print(result.stdout[-10000:], flush=True)
            raise RuntimeError('mutation survived or failed outside assertions: '+label)
        print('KILLED: '+label+' -> '+case, flush=True)
    finally:
        path.write_text(original)
        os.utime(path, None)
        build(target)
    result = run([f'build/mac-clang/tests/roller/{target}', '--gtest_filter='+case])
    if result.returncode:
        print(result.stdout[-10000:], flush=True)
        raise RuntimeError('restored baseline failed: '+label)
    print('RESTORED PASS: '+label, flush=True)
