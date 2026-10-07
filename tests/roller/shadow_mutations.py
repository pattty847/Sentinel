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
SHADOW, ROLLER = 'test_shadow_roller', 'test_roller'

def command(args):
    return subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

def build(target):
    if target is None:
        return
    # -j 2: the owner's Mac froze under heavier load (AGENTS section 4).
    p = command(['cmake', '--build', '--preset', 'mac-clang', '-j', '2', '--target', target.split('/')[-1]])
    if p.returncode:
        raise RuntimeError(p.stdout[-10000:])

def test(name, target, fail=False):
    if name.startswith('sh:'):
        p = command(['bash', name[3:], str(ROOT)])
        if p.returncode != (1 if fail else 0):
            raise RuntimeError(p.stdout[-12000:])
        return
    exe = target if '/' in target else f'roller/{target}'
    p = command([f'build/mac-clang/tests/{exe}', '--gtest_filter=' + name])
    if (p.returncode != (1 if fail else 0)) or (fail and '[  FAILED  ]' not in p.stdout):
        raise RuntimeError(p.stdout[-12000:])

SR = 'libs/core/roller/ShadowRoller.cpp'
BR = 'libs/core/servermodel/BookRecorder.cpp'
APP = 'apps/sentinel-server/SentinelServerApp.cpp'
mutations = [
    ('durable admission', SR,
     'if(!pending.empty() && ceiling && older(pending.front().pos,*ceiling))',
     'if(!pending.empty())',
     'ShadowTest.RetractNeverAppliesOrCheckpointsProvisionalSuffix'),
    ('disk ceiling', SR,
     'if(!older(out.pos,*diskTarget))', 'if(false)',
     'ShadowTest.NeverCrossesHandshakeDurableCeilingWhenAnchorIsNewer'),
    ('missing strict buckets', 'libs/core/roller/Diff.cpp',
     'else if (strictJournal && (l.contains(t) != r.contains(t)))', 'else if (false)',
     'ShadowTest.MismatchMetricIsStrictAndCrossConnectionInformational'),
    ('duplicate overlap', SR,
     'if(applied && older(p,*applied)) return;', 'if(false) return;',
     'ShadowTest.CatchupJoinsSocketOverlapExactlyOnce'),
    ('primary root isolation', SR,
     'overlaps(cfg.outputRoot,primary) || overlaps(cfg.outputRoot,cfg.journalRoot)',
     'false', 'ShadowTest.RefusesAliasedPrimaryRoot'),
    ('stop wait synchronization', SR,
     'std::lock_guard lock(mutex); stopping = true;', 'stopping = true;',
     'ShadowTest.StopWakesCheckerAcrossPredicateWaitTransition'),
    ('comparison watermark restore', SR,
     'p->compared = saved.at("comparedThroughMs").get<int64_t>();', 'p->compared = first;',
     'ShadowTest.ComparisonWatermarkAndMismatchTotalsSurviveRestart'),
    ('malformed journal invalidation', SR,
     'o.onInvalid = [&](const std::string &reason) {',
     'o.onInvalid = [&](const std::string &reason) { if (reason.starts_with("malformed")) throw std::logic_error(reason);',
     'ShadowTest.MalformedRecordInvalidatesAndContinuesLikeBatch'),
    ('persistent fault cooldown', SR,
     'coolingDown ? cfg.failureCooldown : backoff', 'backoff',
     'ShadowTest.PersistentWriteFaultEntersCooldownAndRecovers'),
    ('cooldown minimum duration', SR,
     'now - *failureSince >= cfg.failureMinDuration', 'true',
     'ShadowTest.ThirtySecondOutageNeverEntersCooldown'),
    ('transport retains applied book state', SR,
     'if (!applied) throw;', 'throw;',
     'ShadowTest.JournalUnavailableResumesAppliedCursorWithoutReplay'),
    ('server shadow startup independence', APP,
     'm_shadowRoller = std::make_unique<sentinel::roller::ShadowRoller>(shadow, products, primaryRoot, m_metrics);',
     'm_shadowRoller = std::make_unique<sentinel::roller::ShadowRoller>(shadow, products, primaryRoot, m_metrics); m_shadowRoller->stop();',
     'ShadowTest.StalledAndFailingShadowDoesNotDelayServerPrimary'),
    ('missing comparison checkpoint is not a fresh audit', SR,
     'if (p->comparisonCheckpointSeen)', 'if (false)',
     'ShadowTest.ComparisonWatermarkAndMismatchTotalsSurviveRestart'),
    # Slice D-a (option C live path, recording.source: roller).
    ('A10 lead applies provisional records on arrival', SR,
     'pending.push_back(std::move(r)); if (lead) lead->apply(pending.back());',
     'pending.push_back(std::move(r));',
     'ShadowTest.LeadPublishesProvisionalRecordBeforeItsDurableMarker'),
    ('A10 lead claims no committed cutoff', BR,
     'r->committedThroughMs = lead() ? 0 : s.closedThrough;', 'r->committedThroughMs = s.closedThrough;',
     'ShadowTest.LeadPublishesProvisionalRecordBeforeItsDurableMarker'),
    ('A11 retract drops the lead', SR,
     'pendingSize = 0; if (lead) lead->discard(); }', 'pendingSize = 0; }',
     'ShadowTest.RetractRebuildsLiveMinuteFromDurableState'),
    ('A11 disconnect drops the lead', SR,
     'pendingSize = 0; if (lead) lead->discard(); }', 'pendingSize = 0; }',
     'ShadowTest.DisconnectRebuildsLiveFromDurableState'),
    ('A11 dropped lead is withdrawn before the rebuild publishes', SR,
     'cfg.retractLive(product);', ';',
     'ShadowTest.RetractAcrossMinuteWithdrawsBeforeRebuilding'),
    ('lead fork copies the minute state', BR,
     'to->observed = from->observed;', '',
     'ShadowTest.LeadForkFinishesMinutesExactlyLikeHistory'),
    ('lead wall ticks while the socket waits', SR,
     'lead->idle();', '',
     'ShadowTest.LeadWallTicksAdvanceQuietFormingMinute'),
    ('A1 unjournaled product refused', SR,
     'if (journalHasProduct(cfg.journalRoot, p->name) == false) {', 'if (false) {',
     'ShadowTest.ListedProductWithoutJournalIsRefusedWithoutWorker'),
    ('served watermarks never regress at midnight', SR,
     's.minuteThroughMs = std::max(s.minuteThroughMs, w.minuteThroughMs);',
     's.minuteThroughMs = w.minuteThroughMs;',
     'ShadowTest.MidnightRotationKeepsHistoryAndLiveForTwoProducts'),
    ('A2/A4 server attaches the serving roller', APP,
     'if (serving && m_shadowRoller->active()) {', 'if (false) {',
     'ShadowTest.ServingRollerReplacesPrimaryRecorderForNonDefaultProduct'),
    ('A5 served root is not a protected primary root', APP,
     'serving ? std::filesystem::path(m_serverConfig.recording.dir)', 'serving ? *m_serverModel->recordingDir()',
     'ShadowTest.ServingRollerReplacesPrimaryRecorderForNonDefaultProduct'),
    ('A3 model watermarks from the roller', 'libs/core/servermodel/ServerDataModel.hpp',
     'if (m_rollerAttached.load()) return m_rollerWatermarks(symbol, layer);', '',
     'ShadowTest.ServingRollerReplacesPrimaryRecorderForNonDefaultProduct'),
    ('stall monitor follows roller workers', 'libs/core/servermodel/ServerDataModel.cpp',
     'm_stallMonitor->setConnected(series.symbol, m_rollerRunning(series.symbol), nowMs);', ';',
     'ShadowTest.ServingRollerReplacesPrimaryRecorderForNonDefaultProduct'),
    ('A8 require-recording checks the served root', APP,
     'const std::string dir = roller ? config.rollerShadow.outputRoot : rc.dir;', 'const std::string dir = rc.dir;',
     'ShadowConfig.RequireRecordingChecksTheServedRoot'),
    ('A1 roller_shadow.products parsed', 'libs/core/ConfigLoader.cpp',
     'cfg.rollerShadow.products = normalizedDefaultSymbols(parseSymbolList(shadow["products"]));', ';',
     'ShadowConfig.ProductsAndRecordingSourceKeys'),
    ('A6 publisher is not checkpoint policy', 'libs/core/roller/Roller.cpp',
     'hash = configHash(cfg);', 'hash = configHash(cfg) ^ uint64_t(bool(o.publisher));',
     'Roller.PublisherKeepsSliceACheckpointPolicyHash', ROLLER),
    ('history publishes finals only', BR,
     'cfg.publication == RecorderConfig::Publication::Finals ||', '',
     'Roller.PublisherKeepsSliceACheckpointPolicyHash', ROLLER),
    ('lead never opens a store', BR,
     'if (!lead()) store.emplace(', 'store.emplace(',
     'Roller.LeadForkNeverPersistsAndClaimsNoCommit', ROLLER),
    ('product lease may write the served root', 'libs/core/roller/RollCli.cpp',
     '!o.productWriterLease && server["roller_shadow"]', 'server["roller_shadow"]',
     'Roller.RollCliProductLeaseMayWriteRollerServedRoot', ROLLER),
    ('A9 deploy marker accepts the serving roller', 'scripts/dev/deploy-runtime.sh',
     '/Roller serving ready products=/ {ready = 1}', '',
     'sh:tests/roller/deploy_marker_test.sh', None),
    # Review round 1.
    ('r1-1 lead drop withdraws its provisional minutes', SR,
     'cfg.retractLive(product);', ';',
     'ShadowTest.WithdrawnRecoverySnapshotLeavesLiveServiceSubscribers'),
    ('r1-1 withdrawal frame resends the newest final', 'libs/core/servermodel/RecordingLive.cpp',
     'first = std::prev(snapshot.committed.end());', ';',
     'ShadowTest.WithdrawnRecoverySnapshotLeavesLiveServiceSubscribers'),
    ('r1-1 withdrawal advances the series revision', 'libs/core/servermodel/RecordingLive.cpp',
     'it->second.withdrawn = true; ++it->second.revision;', 'it->second.withdrawn = true;',
     'ShadowTest.WithdrawnRecoverySnapshotLeavesLiveServiceSubscribers'),
    ('r1-2 started line is not a deploy marker', 'scripts/dev/deploy-runtime.sh',
     '/Recording v2 started/ {primary = 1}', '/Recording v2 started|mode=live/ {primary = 1}',
     'sh:tests/roller/deploy_marker_test.sh', None),
    ('r1-2 deploy log must carry the deployed exe', 'scripts/dev/deploy-runtime.sh',
     '''head -n 5 <<<"$content" | awk -v exe="exe=$4" '$1 == "#" && $2 == exe {ok = 1} END {exit !ok}' || continue''', '',
     'sh:tests/roller/deploy_marker_test.sh', None),
    ('r1-2 deploy log must carry the restarted PID', 'scripts/dev/deploy-runtime.sh',
     '''head -n 1 <<<"$content" | awk -v pid="pid=$3" '$NF == pid {ok = 1} END {exit !ok}' || continue''', '',
     'sh:tests/roller/deploy_marker_test.sh', None),
    ('r1-3 merged config names the served root', 'libs/core/roller/RollCli.cpp',
     'effective.recording.source == "roller" &&', 'false &&',
     'Roller.RollCliRefusesRollerRootFromSplitOverride', ROLLER),
    # Review round 2.
    ('r2-1 seed the newest persisted minute before publishing', SR,
     'if (!newest || !cfg.ensureLiveFinal(product, layer, newest)) return false;', 'continue;',
     'ShadowTest.RestartWithdrawalWithZeroCachedFinalsReachesSubscriber'),
    ('r2-1 no lead before a committed minute is cached', SR,
     'if (!cfg.ensureLiveFinal) return true;', 'return true;',
     'ShadowTest.LeadWaitsForACommittedMinuteOnANewRoot'),
    ('r2-1 quiet socket retries the deferred fork', SR,
     'if (atTip) // quiet socket: retry a fork the final-minute gate deferred\n lead->ensure(pending);', '',
     'ShadowTest.LeadWaitsForACommittedMinuteOnANewRoot'),
    ('r2-2 healthy only after a durable checkpoint', SR,
     'sLog_App("Roller writer open product=" << p.name);',
     'sLog_App("Roller writer open product=" << p.name); healthy(p);',
     'ShadowTest.ServingReadinessNeedsADurableCheckpointAfterOpen'),
    ('r2-2 failure or close revokes readiness', SR,
     'if (!std::exchange(p.healthy, false)) return;', 'return;',
     'ShadowTest.ServingReadinessNeedsEveryProductHealthyAtOnce'),
    ('r2-2 deploy check takes the latest readiness transition', 'scripts/dev/deploy-runtime.sh',
     '/Roller serving not ready product=/ {ready = 0}', '',
     'sh:tests/roller/deploy_marker_test.sh', None),
    ('r2-3 accept only if launchd still runs the read PID', 'scripts/dev/deploy-runtime.sh',
     '[[ $(service_pid "$label") == "$pid" ]] || continue', '',
     'sh:tests/roller/deploy_marker_test.sh', None),
    ('r2-3 one read of a regular non-symlink file', 'scripts/dev/deploy-runtime.sh',
     '[[ -f $1 && ! -L $1 ]] || return 1', 'cat -- "$1"; return',
     'sh:tests/roller/deploy_marker_test.sh', None),
    # Review round 3.
    ('r3-2 a failed log read fails', 'scripts/dev/deploy-runtime.sh',
     'content=$(cat <&3 && printf x) || status=1', 'content=$(cat <&3 && printf x)',
     'sh:tests/roller/deploy_marker_test.sh', None),
    ('r3-2 a short log read fails', 'scripts/dev/deploy-runtime.sh',
     '[[ $status == 0 && ${#content} -ge $size ]] || return 1', '[[ $status == 0 ]] || return 1',
     'sh:tests/roller/deploy_marker_test.sh', None),
    # Slice D-b1 (recording.live_feed: journal; one per check, then extras).
    ('db1-A1 the tip hands over one synthesized snapshot', SR,
     'if (live.valid) show(live); else hide("journal book invalid at the tip");',
     'hide("journal book invalid at the tip");',
     'ShadowTest.JournalTapSeedsOneSnapshotEqualToBatchThenProvisionalUpdates'),
    ('db1-A1 provisional records reach the model on arrival', SR,
     'if (tap) tap->apply(pending.back());', '',
     'ShadowTest.JournalTapSeedsOneSnapshotEqualToBatchThenProvisionalUpdates'),
    ('db1-A2 retract invalidates and re-seeds', SR,
     'if (tap.enabled()) tap.drop(reason);', '',
     'ShadowTest.JournalTapRetractInvalidatesThenReseedsDurableState'),
    ('db1-A2 disconnect invalidates and re-seeds', SR,
     'if (tap.enabled()) tap.drop(reason);', '',
     'ShadowTest.JournalTapDisconnectInvalidatesThenReseedsDurableState'),
    ('db1-A2 EOF invalidates and re-seeds', SR,
     'if (tap.enabled()) tap.drop(reason);', '',
     'ShadowTest.JournalTapEofInvalidatesThenReseedsDurableState'),
    ('db1-A3 header metadata reaches the model', SR,
     'cfg.model.metadata(product, *m);', ';',
     'ShadowTest.JournalLiveFeedServerHasNoEngineAndServesCapturedProducts'),
    ('db1-A4 aggressor flip in JournalFeed', 'libs/core/roller/JournalFeed.cpp',
     'if (trade.side == AggressorSide::Buy) trade.side = AggressorSide::Sell;',
     'if (false) trade.side = AggressorSide::Sell;',
     'ShadowTest.JournalLiveFeedServerTradesCarryTheAggressorSide'),
    ('db1-A5 no engine with the journal feed', APP,
     'if (!journal) try {', 'try {',
     'ShadowTest.JournalLiveFeedServerHasNoEngineAndServesCapturedProducts'),
    ('db1-A5 uncaptured products are invalid', APP,
     'return SentinelStreamServer::FeedAdmission::InvalidProduct;',
     'return SentinelStreamServer::FeedAdmission::Accepted;',
     'ShadowTest.JournalLiveFeedServerHasNoEngineAndServesCapturedProducts'),
    ('db1-A5 journal needs the roller source', 'libs/core/config/ConfigTypes.hpp',
     'if (feed == "journal" && config.recording.source != "roller")', 'if (false)',
     'ShadowTest.JournalLiveFeedRefusedWithoutTheRollerSource'),
    ('db1-A6 tip-age guard', SR,
     'if (now() - lastRecordMs >= cfg.tipSilenceMs) {', 'if (false) {',
     'ShadowTest.JournalLiveFeedServerConnectionFollowsTransportAndTipAge'),
    ('db1-A6 transport down invalidates the book', SR,
     'live.clear(); hide(reason);', 'live.clear();',
     'ShadowTest.JournalLiveFeedServerConnectionFollowsTransportAndTipAge'),
    ('db1-A7 nothing reaches the model during catch-up', SR,
     'durable.snapshot(e, v);', 'durable.snapshot(e, v); show(durable);',
     'ShadowTest.JournalTapRestartSeedsOnlyAfterCatchup'),
    ('db1-A8 day rotation is quiet', SR,
     'if (!h && !f) tap.detach();', 'if (!h && !f) tap.drop("writer closed");',
     'ShadowTest.JournalTapMidnightReseedsOnceWithoutInvalidation'),
    ('db1-A9 the feed tap is not checkpoint policy', 'libs/core/roller/Roller.cpp',
     'hash = configHash(cfg);', 'hash = configHash(cfg) ^ uint64_t(bool(o.onFeed));',
     'Roller.FeedTapKeepsCheckpointPolicyHashAndOutput', ROLLER),
    ('db1 re-seed request reaches the worker', SR,
     'p->reseed.store(true);', ';',
     'ShadowTest.JournalTapReseedRequestSendsTheCurrentLiveBook'),
    ('db1 model re-seeds instead of awaiting an upstream snapshot', 'libs/core/servermodel/ServerDataModel.cpp',
     'requestReseed(symbol, "raw band exceeded");', '',
     'ShadowTest.JournalLiveFeedServerHasNoEngineAndServesCapturedProducts'),
    ('db1 journal catch-up never repeats a delivered trade', SR,
     'tradeHere = !seeded && undelivered(p);', 'tradeHere = !seeded;',
     'ShadowTest.JournalTapDeliversEachTradeOnceAcrossRecovery'),
    ('db1 ring replay never repeats a delivered trade', SR,
     'tradeHere = undelivered(r.pos);', 'tradeHere = true;',
     'ShadowTest.JournalTapRingReplayNeverRedeliversATrade'),
    ('db1 the model never asks for metadata over REST', 'libs/core/servermodel/ServerDataModel.cpp',
     'if (m_journalFeed) return;', '',
     'JournalLiveFeedModel.NeverRequestsRestMetadata'),
    ('db1-lag the main-thread lag timer runs', 'libs/core/metrics/EventLoopLag.cpp',
     'm_timer.start();', ';',
     'ServerMetrics.EventLoopLagShowsABlockedMainThread', 'servermodel/test_server_metrics'),
    ('db1-lag a late tick is counted', 'libs/core/metrics/EventLoopLag.cpp',
     'if (lagMs > kLateMs && m_late) m_late->inc();', ';',
     'ServerMetrics.EventLoopLagShowsABlockedMainThread', 'servermodel/test_server_metrics'),
    ('db1-lag the server registers the sampler in journal mode', APP,
     'm_eventLoopLag.registerMetrics(m_metrics);', ';',
     'ShadowTest.JournalLiveFeedServerHasNoEngineAndServesCapturedProducts'),
    ('db1 live_feed key parsed', 'libs/core/ConfigLoader.cpp',
     'readScalar(rec, "live_feed", cfg.recording.liveFeed);', ';',
     'ShadowConfig.LiveFeedKeyDefaultsToEngine'),
    # D-b1 fix round 1.
    ('db1-r1-1 throttled re-seed requests are coalesced, not dropped', 'libs/core/servermodel/ServerDataModel.cpp',
     'm_pendingReseeds.insert(symbol);', ';',
     'ShadowTest.JournalLiveFeedServerCoalescesReseedsInsideTheThrottle'),
    # D-b1 fix round 2: candles of a journal product close on its journal time.
    ('db1-r2 the wall-clock timer never closes a journal product', 'libs/core/servermodel/TimeframeAggregator.cpp',
     'if (!m_journal.contains(symbol)) closeElapsed(state, symbol, nowMs);', 'closeElapsed(state, symbol, nowMs);',
     'JournalLiveFeedModel.DelayedOutageDetectionKeepsCandlesIdentical'),
    ('db1-r2 a trade closes the buckets before it on journal time', 'libs/core/servermodel/TimeframeAggregator.cpp',
     'if (journal) closeElapsed(state, trade.product_id, tsMs);', '',
     'JournalLiveFeedModel.DelayedOutageDetectionKeepsCandlesIdentical'),
    ('db1-r2 the watermark closes quiet buckets', 'libs/core/servermodel/TimeframeAggregator.cpp',
     'if (state != m_states.end()) closeElapsed(state->second, symbol, watermarkMs);', '',
     'JournalLiveFeedModel.HeartbeatsCloseBarsWithoutTrades'),
    ('db1-r2 the model lags the watermark behind receive time', 'libs/core/servermodel/ServerDataModel.cpp',
     'm_aggregator->advance(symbol, journalMs - kJournalCandleLagMs);', 'm_aggregator->advance(symbol, journalMs);',
     'JournalLiveFeedModel.HeartbeatsCloseBarsWithoutTrades'),
    ('db1-r2 a closed journal bucket is never reopened', 'libs/core/servermodel/TimeframeAggregator.cpp',
     'if (journal && start < bar.timestamp_ms) return;', '',
     'JournalLiveFeedModel.HeartbeatsCloseBarsWithoutTrades'),
    ('db1-r2 the tap hands over the journal watermark', SR,
     'sendWatermark(r); serviceReseed();', 'serviceReseed();',
     'ShadowTest.JournalTapDeliversEachTradeOnceAcrossRecovery'),
    ('db1-r2 gap-fill records advance the watermark', SR,
     'if (tradeHere && !seeded) sendWatermark(r);', ';',
     'ShadowTest.JournalTapDeliversEachTradeOnceAcrossRecovery'),
    ('db1-r1-2 a fresh process never replays the day\'s trades', SR,
     'if (!delivered) return false;', 'if (!delivered) return true;',
     'ShadowTest.JournalTapRestartSeedsOnlyAfterCatchup'),
    ('db1-r1-3 captured products are exempt from the engine cap', 'libs/core/protocol/SentinelStreamServer.cpp',
     'const auto pinned = journal ? rollerProducts(m_serverConfig)', 'const auto pinned = false ? rollerProducts(m_serverConfig)',
     'ShadowTest.JournalLiveFeedAdmissionIgnoresTheEngineCap'),
]
if "--round1" in sys.argv:
    mutations = mutations[5:]
if "--slice-d" in sys.argv:
    mutations = mutations[13:]
if "--db1" in sys.argv:
    mutations = [m for m in mutations if m[0].startswith('db1')]
for arg in sys.argv:
    if arg.startswith('--skip='):
        mutations = mutations[int(arg[7:]):]
if "--r1" in sys.argv:
    mutations = [m for m in mutations if m[0].startswith(('r1-', 'r2-', 'r3-', 'A9', 'A11'))]
for entry in mutations:
    name, path, before, after, case = entry[:5]
    target = entry[5] if len(entry) > 5 else SHADOW
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
        build(target)
        test(case, target, fail=True)
        print(f'FAIL-WITHOUT: {name}: expected assertion failure', flush=True)
    finally:
        p.write_text(original)
        os.utime(p, None)
        build(target)
    test(case, target)
    print(f'RESTORED: {name}: passed', flush=True)
print(f'SHADOW_MUTATIONS: {len(mutations)}/{len(mutations)} fail-without and restored checks passed', flush=True)
