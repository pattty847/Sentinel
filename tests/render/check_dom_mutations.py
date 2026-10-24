#!/usr/bin/env python3
"""Fail-without-fix checks. Run inside build-queue; restores source in finally.

python3 tests/render/check_dom_mutations.py [build/mac-clang]
DOM_MUTANTS=pulse_ limits the run to mutants whose name starts with that prefix.
Uses installed dependencies; each mutant must COMPILE and fail its behavior test.
"""
from pathlib import Path
import os
import subprocess
import sys

root = Path(__file__).resolve().parents[2]
build = (root / (sys.argv[1] if len(sys.argv) > 1 else 'build/mac-clang')).resolve()
model = root / 'libs/gui/models/DomModel.cpp'
header = root / 'libs/gui/models/DomModel.hpp'
dock = root / 'libs/gui/widgets/OrderBookDock.cpp'
replica = root / 'libs/gui/datasources/RemoteGridDataSource.cpp'
original = {p: p.read_text() for p in (model, header, dock, replica)}
mutants = [
    ('fixed_dom_columns', dock,
     [('const int core = width - (counts ? countWidth * 3 : delta ? countWidth : 0);', 'const int core = 600;')],
     'DomDock.NarrowLadderFitsWithoutScrollingAndRestoresExecutionColumns'),
    ('leading_zero_kept', model,
     [('if (text.startsWith(QLatin1String("0."))) text.remove(0, 1);', '')],
     'DomModel.SizesShowThreeSignificantDigitsWithoutFloatNoise'),
    ('dust_in_e_notation', model,
     [('    if (value > 0 && value < 1e-4) return', '    if (false) return')],
     'DomModel.SizesShowThreeSignificantDigitsWithoutFloatNoise'),
    ('noisy_size_tooltip', model,
     [("return QString::number(value, 'g', 7);", "return QString::number(value, 'g', 12);")],
     'DomModel.SizesShowThreeSignificantDigitsWithoutFloatNoise'),
    ('duplicate_bucket', model,
     [('return m_top - index.row();', 'return m_top - (index.row() == 1001 ? 1000 : index.row());')],
     'DomModel.SameBucketUniqueBestAskStableEmptyRowsAndAlignment'),
    ('absolute_trade_buckets', model,
     [('const qint64 key = origin + static_cast<qint64>(offset);',
       'const qint64 key = std::llround(trade.price / tick);')],
     'DomModel.ExecutionsMatchReplicaTruncationAtDecimalBoundariesWithNonzeroOrigin'),
    ('false_replica_precision', replica,
     [('book.initialize(minPrice, maxPrice, tickSize);', 'book.initialize(minPrice, maxPrice, tickSize / 10);')],
     'DomModel.ServerQuantisedSnapshotsNeverAdvertiseFalsePrecision'),
    ('coarse_positive_prices_rendered', model,
     [(' || (mid > 0 && tick > mid * 0.01)', '')],
     'DomModel.ServerQuantisedSnapshotsNeverAdvertiseFalsePrecision'),
    ('coarsened_eth_replica', replica,
     [('book.initialize(minPrice, maxPrice, tickSize);',
       'book.initialize(minPrice, maxPrice, symbol == "ETH-USD" ? (minPrice < 3000 ? 0.2 : 0.5) : tickSize);')],
     'DomModel.EthReplicaPreservesAdjacentServerLevelsAndBookTop'),
    ('quiet_book_marked_stale', model,
     [('if (connected == true) return QStringLiteral("Connected · %1").arg(age);',
       'if (nowMs - lastChangeMs >= 3000) return QStringLiteral("Stale · %1").arg(age); if (connected == true) return QStringLiteral("Connected · %1").arg(age);')],
     'DomFreshness.LiveQuietBookHasChangeAgeButDoesNotBecomeStale:DomDock.QuietConnectedBookRetainsLastChangeAgeAndDetectsDisconnect'),
    ('unknown_as_sell', model,
     [('        if (trade.side != AggressorSide::Buy && trade.side != AggressorSide::Sell) continue;\n', '')],
     'DomModel.TradesAreVolumeOverTheTrailingMinuteByTradeTimeUnknownNeverSells'),
    ('counts_not_volume', model,
     [('if (trade.side == AggressorSide::Buy) row.buys += trade.size;', 'if (trade.side == AggressorSide::Buy) row.buys += 1;')],
     'DomModel.TradesAreVolumeOverTheTrailingMinuteByTradeTimeUnknownNeverSells'),
    ('no_time_window', model,
     [('if (!DomTradeWindow::inWindow(trade.timeMs, nowMs) || !std::isfinite(trade.size) || trade.size <= 0) continue;',
       'if (!std::isfinite(trade.size) || trade.size <= 0) continue;')],
     'DomModel.TradesAreVolumeOverTheTrailingMinuteByTradeTimeUnknownNeverSells'),
    ('broken_ring_eviction', model,
     [('m_next = (m_next + 1) % Capacity;', 'm_next = (m_next + 1) % (Capacity - 1);')],
     'DomModel.RingEvictsOldestAndLabelsAWindowItCannotCover'),
    ('coverage_from_retained_head', model,
     [('    if (m_size == Capacity) m_evictedMaxMs = std::max(m_evictedMaxMs, m_entries[m_next].timeMs);\n', '')],
     'DomModel.RingCoverageWarningSurvivesUntimedAndOutOfOrderHeads'),
    ('coverage_never_wakes', model,
     [('    if (trades.evictedMaxMs() > cutoff) wakeAt(trades.evictedMaxMs() + DomTradeWindow::WindowMs);\n', '')],
     'DomModel.CoverageWarningEndsWhenTheNewestEvictedTradeLeavesTheMinute'),
    ('text_invalidated_by_row', model,
     [('if (valueChanged) m_textValid[r] &= uint8_t(~textChanges(m_rows[r], m_next[r], moved));',
       'if (valueChanged) m_textValid[r] = 0;')],
     'DomModel.DisplayStringsAreFormattedOnceUntilTheirValueChanges'),
    ('future_trades_counted', header,
     [('&& timeMs <= nowMs + SkewMs', '')],
     'DomModel.FutureStampedTradesWaitForTheirTimeAndCountOnce'),
    ('unavailable_keeps_republishing', model,
     [('    // dock does not republish each tick until rows return.\n    m_scales.fill({});\n    m_lastScaleMs = 0;\n    m_visibleChanged = false;\n',
       '    // dock does not republish each tick until rows return.\n')],
     'DomModel.PublicationRequestsSettleWhenTheLadderGoesAway'),
    ('depth_walks_price_distance', model,
     [('const qint64 from = std::max<qint64>(fromKey - origin, 0);', 'const qint64 from = fromKey - origin;'),
      ('                if (std::isfinite(side[source]) && side[source] > 0) total += side[source];',
       '                if (source >= 0 && std::isfinite(side[source]) && side[source] > 0) total += side[source];')],
     'DomModel.PinnedCumulativeReadsOnlyTheReplicaAndReusesItsSum'),
    ('depth_rescanned_every_publish', model,
     [('if (!m_depthKey || !(*m_depthKey == key)) {', 'if (true) {')],
     'DomModel.PinnedCumulativeReadsOnlyTheReplicaAndReusesItsSum'),
    ('text_formatted_every_paint', model,
     [('    if (m_textValid[r] & (1u << col)) return text;\n', '')],
     'DomModel.DisplayStringsAreFormattedOnceUntilTheirValueChanges'),
    ('signed_dust_unclear', model,
     [('    if (std::abs(value) < 1e-4) return QStringLiteral("≈0");\n', '')],
     'DomModel.SizesShowThreeSignificantDigitsWithoutFloatNoise'),
    ('all_rows_scale', model,
     [('for (int r = firstVisible; r <= lastVisible; ++r) {', 'for (int r = 0; r < Rows; ++r) {')],
     'DomModel.BarsScaleToVisibleRowsWithSquareRootAndSmoothedMaximum'),
    ('linear_bars', model,
     [('return std::sqrt(std::min(1.0, value / maximum));', 'return std::min(1.0, value / maximum);')],
     'DomModel.BarsScaleToVisibleRowsWithSquareRootAndSmoothedMaximum'),
    ('unsmoothed_scale', model,
     [('scale.shown = ratio <= 0.02 ? scale.target : scale.target * std::exp(ratio);', 'scale.shown = scale.target;')],
     'DomModel.BarsScaleToVisibleRowsWithSquareRootAndSmoothedMaximum'),
    ('window_only_cumulative', model,
     [('m_outsideBid = m_bestBid > 0', 'm_outsideBid = 0 * m_bestBid > 0')],
     'DomModel.CumulativeSumsFromTheTouchOutwardIncludingLevelsBeyondAPinnedWindow'),
    ('cumulative_display_ignored', model,
     [('const double value = m_cumulative ? row.cumBid : row.bid;', 'const double value = row.bid;')],
     'DomModel.CumulativeSumsFromTheTouchOutwardIncludingLevelsBeyondAPinnedWindow'),
    ('unpersisted_cumulative', dock,
     [('m_cumulative->setChecked(settings.value("OrderBookDock/cumulative", false).toBool());', '')],
     'DomDock.CumulativeToggleSwitchesTheSizeColumnsAndPersists'),
    ('unfilled_best_cells', dock,
     [('        painter->fillRect(area, bestFill(bid));\n', '')],
     'DomDock.BestCellsFillInTheSideColourAndBarsUseTheRamp'),
    ('visible_rows_never_reported', dock,
     [('m_model->setVisibleRows(first, last < 0 && first >= 0 ? m_model->rowCount() - 1 : last);',
       'm_model->setVisibleRows(-1, -1);')],
     'DomDock.BestCellsFillInTheSideColourAndBarsUseTheRamp'),
    ('invented_colours', dock,
     [('return paletteRamp()[(bid ? 0 : half) + int(std::lround(position * (half - 1)))];',
       'return bid ? QColor(40, 125, 135) : QColor(160, 115, 40);')],
     'DomDelegate.HeatmapRampPriceDigitsAndSizeTextTint'),
    ('whole_price_dim', dock,
     [('if (price[i].isDigit() && ++digits == 3) return i;', 'if (price[i].isDigit() && ++digits == 3) return 0;')],
     'DomDelegate.HeatmapRampPriceDigitsAndSizeTextTint'),
    ('no_expiry_republish', dock,
     [('if (m_model->needsRepublish(now)) m_dirty = true;', '')],
     'DomDock.TradesLeaveTheWindowWithoutAnyNewEvent'),
    ('forced_follow', dock,
     [('m_model->publish(book, m_trades, m_follow, now);', 'm_model->publish(book, m_trades, true, now);')],
     'DomDock.ScrollPinsPriceAndPositionThenButtonRestoresFollow'),
    ('paint_per_event', dock,
     [('m_trades.ingest(trade);', 'm_dirty = true; refreshDisplay(); m_trades.ingest(trade);')],
     'DomDock.VisibleUpdatesCoalesceAtFifteenHz'),
    ('hidden_work', dock,
     [('if (!m_displayActive || !isVisible() || minimized()) return;', ''),
      ('m_trades.ingest(trade);', 'm_dirty = true; refreshDisplay(); m_trades.ingest(trade);')],
     'DomDock.HiddenIngestsAllTradesButDoesNoModelOrReplicaReadWork'),
    ('paint_time_as_freshness', dock,
     [('std::chrono::duration_cast<std::chrono::milliseconds>(book.getLastUpdate().time_since_epoch()).count()',
       'QDateTime::currentMSecsSinceEpoch()')],
     'DomDock.QuietConnectedBookRetainsLastChangeAgeAndDetectsDisconnect'),
    # Liquidity Pulse
    ('pulse_threshold_ignored', model,
     [('        if (event.size < pulseThreshold(bid)) continue;\n', '')],
     'DomPulse.AddedPulledAndEatenFlashOnTheirRowsWithTagsAndSmallChangesHide'),
    ('pulse_rows_not_reversed', model,
     [('m_pulseRows[i] = {m_rows[last - i].bid, m_rows[last - i].ask};',
       'm_pulseRows[i] = {m_rows[first + i].bid, m_rows[first + i].ask};')],
     'DomPulse.AddedPulledAndEatenFlashOnTheirRowsWithTagsAndSmallChangesHide'),
    ('pulse_trades_not_matched', model,
     [('const auto status = m_classifier.update(snapshot, trades);', 'const auto status = m_classifier.update(snapshot);')],
     'DomPulse.AddedPulledAndEatenFlashOnTheirRowsWithTagsAndSmallChangesHide'),
    ('pulse_fades_in_three_seconds', header,
     [('static constexpr qint64 PulseFadeMs = 1200;', 'static constexpr qint64 PulseFadeMs = 3000;')],
     'DomPulse.FlashesFadeWithinTheFadeTimeAndRepublishSettles'),
    ('pulse_never_expires', model,
     [('if (f.change && nowMs - f.changeAt >= PulseFadeMs) {', 'if (false) {')],
     'DomPulse.FlashesFadeWithinTheFadeTimeAndRepublishSettles'),
    ('pulse_repaint_never_settles', model,
     [('    m_repaintFirst = Rows;\n    m_repaintLast = -1;\n    m_rows.swap(m_next);', '    m_rows.swap(m_next);')],
     'DomPulse.FlashesFadeWithinTheFadeTimeAndRepublishSettles'),
    ('pulse_fade_not_repainted', model,
     [('if (valueChanged || scaleChanged || pulseRepaint(r)) {', 'if (valueChanged || scaleChanged) {')],
     'DomPulse.FlashesFadeWithinTheFadeTimeAndRepublishSettles'),
    ('pulse_prices_by_row', model,
     [('snapshot.firstPrice = double(m_top - last) * m_tick;', 'snapshot.firstPrice = double(Rows - 1 - last) * m_tick;')],
     'DomPulse.ScrollFollowShiftSymbolSwitchAndWithdrawalProduceNoEvents'),
    ('pulse_symbol_switch_keeps_baseline', model,
     [('    ++m_pulseSeries;\n    resetPulse();\n', '')],
     'DomPulse.ScrollFollowShiftSymbolSwitchAndWithdrawalProduceNoEvents'),
    ('pulse_withdrawal_not_fed', model,
     [('    const auto status = m_classifier.update(snapshot, trades);',
       '    if (snapshot.rows.empty()) return;\n    const auto status = m_classifier.update(snapshot, trades);')],
     'DomPulse.ScrollFollowShiftSymbolSwitchAndWithdrawalProduceNoEvents'),
    ('pulse_footer_without_biggest_pull', model,
     [('    if (t.largestPull && m_tick > 0)', '    if (false)')],
     'DomPulse.FooterSumsEachKindPerSideAndNamesTheBiggestPull'),
    ('pulse_tag_formatted_every_call', model,
     [('    if (m_pulseTextValid[row] & (1u << slot)) return text;\n', '')],
     'DomPulse.PublishAndStepAllocateNothingAfterWarmUp'),
    ('pulse_only_when_dirty', dock,
     [('quiet book.\n        if (m_pulseActive) {', 'quiet book.\n        if (m_pulseActive && m_dirty) {')],
     'DomPulseDock.QuietBookTotalsExpireOnScheduleFedEveryTick'),
    ('pulse_stamped_by_exchange_time', dock,
     [('m_pulseTrades.push_back({trade.price, trade.size, aggressor, pulseClock(false)});',
       'm_pulseTrades.push_back({trade.price, trade.size, aggressor, std::chrono::duration_cast<std::chrono::milliseconds>(trade.timestamp.time_since_epoch()).count()});')],
     'DomPulseDock.TradesAreStampedOnReceiptAndEatTheNextSnapshot'),
    ('pulse_same_millisecond_trade_dropped', dock,
     [('m_lastPulseMs + 1);', 'm_lastPulseMs);')],
     'DomPulseDock.TradesAreStampedOnReceiptAndEatTheNextSnapshot'),
    ('pulse_no_rebaseline', dock,
     [('    m_model->resetPulse();\n    m_footerVersion = UINT64_MAX;', '    m_footerVersion = UINT64_MAX;')],
     'DomPulseDock.HiddenOrDisabledPulseDoesNoWorkAndStartsFromAFreshBaseline'),
    ('pulse_ignores_toggle', dock,
     [('const bool active = m_displayActive && m_pulseOn;', 'const bool active = m_displayActive;')],
     'DomPulseDock.HiddenOrDisabledPulseDoesNoWorkAndStartsFromAFreshBaseline'),
    ('pulse_unpersisted', dock,
     [('    m_pulseOn = settings.value("OrderBookDock/pulse", true).toBool();\n', '')],
     'DomPulseDock.PulseTogglePersistsAndRestores'),
    ('pulse_eaten_in_level_colour', dock,
     [('QColor color = barColor(!bidLevel, 1);', 'QColor color = barColor(bidLevel, 1);')],
     'DomPulseDock.FlashesPaintInTheirColours'),
    ('pulse_pulled_outline_missing', dock,
     [('            painter->fillRect(QRect(x, box.top(), w, 1), grey);\n', '')],
     'DomPulseDock.FlashesPaintInTheirColours'),
    ('pulse_tag_reallocated_per_event', model,
     [('    text.truncate(0);\n    text.append(QLatin1StringView(buffer, length));', '    text = QString::fromLatin1(buffer, length);')],
     'DomPulseDock.FreshFlashesAndTheirPaintAllocateNothingAfterWarmUp'),
    ('pulse_tag_drawn_as_text', dock,
     [('        painter->drawImage(QRectF(x, y, w, glyphs.height), strip,\n                           QRectF(glyphs.offset[i] * scale, 0, w * scale, glyphs.height * scale));',
       '        painter->drawText(QPointF(x, y + glyphs.ascent), QString(c));')],
     'DomPulseDock.FreshFlashesAndTheirPaintAllocateNothingAfterWarmUp'),
    ('pulse_tag_not_drawn', dock,
     [('        painter->drawImage(QRectF(x, y, w, glyphs.height), strip,', '        if (false) painter->drawImage(QRectF(x, y, w, glyphs.height), strip,')],
     'DomPulseDock.FlashesPaintInTheirColours'),
    ('pulse_expiry_keeps_tag', model,
     [('                m_pulseTextValid[r] &= uint8_t(~(1u << side));\n', '')],
     'DomPulse.FlashesFadeWithinTheFadeTimeAndRepublishSettles'),
    ('pulse_exponent_glyph_missing', header,
     [('static constexpr char TagAlphabet[] = "0123456789.+-e<kmb\\xD7";', 'static constexpr char TagAlphabet[] = "0123456789.+-<kmb\\xD7";')],
     'DomPulse.TagAlphabetCoversEverythingFormatSizeEmits'),
    ('pulse_exponent_glyph_not_drawn', header,
     [('static constexpr char TagAlphabet[] = "0123456789.+-e<kmb\\xD7";', 'static constexpr char TagAlphabet[] = "0123456789.+-<kmb\\xD7";')],
     'DomPulseDock.ExponentTagsRenderEveryGlyph'),
]
env = dict(os.environ, CCACHE_READONLY='1', CCACHE_TEMPDIR='/tmp')
output = build / 'dom-validation'
output.mkdir(exist_ok=True)


def compile_target(name):
    with (output / f'{name}-build.txt').open('w') as stream:
        subprocess.run(['cmake', '--build', str(build), '-j', '4', '--target', 'test_dom'],
                       cwd=root, env=env, stdout=stream, stderr=subprocess.STDOUT, check=True)


mutants = [m for m in mutants if m[0].startswith(os.environ.get('DOM_MUTANTS', ''))]
try:
    for name, path, replacements, test in mutants:
        changed = original[path]
        for before, after in replacements:
            assert changed.count(before) == 1, (name, before)
            changed = changed.replace(before, after)
        path.write_text(changed)
        compile_target(name)
        result = subprocess.run([str(build / 'tests/render/test_dom'), f'--gtest_filter={test}'],
                                cwd=root, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        (output / f'{name}-test.txt').write_text(result.stdout)
        assert result.returncode == 1 and '[  FAILED  ]' in result.stdout, (name, result.returncode, result.stdout)
        print(f'MUTATION_REJECTED {name}: compiled; behavior test failed as required', flush=True)
        path.write_text(original[path])
finally:
    for path, content in original.items():
        path.write_text(content)
    compile_target('restored')

subprocess.run([str(build / 'tests/render/test_dom')], cwd=root, env=env, check=True)
print(f'MUTATION_SUMMARY {len(mutants)}/{len(mutants)} rejected; restored suite passed', flush=True)
