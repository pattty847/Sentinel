#!/usr/bin/env python3
"""Run inside build-queue; mutations restore, touch, rebuild and re-pass (FM-132)."""
import os
import re
import sys
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
os.chdir(ROOT)
os.environ['CCACHE_DIR'] = '/tmp/sentinel-ccache'

def command(args):
    return subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

def build():
    p = command(['cmake', '--build', '--preset', 'mac-clang', '-j', '4', '--target', 'test_shadow_roller'])
    if p.returncode:
        raise RuntimeError(p.stdout[-10000:])

def test(name, fail=False):
    p = command(['build/mac-clang/tests/roller/test_shadow_roller', '--gtest_filter=' + name])
    if (p.returncode != (1 if fail else 0)) or (fail and '[  FAILED  ]' not in p.stdout):
        raise RuntimeError(p.stdout[-12000:])

mutations = [
    ('durable admission', 'libs/core/roller/ShadowRoller.cpp',
     'if(!pending.empty() && ceiling && older(pending.front().pos,*ceiling))',
     'if(!pending.empty())',
     'ShadowTest.RetractNeverAppliesOrCheckpointsProvisionalSuffix'),
    ('disk ceiling', 'libs/core/roller/ShadowRoller.cpp',
     'if(!older(out.pos,*diskTarget))', 'if(false)',
     'ShadowTest.NeverCrossesHandshakeDurableCeilingWhenAnchorIsNewer'),
    ('missing strict buckets', 'libs/core/roller/Diff.cpp',
     'else if (strictJournal && (l.contains(t) != r.contains(t)))', 'else if (false)',
     'ShadowTest.MismatchMetricIsStrictAndCrossConnectionInformational'),
    ('duplicate overlap', 'libs/core/roller/ShadowRoller.cpp',
     'if(applied && older(p,*applied)) return;', 'if(false) return;',
     'ShadowTest.CatchupJoinsSocketOverlapExactlyOnce'),
    ('primary root isolation', 'libs/core/roller/ShadowRoller.cpp',
     'overlaps(cfg.outputRoot,primary) || overlaps(cfg.outputRoot,cfg.journalRoot)',
     'false', 'ShadowTest.RefusesAliasedPrimaryRoot'),
    ('stop wait synchronization', 'libs/core/roller/ShadowRoller.cpp',
     'std::lock_guard lock(mutex); stopping = true;', 'stopping = true;',
     'ShadowTest.StopWakesCheckerAcrossPredicateWaitTransition'),
    ('comparison watermark restore', 'libs/core/roller/ShadowRoller.cpp',
     'p->compared = saved.at("comparedThroughMs").get<int64_t>();', 'p->compared = first;',
     'ShadowTest.ComparisonWatermarkAndMismatchTotalsSurviveRestart'),
    ('malformed journal invalidation', 'libs/core/roller/ShadowRoller.cpp',
     'o.onInvalid = [&](const std::string &reason) {',
     'o.onInvalid = [&](const std::string &reason) { if (reason.starts_with("malformed")) throw std::logic_error(reason);',
     'ShadowTest.MalformedRecordInvalidatesAndContinuesLikeBatch'),
    ('persistent fault cooldown', 'libs/core/roller/ShadowRoller.cpp',
     'consecutive >= cfg.failureThreshold ? cfg.failureCooldown : backoff', 'backoff',
     'ShadowTest.PersistentWriteFaultEntersCooldownAndRecovers'),
    ('transport retains applied book state', 'libs/core/roller/ShadowRoller.cpp',
     'if (!applied) throw;', 'throw;',
     'ShadowTest.JournalUnavailableResumesAppliedCursorWithoutReplay'),
    ('server shadow startup independence', 'apps/sentinel-server/SentinelServerApp.cpp',
     'm_serverModel->recordingDir().value_or(m_serverConfig.recording.dir), m_metrics);',
     'm_serverModel->recordingDir().value_or(m_serverConfig.recording.dir), m_metrics); m_shadowRoller->stop();',
     'ShadowTest.StalledAndFailingShadowDoesNotDelayServerPrimary'),
    ('missing comparison checkpoint is not a fresh audit', 'libs/core/roller/ShadowRoller.cpp',
     'if (p->comparisonCheckpointSeen)', 'if (false)',
     'ShadowTest.ComparisonWatermarkAndMismatchTotalsSurviveRestart'),
]
if "--round1" in sys.argv:
    mutations = mutations[5:]
for name, path, before, after, case in mutations:
    p = ROOT / path
    original = p.read_text()
    # Match tokens across clang-format whitespace, retaining exact operators,
    # identifiers and string contents. Refuse an ambiguous or absent anchor.
    anchor = re.compile(r'\s*'.join(re.escape(ch) for ch in ''.join(before.split())))
    matches = list(anchor.finditer(original))
    if len(matches) != 1:
        raise RuntimeError(f'{name}: mutation anchor count is not one')
    hit = matches[0]
    try:
        p.write_text(original[:hit.start()] + after + original[hit.end():])
        os.utime(p, None)
        build()
        test(case, fail=True)
        print(f'FAIL-WITHOUT: {name}: expected assertion failure', flush=True)
    finally:
        p.write_text(original)
        os.utime(p, None)
        build()
    test(case)
    print(f'RESTORED: {name}: passed', flush=True)
print(f'SHADOW_MUTATIONS: {len(mutations)}/{len(mutations)} fail-without and restored checks passed', flush=True)
