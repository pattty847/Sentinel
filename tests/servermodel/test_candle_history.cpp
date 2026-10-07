#include <gtest/gtest.h>
#include "marketdata/rest/CoinbaseRestClient.hpp"
#include <future>

namespace {
CandleFetchResult fixture(int64_t start, int64_t end, const std::string& granularity) {
    int64_t step = 0;
    for (const int64_t tf : {60, 300, 900, 1800, 3600, 7200, 14400, 21600, 86400})
        if (CoinbaseRestClient::granularityFromSeconds(tf) == granularity) step = tf;
    EXPECT_GT(step, 0);
    CandleFetchResult result{true, {}, {}};
    if (step <= 0) return result;
    for (int64_t t = end - step; t >= start; t -= step)
        result.candles.push_back({t * 1000, 10, 14, 9, 12, 2, 0, false});
    return result; // Coinbase returns newest first and may include the end bucket
}
}

TEST(CandleHistoryCache, EveryNativeTimeframeUsesOneCallFor350BarsAndRepeatUsesZero) {
    for (const int64_t tf : {60, 300, 900, 1800, 3600, 7200, 14400, 21600, 86400}) {
        CandleHistoryCache cache;
        int calls = 0;
        auto fetch = [&](int64_t start, int64_t end, const std::string& granularity) {
            ++calls;
            EXPECT_EQ(granularity, *CoinbaseRestClient::granularityFromSeconds(tf));
            EXPECT_EQ(end - start, 350 * tf);
            return fixture(start, end, granularity);
        };
        for (int repeat = 0; repeat < 2; ++repeat) {
            auto result = cache.fetch("BTC-USD", tf, 1000 * tf, 1350 * tf, 2000 * tf, fetch);
            ASSERT_TRUE(result.ok) << result.error;
            ASSERT_EQ(result.candles.size(), 350u);
            EXPECT_EQ(result.candles.front().timestamp_ms, 1000 * tf * 1000);
            EXPECT_EQ(result.candles.back().timestamp_ms, 1349 * tf * 1000);
        }
        EXPECT_EQ(calls, 1);
    }
}

TEST(CandleHistoryCache, NonNativeUsesLargestDivisorAcrossSourcePagesOnUtcBoundaries) {
    CandleHistoryCache cache;
    constexpr int64_t tf = 3 * 3600;
    int calls = 0;
    auto fetch = [&](int64_t start, int64_t end, const std::string& granularity) {
        ++calls;
        EXPECT_EQ(granularity, "ONE_HOUR");
        EXPECT_LE((end - start) / 3600, 350);
        return fixture(start, end, granularity);
    };
    auto result = cache.fetch("BTC-USD", tf, 1000 * tf, 1350 * tf, 2000 * tf, fetch);
    ASSERT_TRUE(result.ok);
    ASSERT_EQ(result.candles.size(), 350u);
    EXPECT_EQ(calls, 3);
    for (const auto& bar : result.candles) {
        EXPECT_EQ(bar.timestamp_ms % (tf * 1000), 0);
        EXPECT_EQ(bar.volume, 6);
        EXPECT_TRUE(bar.is_closed);
    }
}

TEST(CandleHistoryCache, EmptyRangesCacheButFailuresRetryAndProductsStaySeparate) {
    CandleHistoryCache cache;
    int calls = 0;
    auto fetch = [&](int64_t, int64_t, const std::string&) {
        ++calls;
        return CandleFetchResult{calls != 1, calls == 1 ? "failed" : "", {}};
    };
    EXPECT_FALSE(cache.fetch("BTC-USD", 900, 900, 1800, 3600, fetch).ok);
    EXPECT_TRUE(cache.fetch("BTC-USD", 900, 900, 1800, 3600, fetch).ok);
    EXPECT_TRUE(cache.fetch("BTC-USD", 900, 900, 1800, 3600, fetch).ok);
    EXPECT_EQ(calls, 2);
    EXPECT_TRUE(cache.fetch("ETH-USD", 900, 900, 1800, 3600, fetch).ok);
    EXPECT_EQ(calls, 3);
}

TEST(CandleHistoryCache, OverlapFetchesOnlyMissingClosedBucketsAndNeverCachesForming) {
    CandleHistoryCache cache;
    int calls = 0;
    auto fetch = [&](int64_t start, int64_t end, const std::string& granularity) {
        ++calls;
        EXPECT_LE(end, calls <= 2 ? 9000 : 9900);
        auto page = fixture(start, end, granularity);
        page.candles.push_back({end * 1000, 999, 999, 999, 999, 999});
        return page;
    };
    auto first = cache.fetch("BTC-USD", 900, 7200, 9900, 9450, fetch);
    ASSERT_EQ(first.candles.size(), 2u);
    EXPECT_TRUE(first.candles.back().is_closed);
    auto overlap = cache.fetch("BTC-USD", 900, 6300, 9900, 9450, fetch);
    ASSERT_EQ(overlap.candles.size(), 3u);
    EXPECT_EQ(calls, 2);
    auto repeat = cache.fetch("BTC-USD", 900, 6300, 9900, 9450, fetch);
    EXPECT_EQ(repeat.candles.size(), 3u);
    EXPECT_EQ(calls, 2);
    auto rolled = cache.fetch("BTC-USD", 900, 6300, 10800, 9900, fetch);
    ASSERT_EQ(rolled.candles.size(), 4u);
    EXPECT_EQ(calls, 3);
    EXPECT_EQ(rolled.candles.back().timestamp_ms, 9000000);
    EXPECT_EQ(rolled.candles.back().close, 12); // not the old forming snapshot
}

TEST(CandleHistoryCache, ConcurrentRequestsShareFetchAndCancellationDoesNotPoisonCache) {
    CandleHistoryCache cache;
    int calls = 0;
    auto fetch = [&](int64_t start, int64_t end, const std::string& granularity) {
        ++calls;
        return fixture(start, end, granularity);
    };
    EXPECT_FALSE(cache.fetch("BTC-USD", 900, 900, 1800, 3600, fetch, [] { return true; }).ok);
    EXPECT_EQ(calls, 0);
    auto request = [&] { return cache.fetch("BTC-USD", 900, 900, 1800, 3600, fetch); };
    auto a = std::async(std::launch::async, request);
    auto b = std::async(std::launch::async, request);
    EXPECT_TRUE(a.get().ok);
    EXPECT_TRUE(b.get().ok);
    EXPECT_EQ(calls, 1);
}

TEST(CandleHistoryCache, BoundedRetentionEvictsOldBucketsAndLeastRecentlyUsedSeries) {
    CandleHistoryCache cache;
    int calls = 0;
    auto fetch = [&](int64_t start, int64_t end, const std::string& granularity) {
        ++calls;
        return fixture(start, end, granularity);
    };
    for (int page = 0; page < 24; ++page)
        ASSERT_TRUE(cache.fetch("BTC-USD", 60, (1 + page * 350) * 60,
            (351 + page * 350) * 60, 10000 * 60, fetch).ok);
    EXPECT_EQ(calls, 24);
    EXPECT_TRUE(cache.fetch("BTC-USD", 60, 8400 * 60, 8401 * 60, 10000 * 60, fetch).ok);
    EXPECT_EQ(calls, 24);
    EXPECT_TRUE(cache.fetch("BTC-USD", 60, 60, 120, 10000 * 60, fetch).ok);
    EXPECT_EQ(calls, 25);
    for (size_t i = 0; i < CandleHistoryCache::kMaxSeries; ++i)
        ASSERT_TRUE(cache.fetch("PRODUCT-" + std::to_string(i), 60, 60, 120, 10000 * 60, fetch).ok);
    const int before = calls;
    EXPECT_TRUE(cache.fetch("BTC-USD", 60, 8400 * 60, 8401 * 60, 10000 * 60, fetch).ok);
    EXPECT_EQ(calls, before + 1);
}
