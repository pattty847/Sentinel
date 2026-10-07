#include <gtest/gtest.h>
#include "marketdata/rest/CoinbaseRestClient.hpp"
#include <future>
#include <atomic>

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
        EXPECT_LE(end, calls <= 3 ? 9000 : 9900);
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
    EXPECT_EQ(calls, 3); // the most recently closed bucket is still settling
    auto rolled = cache.fetch("BTC-USD", 900, 6300, 10800, 9900, fetch);
    ASSERT_EQ(rolled.candles.size(), 4u);
    EXPECT_EQ(calls, 4);
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

TEST(CandleHistoryCache, RecentlyClosedMissingAndPartialBucketsRefetchUntilSettled) {
    for (const int64_t tf : {60, 900, 10800}) {
        for (bool omitted : {false, true}) {
            SCOPED_TRACE(std::to_string(tf) + (omitted ? " missing" : " partial"));
            CandleHistoryCache cache;
            const int64_t end = 100 * tf;
            const int64_t source = CandleHistoryCache::sourceSeconds(tf);
            int calls = 0;
            auto fetch = [&](int64_t start, int64_t stop, const std::string& granularity) {
                ++calls;
                if (calls > 1) EXPECT_EQ(start, end - tf); // older settled bucket stays cached
                auto result = fixture(start, stop, granularity);
                if (calls == 1) {
                    std::erase_if(result.candles, [&](const auto& bar) {
                        return omitted && bar.timestamp_ms >= (end - tf) * 1000;
                    });
                    for (auto& bar : result.candles)
                        if (bar.timestamp_ms >= (end - tf) * 1000) bar.close = 11;
                }
                return result;
            };
            const auto first = cache.fetch("BTC-USD", tf, end - 2 * tf, end, end + 1, fetch);
            ASSERT_TRUE(first.ok);
            ASSERT_EQ(first.candles.size(), omitted ? 1u : 2u);
            if (!omitted) EXPECT_EQ(first.candles.back().close, 11);
            const auto second = cache.fetch("BTC-USD", tf, end - 2 * tf, end, end + 2, fetch);
            ASSERT_TRUE(second.ok);
            ASSERT_EQ(second.candles.size(), 2u);
            EXPECT_EQ(second.candles.back().close, 12);
            EXPECT_EQ(calls, 2); // neither an empty slot nor a partial snapshot was retained
            const auto settled = cache.fetch("BTC-USD", tf, end - 2 * tf, end, end + source, fetch);
            ASSERT_TRUE(settled.ok);
            EXPECT_EQ(calls, 3);
            EXPECT_TRUE(cache.fetch("BTC-USD", tf, end - 2 * tf, end, end + source, fetch).ok);
            EXPECT_EQ(calls, 3);
        }
    }
}

TEST(CandleHistoryCache, UnrelatedCacheHitReturnsWhileProviderIsBlocked) {
    CandleHistoryCache cache;
    int hitSeriesCalls = 0;
    auto hitFetcher = [&](int64_t start, int64_t end, const std::string& granularity) {
        ++hitSeriesCalls;
        return fixture(start, end, granularity);
    };
    ASSERT_TRUE(cache.fetch("B", 900, 900, 1800, 3600, hitFetcher).ok);
    std::promise<void> entered, release;
    const auto released = release.get_future().share();
    auto miss = std::async(std::launch::async, [&] {
        return cache.fetch("A", 900, 900, 1800, 3600,
            [&](int64_t start, int64_t end, const std::string& granularity) {
                entered.set_value();
                released.wait();
                return fixture(start, end, granularity);
            });
    });
    entered.get_future().wait();
    auto hit = std::async(std::launch::async, [&] {
        return cache.fetch("B", 900, 900, 1800, 3600, hitFetcher);
    });
    const auto hitStatus = hit.wait_for(std::chrono::milliseconds(200));
    release.set_value(); // release even on regression, so futures cannot deadlock the test
    EXPECT_EQ(hitStatus, std::future_status::ready);
    EXPECT_TRUE(hit.get().ok);
    EXPECT_TRUE(miss.get().ok);
    EXPECT_EQ(hitSeriesCalls, 1);
}

TEST(CandleHistoryCache, ConcurrentRecentTailSharesFlightButLaterRequestRefetches) {
    CandleHistoryCache cache;
    std::atomic<int> calls{0};
    std::promise<void> entered, release, waiterJoined;
    const auto released = release.get_future().share();
    auto fetch = [&](int64_t start, int64_t end, const std::string& granularity) {
        if (++calls == 1) {
            entered.set_value();
            released.wait();
        }
        return fixture(start, end, granularity);
    };
    auto producer = std::async(std::launch::async, [&] {
        return cache.fetch("BTC-USD", 900, 900, 1800, 1801, fetch);
    });
    entered.get_future().wait();
    auto waiter = std::async(std::launch::async, [&] {
        int checks = 0;
        return cache.fetch("BTC-USD", 900, 900, 1800, 1801, fetch, [&] {
            if (++checks == 2) waiterJoined.set_value(); // second check is in the wait loop
            return false;
        });
    });
    const auto joined = waiterJoined.get_future().wait_for(std::chrono::seconds(1));
    release.set_value();
    EXPECT_EQ(joined, std::future_status::ready);
    EXPECT_TRUE(producer.get().ok);
    EXPECT_TRUE(waiter.get().ok);
    EXPECT_EQ(calls, 1);
    EXPECT_TRUE(cache.fetch("BTC-USD", 900, 900, 1800, 1802, fetch).ok);
    EXPECT_EQ(calls, 2);
}

TEST(CandleHistoryCache, CancelledProducerDoesNotCancelAnotherSessionsWaiter) {
    CandleHistoryCache cache;
    std::promise<void> entered, release, waiterJoined;
    const auto released = release.get_future().share();
    std::atomic<bool> cancelProducer{false};
    int producerCalls = 0, waiterCalls = 0;
    auto producer = std::async(std::launch::async, [&] {
        return cache.fetch("BTC-USD", 900, 900, 1800, 3600,
            [&](int64_t start, int64_t end, const std::string& granularity) {
                ++producerCalls;
                entered.set_value();
                released.wait();
                cancelProducer = true; // session A disconnects during provider I/O
                return fixture(start, end, granularity);
            }, [&] { return cancelProducer.load(); });
    });
    entered.get_future().wait();
    auto waiter = std::async(std::launch::async, [&] {
        int checks = 0;
        return cache.fetch("BTC-USD", 900, 900, 1800, 3600,
            [&](int64_t start, int64_t end, const std::string& granularity) {
                ++waiterCalls;
                return fixture(start, end, granularity);
            }, [&] {
                if (++checks == 2) waiterJoined.set_value();
                return false; // session B stays connected
            });
    });
    const auto joined = waiterJoined.get_future().wait_for(std::chrono::seconds(1));
    release.set_value();
    EXPECT_EQ(joined, std::future_status::ready);
    EXPECT_EQ(producer.get().error, "cancelled");
    const auto result = waiter.get();
    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_EQ(result.candles.size(), 1u);
    EXPECT_EQ(result.candles.front().close, 12);
    EXPECT_EQ(producerCalls, 1);
    EXPECT_EQ(waiterCalls, 1);
}

TEST(CandleHistoryCache, ExceptionAfterFetchReleasesFlightForLaterRequest) {
    CandleHistoryCache cache;
    int calls = 0;
    auto fetch = [&](int64_t start, int64_t end, const std::string& granularity) {
        ++calls;
        return fixture(start, end, granularity);
    };
    // The post-fetch cancellation check also models an exception during merge
    // or storage, outside fetchRange's error conversion, without a test-only hook.
    EXPECT_THROW(cache.fetch("BTC-USD", 900, 900, 1800, 3600, fetch, [&]() -> bool {
        if (calls != 0) throw std::runtime_error("injected after fetch");
        return false;
    }), std::runtime_error);
    EXPECT_EQ(calls, 1);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    const auto later = cache.fetch("BTC-USD", 900, 900, 1800, 3600, fetch, [&] {
        return std::chrono::steady_clock::now() >= deadline; // bounds a leaked-flight regression
    });
    ASSERT_TRUE(later.ok) << later.error;
    ASSERT_EQ(later.candles.size(), 1u);
    EXPECT_EQ(calls, 2);
}
