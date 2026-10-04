#!/usr/bin/env python3
"""Fail-without-fix checks. Run inside build-queue; restores source in finally.

python3 tests/render/check_dom_mutations.py [build/mac-clang]
Uses installed dependencies; each mutant must COMPILE and fail its behavior test.
"""
from pathlib import Path
import os
import subprocess
import sys

root = Path(__file__).resolve().parents[2]
build = (root / (sys.argv[1] if len(sys.argv) > 1 else 'build/mac-clang')).resolve()
model = root / 'libs/gui/models/DomModel.cpp'
dock = root / 'libs/gui/widgets/OrderBookDock.cpp'
replica = root / 'libs/gui/datasources/RemoteGridDataSource.cpp'
original = {p: p.read_text() for p in (model, dock, replica)}
mutants = [
    ('fixed_dom_columns', dock,
     [('const int core = width - (counts ? countWidth * 3 : delta ? countWidth : 0);', 'const int core = 600;')],
     'DomDock.NarrowLadderFitsWithoutScrollingAndRestoresExecutionColumns'),
    ('unshortened_quantities', dock,
     [('if (metrics.horizontalAdvance(full) <= width) return full;', 'return full;')],
     'DomDelegate.CompactNumbersAreWholeValuesAndPricesKeepTheirPrecision'),
    ('duplicate_bucket', model,
     [('return m_top - index.row();', 'return m_top - (index.row() == 1001 ? 1000 : index.row());')],
     'DomModel.SameBucketUniqueBestAskStableEmptyRowsAndAlignment'),
    ('absolute_trade_buckets', model,
     [('std::optional<qint64>(*origin + static_cast<qint64>(offset))',
       'std::optional<qint64>(std::llround(trade.price / tick))')],
     'DomModel.ExecutionsMatchReplicaTruncationAtDecimalBoundariesWithNonzeroOrigin'),
    ('false_replica_precision', replica,
     [('const double tickSize = serverTick;', 'const double tickSize = serverTick / 10;')],
     'DomModel.ServerQuantisedSnapshotsNeverAdvertiseFalsePrecision'),
    ('coarse_positive_prices_rendered', model,
     [(' || (mid > 0 && tick > mid * 0.01)', '')],
     'DomModel.ServerQuantisedSnapshotsNeverAdvertiseFalsePrecision'),
    ('coarsened_eth_replica', replica,
     [('const double tickSize = serverTick;',
       'const double tickSize = symbol == "ETH-USD" ? (minPrice < 3000 ? 0.2 : 0.5) : serverTick;')],
     'DomModel.EthReplicaPreservesAdjacentServerLevelsAndBookTop'),
    ('quiet_book_marked_stale', model,
     [('if (connected == true) return QStringLiteral("Connected · %1").arg(age);',
       'if (nowMs - lastChangeMs >= 3000) return QStringLiteral("Stale · %1").arg(age); if (connected == true) return QStringLiteral("Connected · %1").arg(age);')],
     'DomFreshness.LiveQuietBookHasChangeAgeButDoesNotBecomeStale:DomDock.QuietConnectedBookRetainsLastChangeAgeAndDetectsDisconnect'),
    ('unknown_as_sell', model,
     [('else if (trade.side == AggressorSide::Sell) ++row.sells;', 'else ++row.sells;')],
     'DomModel.RingCountsEveryEventUnknownNeverSellsAndCountsAreNotVolume'),
    ('broken_ring_eviction', model,
     [('m_next = (m_next + 1) % Capacity;', 'm_next = (m_next + 1) % (Capacity - 1);')],
     'DomModel.RingCountsEveryEventUnknownNeverSellsAndCountsAreNotVolume'),
    ('forced_follow', dock,
     [('m_model->publish(book, m_trades, m_follow);', 'm_model->publish(book, m_trades, true);')],
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
]
env = dict(os.environ, CCACHE_READONLY='1', CCACHE_TEMPDIR='/tmp')
output = build / 'dom-validation'
output.mkdir(exist_ok=True)


def compile_target(name):
    with (output / f'{name}-build.txt').open('w') as stream:
        subprocess.run(['cmake', '--build', str(build), '-j', '4', '--target', 'test_dom'],
                       cwd=root, env=env, stdout=stream, stderr=subprocess.STDOUT, check=True)


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
