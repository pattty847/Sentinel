#include "levelstore/D2Codec.hpp"
#include "levelstore/MinuteSweep.hpp"
#include <gtest/gtest.h>
using namespace sentinel::levelstore;
namespace {
D2Hour fixture() {
    D2Hour h;
    h.start = {0, true, {{true, 99, 1}, {false, 101, 2}}, 0, 0};
    for (int i = 0; i < 60; ++i) {
        MinuteBlock b;
        b.bucketStartMs = i * 60000;
        b.observedAtStart = true;
        b.recorderObservedMs = 60000;
        h.blocks.push_back(b);
    }
    auto &b = h.blocks[0];
    b.messages = {{1000, true, false, ClearCause::None, 0, 0},
                  {1000, true, false, ClearCause::None, 0, 0},
                  {2000, false, false, ClearCause::Invalid, 0, 0},
                  {3000, true, true, ClearCause::None, 0, 0}};
    b.changes = {{0, true, 99, 3}, {1, true, 99, 5}, {3, true, 98, 7}, {3, false, 102, 8}};
    b.inputs = {{0, true, 99, 9}, {0, true, 99, 3}, {1, true, 99, 5}, {3, true, 98, 7}, {3, false, 102, 8}};
    b.recorderObservedMs = 59000;
    auto &next = h.blocks[1];
    next.messages = {{61000, true, false, ClearCause::None, 2, 3}};
    next.changes = {{0, true, 9812, 1234}};
    next.inputs = next.changes;
    for (size_t i = 2; i < h.blocks.size(); ++i) {
        h.blocks[i].priceDigits = 2;
        h.blocks[i].sizeDigits = 3;
    }
    return h;
}
} // namespace
TEST(D2Codec, LosslessMessagesScalesResetsClearsAndEveryInput) {
    const auto original = fixture();
    const auto encoded = encodeD2(original);
    const auto decoded = decodeD2(encoded);
    EXPECT_EQ(decoded, original);
    EXPECT_EQ(encodeD2(decoded), encoded);
    const auto end = endImage(decoded);
    EXPECT_EQ(end.priceDigits, 2);
    EXPECT_EQ(end.sizeDigits, 3);
    EXPECT_EQ(end.levels, (std::vector<BookLevel>{{true, 9800, 7000}, {true, 9812, 1234}, {false, 10200, 8000}}));
}
TEST(D2Codec, FullInt64PriceAndQuantityRemainExact) {
    D2Hour h;
    h.start = {0, false, {{true, INT64_MAX - 1, INT64_MAX}, {false, INT64_MAX, 1}}, 0, 0};
    EXPECT_EQ(decodeD2(encodeD2(h)), h);
}
TEST(D2Codec, CorruptionAndTruncationNeverDecode) {
    const auto bytes = encodeD2(fixture());
    for (size_t i = 0; i < bytes.size(); ++i) {
        auto bad = bytes;
        bad[i] ^= 1;
        EXPECT_THROW(decodeD2(bad), std::exception) << i;
    }
    for (size_t i = 0; i < bytes.size(); ++i)
        EXPECT_THROW(decodeD2(std::span(bytes).first(i)), std::exception) << i;
}
TEST(D2Codec, RemovedLargeValuesDoNotOverflowALaterFinerScale) {
    D2Hour h;
    h.start = {0, false, {{true, INT64_MAX, INT64_MAX}}, 0, 0};
    MinuteBlock b;
    b.messages = {{1, false, false, ClearCause::Invalid, 0, 0}, {2, true, true, ClearCause::None, 8, 8}};
    b.changes = {{1, true, 100, 100}, {1, false, 200, 100}};
    h.blocks.push_back(b);
    const auto decoded = decodeD2(encodeD2(h));
    EXPECT_EQ(decoded, h);
    EXPECT_EQ(endImage(decoded).levels, (std::vector<BookLevel>{{true, 100, 100}, {false, 200, 100}}));
}

#include "levelstore/BalSummary.hpp"
TEST(BalSummary, IntegerCellsBandsAndRoundTripAreExact) {
    D2Hour h;
    h.start = {0, true, {{true, 9900, 100000000}, {false, 10100, 200000000}}, 2, 8};
    for (int i = 0; i < 60; ++i) {
        MinuteBlock b;
        b.bucketStartMs = i * 60000;
        b.observedAtStart = true;
        b.recorderObservedMs = 60000;
        b.priceDigits = 2;
        b.sizeDigits = 8;
        h.blocks.push_back(b);
    }
    auto &b = h.blocks[0];
    b.messages = {{1000, true, false, ClearCause::None, 2, 8},
                  {1000, true, false, ClearCause::None, 2, 8},
                  {2000, true, false, ClearCause::None, 2, 8}};
    b.changes = {{0, true, 9900, 500000000}, {1, true, 9900, 300000000}, {2, true, 9900, 0}};
    const SummaryLevel level{60000, {1, 0}, 300, false};
    const auto nodes = buildSummary({h}, {level});
    ASSERT_EQ(nodes.size(), 60);
    const auto &n = nodes.front();
    EXPECT_EQ(n.coveredMs, 60000);
    EXPECT_EQ(n.bandLo, 97);
    EXPECT_EQ(n.bandHi, 105);
    ASSERT_EQ(n.rows.size(), 2);
    EXPECT_EQ(n.rows[0], (BinRow{true, 99, Wide(400000000000LL), 2000, Wide(500000000), 0}));
    EXPECT_EQ(n.rows[1], (BinRow{false, 101, Wide(12000000000000LL), 60000, Wide(200000000), Wide(200000000)}));
    EXPECT_EQ(decodeSummary(encodeSummary(nodes)), nodes);
    auto twap = level;
    twap.twapOnly = true;
    const auto tw = buildSummary({h}, {twap});
    EXPECT_EQ(tw[0].rows[0].integral, n.rows[0].integral);
    EXPECT_EQ(tw[0].rows[0].peak, 0);
    EXPECT_EQ(tw[0].rows[0].presenceMs, 0);
}
TEST(BalSummary, ScaleTransitionPreservesPhysicalBinsAndIntegral) {
    D2Hour h;
    h.start = {0, true, {{true, 99, 1}, {false, 101, 2}}, 0, 0};
    MinuteBlock b;
    b.observedAtStart = true;
    b.messages = {{1000, true, false, ClearCause::None, 2, 3}};
    b.changes = {{0, true, 9901, 1}};
    h.blocks.push_back(b);
    const auto nodes = buildSummary({h}, {{60000, {1, 0}, 0, false}});
    ASSERT_EQ(nodes.size(), 1);
    EXPECT_EQ(nodes[0].rows[0].bin, 99);
    EXPECT_EQ(nodes[0].rows[0].last, 1001);
    EXPECT_EQ(nodes[0].rows[0].integral, Wide(60059000));
    EXPECT_EQ(nodes[0].sizeDigits, 3);
}
TEST(BalSummary, PublishedLadderHasProductSpecificNativeResolution) {
    const auto btc = bal1mLevels("BTC-USD"), pepe = bal1mLevels("PEPE-USD");
    EXPECT_EQ(btc.size(), 29);
    EXPECT_EQ(pepe.size(), 18);
    EXPECT_EQ(btc.front(), (SummaryLevel{60000, {1, 0}, 300, true}));
    EXPECT_EQ(pepe.front(), (SummaryLevel{60000, {1, 8}, 3000, true}));
}
TEST(BalSummary, StaticAndTwapOnlyCellsUseCompactExactEncoding) {
    SummaryNode n;
    n.level = {60000, {1, 0}, 0, false};
    n.coveredMs = 60000;
    for (int i = 0; i < 1000; ++i)
        n.rows.push_back({true, i, Wide(60000000000LL), 60000, Wide(1000000), Wide(1000000)});
    const auto compact = encodeSummary({n});
    EXPECT_LT(compact.size(), 6000);
    EXPECT_EQ(decodeSummary(compact), (std::vector<SummaryNode>{n}));
    for (auto &row : n.rows) {
        row.integral += 1;
        row.presenceMs = 10;
    }
    const auto full = encodeSummary({n});
    n.level.twapOnly = true;
    for (auto &row : n.rows) {
        row.peak = 0;
        row.presenceMs = 0;
    }
    const auto twap = encodeSummary({n});
    EXPECT_LT(twap.size(), full.size());
    EXPECT_EQ(decodeSummary(twap), (std::vector<SummaryNode>{n}));
}

// Deliberately slow oracle: reconstruct all levels and sum every bin at every
// message. Uses built-in 128-bit arithmetic, independently of MinuteSweep/Wide.
TEST(BalSummary, EveryNodeMatchesIndependentMessageStateOracle) {
    using Key = std::pair<bool, int64_t>;
    struct Cell {
        __int128 integral = 0, peak = 0, last = 0;
        int64_t presence = 0;
    };
    D2Hour h;
    h.start = {0, true, {{true, 990, 10}, {false, 1010, 20}}, 1, 1};
    std::map<Key, int64_t> live{{{true, 990}, 10}, {{false, 1010}, 20}};
    bool observed = true;
    unsigned pd = 1, qd = 1;
    std::vector<SummaryNode> expected;
    for (int minute = 0; minute < 60; ++minute) {
        MinuteBlock b;
        b.bucketStartMs = minute * 60000;
        b.priceDigits = pd;
        b.sizeDigits = qd;
        b.observedAtStart = observed;
        for (int j = 0; j < 4; ++j) {
            const auto index = b.messages.size();
            const bool clear = (minute % 7 == 0 && j == 2), reset = j == 3;
            const auto nextPd = minute >= 30 ? 2u : 1u, nextQd = minute >= 30 ? 2u : 1u;
            b.messages.push_back({b.bucketStartMs + (j == 1 ? 1000 : j * 1000), !clear, reset,
                                  clear ? ClearCause::Invalid : ClearCause::None, nextPd, nextQd});
            if (!clear) {
                const int scale = nextPd == 2 ? 10 : 1;
                b.changes.push_back({uint32_t(index), true, 990 * scale, int64_t((minute + j) % 9) * scale});
                b.changes.push_back({uint32_t(index), false, 1010 * scale, int64_t(20 + j) * scale});
                if (j == 0)
                    b.changes.push_back({uint32_t(index), true, 991 * scale, 3 * scale});
            }
        }
        // Normalize the oracle's book and events to this minute's final scale
        // before integrating; this is independent of production scale rebasing.
        const unsigned fp = b.messages.back().priceDigits, fq = b.messages.back().sizeDigits;
        std::map<Key, int64_t> normalized;
        for (auto [key, q] : live)
            normalized[{key.first, key.second * (fp > pd ? 10 : 1)}] = q * (fq > qd ? 10 : 1);
        live = std::move(normalized);
        pd = fp;
        qd = fq;
        SummaryNode n;
        n.level = {60000, {1, 0}, 0, false};
        n.startMs = b.bucketStartMs;
        n.priceDigits = pd;
        n.sizeDigits = qd;
        std::map<Key, Cell> cells;
        const auto sums = [&] {
            std::map<Key, __int128> result;
            for (auto [key, q] : live)
                if (q)
                    result[{key.first, key.second / (pd == 2 ? 100 : 10)}] += q;
            return result;
        };
        const auto peak = [&] {
            if (observed)
                for (auto [key, q] : sums())
                    cells[key].peak = std::max(cells[key].peak, q);
        };
        const auto accrue = [&](int64_t dt) {
            if (!observed)
                return;
            n.coveredMs += dt;
            for (auto [key, q] : sums()) {
                cells[key].integral += q * dt;
                if (q)
                    cells[key].presence += dt;
            }
        };
        peak();
        int64_t previous = b.bucketStartMs;
        size_t change = 0;
        for (size_t i = 0; i < b.messages.size(); ++i) {
            const auto &m = b.messages[i];
            accrue(m.timeMs - previous);
            previous = m.timeMs;
            if (m.reset || m.clear != ClearCause::None)
                live.clear();
            while (change < b.changes.size() && b.changes[change].message == i) {
                const auto &c = b.changes[change++];
                if (c.size)
                    live[{c.isBid, c.units}] = c.size;
                else
                    live.erase({c.isBid, c.units});
            }
            observed = m.observed;
            peak();
        }
        accrue(b.bucketStartMs + 60000 - previous);
        for (auto [key, q] : sums())
            cells[key].last = q;
        for (auto [key, c] : cells)
            if (c.integral || c.peak || c.last || c.presence)
                n.rows.push_back({key.first, key.second, Wide(c.integral), c.presence, Wide(c.peak), Wide(c.last)});
        std::sort(n.rows.begin(), n.rows.end(),
                  [](const auto &a, const auto &b) { return a.isBid != b.isBid ? a.isBid : a.bin < b.bin; });
        b.recorderObservedMs = n.coveredMs;
        h.blocks.push_back(b);
        expected.push_back(n);
    }
    const SummaryLevel one{60000, {1, 0}, 0, false};
    EXPECT_EQ(buildSummary({h}, {one}), expected);
    for (int duration : {5, 15, 60}) {
        std::vector<SummaryNode> folded;
        for (int begin = 0; begin < 60; begin += duration) {
            SummaryNode n;
            n.level = one;
            n.level.durationMs = duration * 60000;
            n.startMs = begin * 60000;
            n.priceDigits = expected[begin + duration - 1].priceDigits;
            n.sizeDigits = expected[begin + duration - 1].sizeDigits;
            std::map<Key, BinRow> cells;
            for (int k = begin; k < begin + duration; ++k) {
                const auto &m = expected[k];
                n.coveredMs += m.coveredMs;
                for (auto &[key, row] : cells)
                    row.last = 0;
                for (auto row : m.rows) {
                    const auto factor = m.sizeDigits < n.sizeDigits ? 10 : 1;
                    row.integral *= factor;
                    row.peak *= factor;
                    row.last *= factor;
                    auto &target = cells[{row.isBid, row.bin}];
                    target.isBid = row.isBid;
                    target.bin = row.bin;
                    target.integral += row.integral;
                    target.presenceMs += row.presenceMs;
                    target.peak = std::max(target.peak, row.peak);
                    target.last = row.last;
                }
            }
            for (auto [key, row] : cells)
                n.rows.push_back(row);
            std::sort(n.rows.begin(), n.rows.end(),
                      [](const auto &a, const auto &b) { return a.isBid != b.isBid ? a.isBid : a.bin < b.bin; });
            folded.push_back(n);
        }
        auto level = one;
        level.durationMs = duration * 60000;
        EXPECT_EQ(buildSummary({h}, {level}), folded);
    }
}

TEST(BalSummary, FourHourAndDayNodesMatchClosedFormIntegerIntegrals) {
    std::vector<D2Hour> hours;
    for (int hour = 0; hour < 24; ++hour) {
        D2Hour h;
        h.start = {hour * 3600000LL, true, {{true, 9900, 100000000}, {false, 10100, 200000000}}, 2, 8};
        for (int m = 0; m < 60; ++m) {
            MinuteBlock b;
            b.bucketStartMs = h.start.atMs + m * 60000;
            b.observedAtStart = true;
            b.recorderObservedMs = 60000;
            b.priceDigits = 2;
            b.sizeDigits = 8;
            h.blocks.push_back(b);
        }
        hours.push_back(h);
    }
    auto levels = bal1mLevels("BTC-USD");
    std::erase_if(levels, [](const auto &l) { return l.durationMs <= 3600000; });
    const auto nodes = buildSummary(hours, levels);
    ASSERT_EQ(nodes.size(), 30);
    for (const auto &n : nodes) {
        ASSERT_EQ(n.rows.size(), 2);
        EXPECT_EQ(n.coveredMs, n.level.durationMs);
        EXPECT_EQ(n.priceDigits, 2);
        EXPECT_EQ(n.sizeDigits, 8);
        for (const auto &row : n.rows) {
            const int64_t quantity = row.isBid ? 100000000 : 200000000;
            EXPECT_EQ(row.bin, (row.isBid ? 99 : 101) / n.level.resolution.mantissa);
            EXPECT_EQ(row.integral, Wide(quantity) * n.level.durationMs);
            EXPECT_EQ(row.peak, quantity);
            EXPECT_EQ(row.last, quantity);
            EXPECT_EQ(row.presenceMs, n.level.durationMs);
        }
    }
    EXPECT_EQ(decodeSummary(encodeSummary(nodes)), nodes);
}
