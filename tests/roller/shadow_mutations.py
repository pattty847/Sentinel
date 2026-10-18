#!/usr/bin/env python3
"""Run inside build-queue; mutations restore, touch, rebuild and re-pass (FM-132)."""
import os
import re
import sys
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
os.chdir(ROOT)
os.environ.setdefault('CCACHE_DIR', '/tmp/sentinel-db2b-ccache')
SHADOW, ROLLER = 'test_shadow_roller', 'test_roller'

def command(args):
    return subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

def build(target):
    if target is None:
        return
    # Caller holds the FIFO build queue; use the owner-approved six jobs.
    p = command(['cmake', '--build', '--preset', 'mac-clang', '-j', '6', '--target', target.split('/')[-1]])
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
     'ShadowTest.MismatchMetricChecksOnlySameJournal'),
    ('duplicate overlap', SR,
     'if(applied && older(p,*applied)) return;', 'if(false) return;',
     'ShadowTest.CatchupJoinsSocketOverlapExactlyOnce'),
    ('protected root isolation', SR,
     '!root.empty() && overlaps(cfg.outputRoot, root)',
     'false', 'ShadowTest.RefusesAliasedProtectedRoot'),
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
     'shadow.protectedRoots = {m_serverConfig.recording.dir};', 'shadow.protectedRoots = {*m_serverModel->recordingDir()};',
     'ShadowTest.ServingRollerReplacesPrimaryRecorderForNonDefaultProduct'),
    ('A3 model watermarks from the roller', 'libs/core/servermodel/ServerDataModel.hpp',
     'if (m_rollerAttached.load()) return m_rollerWatermarks(symbol, layer);', '',
     'ShadowTest.ServingRollerReplacesPrimaryRecorderForNonDefaultProduct'),
    ('stall monitor follows roller workers', 'libs/core/servermodel/ServerDataModel.cpp',
     'm_stallMonitor->setConnected(series.symbol, m_rollerRunning(series.symbol), nowMs);', ';',
     'ShadowTest.ServingRollerReplacesPrimaryRecorderForNonDefaultProduct'),
    ('A8 require-recording checks the served root', APP,
     'const std::string dir = config.rollerShadow.outputRoot;', 'const std::string dir = rc.dir;',
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
    ('db1-A5 uncaptured products are invalid', APP,
     'return SentinelStreamServer::FeedAdmission::InvalidProduct;',
     'return SentinelStreamServer::FeedAdmission::Accepted;',
     'ShadowTest.JournalLiveFeedServerHasNoEngineAndServesCapturedProducts'),
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
    ('db1-lag the queue-latency helper runs', 'libs/core/metrics/EventLoopLag.cpp',
     'm_worker = std::thread([this] { run(); });', ';',
     'ServerMetrics.EventLoopLagShowsABlockedMainThread', 'servermodel/test_server_metrics'),
    ('db1-lag a late tick is counted', 'libs/core/metrics/EventLoopLag.cpp',
     'if (lagMs > kLateMs && m_late) m_late->inc();', ';',
     'ServerMetrics.EventLoopLagShowsABlockedMainThread', 'servermodel/test_server_metrics'),
    ('db1-lag queue latency is not a constant zero', 'libs/core/metrics/EventLoopLag.cpp',
     'const double lagMs = std::max(0.0, std::chrono::duration<double, std::milli>(delivered - sent).count());',
     'const double lagMs = 0.0;',
     'ServerMetrics.EventLoopLagSteadyBusyMainThread:'
     'ServerMetrics.EventLoopLagQueueLatencyUsesSendTimeAndRecovers:'
     'ServerMetrics.EventLoopLagWindowExpiryAndRingWrap',
     'servermodel/test_server_metrics'),
    ('db1-lag only one probe post may be outstanding', 'libs/core/metrics/EventLoopLag.cpp',
     'if (m_outstanding.exchange(true, std::memory_order_acq_rel))', 'if (false)',
     'ServerMetrics.EventLoopLagOutstandingGateIsBounded', 'servermodel/test_server_metrics'),
    ('db1-lag the server registers the sampler in journal mode', APP,
     'm_eventLoopLag.registerMetrics(m_metrics);', ';',
     'ShadowTest.JournalLiveFeedServerHasNoEngineAndServesCapturedProducts'),
    # D-b1 fix round 1.
    ('db1-r1-1 throttled re-seed requests are coalesced, not dropped', 'libs/core/servermodel/ServerDataModel.cpp',
     'm_pendingReseeds.insert(symbol);', ';',
     'ShadowTest.JournalLiveFeedServerCoalescesReseedsInsideTheThrottle'),
    # D-b1 fix round 2: candles of a journal product close on its journal time.
    ('db1-r2 a trade closes the buckets before it on journal time', 'libs/core/servermodel/TimeframeAggregator.cpp',
     'closeElapsed(state, trade.product_id, tsMs);', '',
     'JournalLiveFeedModel.DelayedOutageDetectionKeepsCandlesIdentical'),
    ('db1-r2 a trade closes quiet buckets before the next watermark', 'libs/core/servermodel/TimeframeAggregator.cpp',
     'closeElapsed(state, trade.product_id, tsMs);', '',
     'JournalLiveFeedModel.TradeClosesQuietBucketsBeforeTheNextWatermark'),
    ('db1-r2 the watermark closes quiet buckets', 'libs/core/servermodel/TimeframeAggregator.cpp',
     'if (state != m_states.end()) closeElapsed(state->second, symbol, watermarkMs);', '',
     'JournalLiveFeedModel.HeartbeatsCloseBarsWithoutTrades'),
    ('db1-r2 the model lags the watermark behind receive time', 'libs/core/servermodel/ServerDataModel.cpp',
     'm_aggregator->advance(symbol, journalMs - kJournalCandleLagMs);', 'm_aggregator->advance(symbol, journalMs);',
     'JournalLiveFeedModel.HeartbeatsCloseBarsWithoutTrades'),
    ('db1-r2 a closed journal bucket is never reopened', 'libs/core/servermodel/TimeframeAggregator.cpp',
     'if (start < bar.timestamp_ms) {',
     'if (start < bar.timestamp_ms && bar.count > 0) {',
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
]
mutations.append(('db1-anchor failure metric registered', SR,
    'registerAnchorMetrics(registry, p->name);', ';',
    'ShadowTest.AnchorFailureMetricRegisteredAtStartup'))
mutations.append(('db1-anchor durable model book is restored without replay', SR,
    'if (tap.enabled()) tap.restoreAnchor(book,up);', ';',
    'ShadowTest.JournalTapAnchorAtDurableTipRestoresWholeBookWithoutReplay'))
mutations.append(('db1-anchor batch rebuild remains live-compatible', 'libs/core/roller/Roller.cpp',
    'cfg.publication = recording::RecorderConfig::Publication::Finals;',
    'if (o.publisher) cfg.publication = recording::RecorderConfig::Publication::Finals;',
    'ShadowTest.JournalTapRestoresBatchRebuiltAnchor'))
mutations.append(('db1-anchor restored durable state reports running', SR,
    'p.last->store(receiveMs); p.running->set(1);', ';',
    'ShadowTest.JournalTapAnchorAtDurableTipRestoresWholeBookWithoutReplay'))
# D-b2: structural deletion guards plus behavioral contract mutations.
ADMISSION = 'servermodel/test_server_feed_admission'
mutations.extend([
    ('db2-C1 server cannot own an engine', APP,
     '#include "SentinelServerApp.hpp"',
     '#include "SentinelServerApp.hpp"\n#include "marketdata/MarketDataFeeds.hpp"',
     'ServerFeedAdmissionSource.ServerHasOnlyTheJournalFeed', ADMISSION),
    ('db2-C2 uncaptured products are refused', APP,
     'return SentinelStreamServer::FeedAdmission::InvalidProduct;',
     'return SentinelStreamServer::FeedAdmission::Accepted;',
     'ShadowTest.JournalLiveFeedAdmissionAcceptsAllCapturedProducts'),
    ('db2-C2 refusal JSON has no cap', 'libs/core/protocol/SentinelStreamServer.cpp',
     '{"code", "invalid_product"}, {"symbol", symbol}',
     '{"code", "invalid_product"}, {"symbol", symbol}, {"max_connections", 8}',
     'ServerFeedAdmissionTest.UncapturedProductRefusalIsAtomic', ADMISSION),
    ('db2-C3 primary source cannot start', 'libs/core/config/ConfigTypes.hpp',
     'if (config.recording.source != "roller")', 'if (false)',
     'ShadowTest.JournalLiveFeedRefusedWithoutTheRollerSource'),
    ('db2-C4 trades never create legacy logs', 'libs/core/servermodel/ServerDataModel.cpp',
     'void ServerDataModel::onTrade(const Trade& trade) {',
     'void ServerDataModel::onTrade(const Trade& trade) { std::filesystem::create_directories("data/market");',
     'ServerFeedAdmissionTest.TradesNeverCreateLegacyTickLogs', ADMISSION),
    ('db2-C5 no metadata REST call site', 'libs/core/protocol/SentinelStreamServer.cpp',
     'CoinbaseRestClient& SentinelStreamServer::restClient() {',
     'CoinbaseRestClient& SentinelStreamServer::restClient() { /* fetchProductMetadata */',
     'ServerFeedAdmissionSource.ServerHasOnlyTheJournalFeed', ADMISSION),
    ('db2-C6 journal feed metric replaces MDC', 'libs/core/servermodel/ServerDataModel.cpp',
     'r.familyFn("sentinel_live_feed_up",', 'r.familyFn("sentinel_mdc_connected",',
     'ShadowTest.JournalLiveFeedServerConnectionFollowsTransportAndTipAge'),
    ('db2-C7 REST CA override survives', 'libs/core/ConfigLoader.cpp',
     'readMdcScalar(serverRoot, mdc, filePath, wrapped, "ssl_ca_bundle", cfg.mdc.sslCaBundle);', ';',
     'ServerFeedConfig.OnlyRestCaBundleIsReadFromMdc', ADMISSION),
    ('db2-C8 alert watches live journal feed', 'ops/monitoring/grafana/provisioning/alerting/rules.yaml',
     'expr: min by (product) (sentinel_live_feed_up)',
     'expr: min by (product) (sentinel_mdc_connected{pinned="1"})',
     'ServerFeedAlerts.A1bSelectsEachCapturedProductForFiveMinutes', ADMISSION),
])
# B2-R2: journal floor, bulk restore and candle rebuild.
TA = 'libs/core/servermodel/TimeframeAggregator.cpp'
CR = 'libs/core/roller/CandleRestore.cpp'
AGG, RESTORE = 'servermodel/test_timeframe_aggregator', 'test_candle_restore'
mutations.extend([
    ('b2r2 floor compare', TA,
     'start < state.floorMs[index])) {', 'false)) {',
     'TimeframeAggregatorRestore.JournalFloorDropsStaleTradesBeforeTheFirstBar', AGG),
    ('b2r2 floor compare reaches the rebuild', TA,
     'start < state.floorMs[index])) {', 'false)) {',
     'CandleRestore.InitialStateExtendsOneFileBackOrIsPartial', RESTORE),
    ('b2r2 floor is monotonic', TA,
     'state.floorMs[i] = std::max(state.floorMs[i], bucketStart(nowMs, tf));',
     'state.floorMs[i] = bucketStart(nowMs, tf);',
     'TimeframeAggregatorRestore.JournalFloorDropsStaleTradesBeforeTheFirstBar', AGG),
    ('b2r2 floor written only without a bar', TA,
     'if (bar.count == 0 && bar.timestamp_ms == 0) { if (state.floorOn[i])',
     'if (state.floorOn[i]) state.floorMs[i] = std::max(state.floorMs[i], bucketStart(nowMs, tf));'
     ' if (bar.count == 0 && bar.timestamp_ms == 0) { if (false)',
     'TimeframeAggregatorRestore.JournalFloorDropsStaleTradesBeforeTheFirstBar', AGG),
    ('b2r2 restore refuses an enabled floor', TA,
     'existing.floorOn[i])', 'existing.floorOn[i] && false)',
     'TimeframeAggregatorRestore.InstallsClosedAndActiveWithoutEmitting', AGG),
    ('b2r2 restore refuses existing history', TA,
     '!hist->second.empty()', 'false',
     'TimeframeAggregatorRestore.InstallsClosedAndActiveWithoutEmitting', AGG),
    ('b2r2 regression guard', CR,
     'return i == 0 ? 0 : i - 1;', 'break;',
     'CandleRestore.RegressionGuardStartsBeforeAClockAheadFile', RESTORE),
    ('b2r2 deep regression boundary', CR,
     'if (maxSeenMs - ms > kRestoreLeadInMs)', 'if (maxSeenMs - ms >= kRestoreLeadInMs)',
     'CandleRestore.CountsReceiveClockRegressionsWithoutChangingTheOutcome', RESTORE),
    ('b2r2 lead-in extension', CR,
     'const bool extend = !ok && start > 0', 'const bool extend = false && start > 0',
     'CandleRestore.InitialStateExtendsOneFileBackOrIsPartial', RESTORE),
    ('b2r2 rebuild seeds the floor', CR,
     'aggregator.seedJournalFloor(in.product, out.seedMs, in.timeframesMs);', ';',
     'CandleRestore.InitialStateExtendsOneFileBackOrIsPartial', RESTORE),
    ('b2r2 stopping is checked after each record, through included', CR,
     '// Checked after every record, `through` included.\n        if (in.stopping && in.stopping->load()) return std::nullopt;', ';',
     'CandleRestore.StoppingAbortsWithinOneRecord', RESTORE),
    ('b2r2 seed leaves a timeframe with a bar unchanged', TA,
     'bar != state.activeBars.end() && (bar->second.count != 0 || bar->second.timestamp_ms != 0))',
     'false)',
     'TimeframeAggregatorRestore.FloorIsNeitherSeededNorReadOnceABarExists', AGG),
    ('b2r2 floor read only without a bar', TA,
     '(state.floorOn[index] && bar.count == 0 && bar.timestamp_ms == 0 &&',
     '(state.floorOn[index] &&',
     'TimeframeAggregatorRestore.FloorIsNeitherSeededNorReadOnceABarExists', AGG),
])
# B2-R3: restart candle reconstruction activated (server and GUI).
SDM = 'libs/core/servermodel/ServerDataModel.cpp'
SSS = 'libs/core/protocol/SentinelStreamServer.cpp'
SSC = 'libs/core/protocol/SentinelStreamClient.cpp'
CBS = 'libs/gui/datasources/CandleBackfillState.hpp'
RGD = 'libs/gui/datasources/RemoteGridDataSource.cpp'
WIRE, BACKFILL, SOURCE = 'servermodel/test_candle_wire', 'render/test_CandleBackfillState', 'render/test_CandleDataSource'
GATE = 'ShadowTest.CandleRestoreGateDropsReconnectResendsUntilItExpires'
mutations.extend([
    ('b2r3 gate dedupe', SR,
     'if (!gate.ids.insert(t.trade_id).second) {', 'if (!gate.ids.insert(t.trade_id).second && false) {', GATE),
    ('b2r3 gate expires at TradeIdWindowCapacity', SR,
     'if (++gate.delivered >= JournalFeed::TradeIdWindowCapacity) {',
     'if (++gate.delivered > JournalFeed::TradeIdWindowCapacity) {', GATE),
    ('b2r3 gate skips empty ids', SR,
     'if (gate.active && !t.trade_id.empty()) {', 'if (gate.active) {', GATE),
    ('b2r3 gate survives an outer retry', SR,
     'gate.active = true;', 'gate.active = false;',
     'ShadowTest.CandleRestoreOuterRetryKeepsTheInstallAndDropsAResentId'),
    ('b2r3 handoff delivered rule', SR,
     'if (!delivered) delivered = r.through;', ';',
     'ShadowTest.CandleRestoreHandsOverExactlyAcrossARotationDuringTheRead'),
    ('b2r3 handoff watermark second', SR,
     'sentSecond = std::max(sentSecond, r.sentSecond);', ';',
     'ShadowTest.CandleRestoreHandoffKeepsTheWatermarkSecond'),
    ('b2r3 install precedes roll', SR,
     'if (restoreEnabled() && !std::exchange(p.candlesRestored, true)) restoreCandles(p, source, tap);', ';',
     'ShadowTest.CandleRestoreAfterRestartAcrossAnHourMatchesUninterruptedBars'),
    ('b2r3 one attempt per process', SR,
     'if (restoreEnabled() && !std::exchange(p.candlesRestored, true))', 'if (restoreEnabled())',
     'ShadowTest.CandleRestoreOuterRetryKeepsTheInstallAndDropsAResentId'),
    ('b2r3 failure still ends warming', SR,
     'counted = true; sLog_Error("Candle history restore failed product=" << p.name << " reason=" << failure); cfg.model.candleRestore(p.name, std::nullopt);',
     'counted = true;', 'ShadowTest.CandleRestoreFailureMarksReadyWithAnEmptyWindow'),
    ('b2r3 refused product ends warming', SR,
     'p->candleRestoreFailures->inc(); cfg.model.candleRestore(p->name, std::nullopt);',
     'p->candleRestoreFailures->inc();', 'ShadowTest.CandleRestoreRefusedProductIsReadyWithoutAnAttempt'),
    ('b2r3 cancellation posts nothing', SR,
     'if (!restore && stopping) {', 'if (false) {',
     'ShadowTest.CandleRestoreCancelledAtShutdownPostsNothing'),
    ('b2r3 readiness flip', SDM,
     'it->second.store(true, std::memory_order_release);', ';',
     'CandleWireRestore.InstallsIntoEmptySeriesOnlyAndAlwaysEndsWarming', WIRE),
    ('b2r3 refusal of a non-empty series', SDM,
     'empty = empty && m_aggregator->canRestoreSeries(symbol, tf);', ';',
     'CandleWireRestore.InstallsIntoEmptySeriesOnlyAndAlwaysEndsWarming', WIRE),
    ('b2r3 warming reply', SSS,
     'if (!model_.candleHistoryReady(symbol)) {', 'if (false) {',
     'CandleWireWarming.WarmingReplyEchoesTheWindowUntilTheRebuildEnds', WIRE),
    ('b2r3 warming gauge', SDM,
     "ready.load(std::memory_order_acquire) ? 0.0 : 1.0", '1.0',
     'ShadowTest.CandleRestoreAfterRestartAcrossAnHourMatchesUninterruptedBars'),
])
# The GUI half (commit 2): absent before it, so listed only when present.
if (ROOT / CBS).read_text().find('kWarmingRetryMs') >= 0:
    mutations.extend([
        ('b2r3 client reports warming', SSC,
         'if (msg.value("status", "") == "warming") {', 'if (false) {',
         'CandleDataSourceTest.WarmingRequestParksOutsideItsNeedAndResendsVerbatim', SOURCE),
        ('b2r3 held-request retry deadline', CBS,
         'deadline = std::max(deadline, m_pending->retryAtMs);', ';',
         'CandleBackfillState.WarmingHoldsTheRequestAndResendsItVerbatim', BACKFILL),
        ('b2r3 held request resent verbatim', CBS,
         'm_pending->held = false; m_pending->retryAtMs = 0; m_nextSendMs = nowMs + kThrottleMs; return m_pending;',
         'm_pending.reset();',
         'CandleBackfillState.WarmingHoldsTheRequestAndResendsItVerbatim', BACKFILL),
        ('b2r3 data source retries on warming', RGD,
         'if (m_candleBackfill.warming(symbol, timeframeMs, startMs, endMs, QDateTime::currentMSecsSinceEpoch()))',
         'if (false)', 'CandleDataSourceTest.WarmingSelectionSwitchSendsTheOtherRequestAtOnce', SOURCE),
    ])
if "--db2" in sys.argv:
    mutations = [m for m in mutations if m[0].startswith('db2-')]
if "--round1" in sys.argv:
    mutations = mutations[5:]
if "--slice-d" in sys.argv:
    mutations = mutations[13:]
if "--b2r2" in sys.argv:
    mutations = [m for m in mutations if m[0].startswith('b2r2')]
if "--b2r3" in sys.argv:
    mutations = [m for m in mutations if m[0].startswith('b2r3')]
if "--db1" in sys.argv:
    mutations = [m for m in mutations if m[0].startswith('db1')]
for arg in sys.argv:
    if arg.startswith('--from='):  # resume at a mutation by name, stable across list edits
        names = [m[0] for m in mutations]
        if arg[7:] not in names:
            raise SystemExit(f'--from: no mutation named {arg[7:]!r}')
        mutations = mutations[names.index(arg[7:]):]
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
