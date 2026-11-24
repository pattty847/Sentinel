// SCK1 golden bytes are literal wire fixtures, independent of the encoder.
// Payload semantics/count bounds are covered by FlowChunkTests; this tests the
// transport lengths, hash, compression and SHC1/SCK1 SHE1 dispatch boundary.
#include "protocol/ChunkFrame.hpp"
#include "protocol/FlowChunkWire.hpp"
#include "flow/FlowChunkCodec.hpp"
#include "servermodel/Hmc2Store.hpp"
#include <gtest/gtest.h>
#include <random>
#include <zstd.h>
namespace w = protocol::chunks;
namespace {
w::Frame example() {
    w::Frame f;
    f.key = {"coinbase:BTC-USD", "flow.summary", 60000, 0, 946684800000};
    f.endMs = f.committedThroughMs = 946688400000;
    f.epoch = 0x0807060504030201;
    f.payload = {0, 1, 0, 0, 60, 0, 1, 0};
    return f;
}
std::vector<uint8_t> unhex(std::string_view text) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i < text.size(); i += 2) out.push_back(uint8_t(std::stoul(std::string(text.substr(i, 2)), nullptr, 16)));
    return out;
}
void put(std::vector<uint8_t>& b, size_t at, uint64_t value, size_t n) {
    for (size_t i = 0; i < n; ++i) b.at(at + i) = uint8_t(value >> (8 * i));
}
constexpr size_t Extent = 62, Hash = 94, Version = 102, Codec = 104, Raw = 105, Stored = 109, Body = 113;
}
TEST(ChunkFrame, GoldenChunkNotModifiedError) {
    auto f = example();
    const auto chunk = unhex("53434b310100010110636f696e626173653a4254432d5553440c666c6f772e73756d6d61727960ea000000000000000000000000000000accf6adc000000809a066bdc000000809a066bdc00000000000000000000000102030405060708cb21d17841075e790100000800000008000000000100003c000100");
    const auto nm = unhex("53434b310100030110636f696e626173653a4254432d5553440c666c6f772e73756d6d61727960ea000000000000000000000000000000accf6adc000000809a066bdc000000809a066bdc00000000000000000000000102030405060708cb21d17841075e79");
    const auto error = unhex("53434b310100040110636f696e626173653a4254432d5553440c666c6f772e73756d6d61727960ea000000000000000000000000000000accf6adc00000002000b756e617661696c61626c65");
    EXPECT_EQ(w::encode(f), chunk);
    const auto decoded = w::decode(chunk);
    EXPECT_EQ(decoded.payload, f.payload); EXPECT_EQ(decoded.epoch, f.epoch);
    EXPECT_EQ(decoded.contentHash, w::contentHash(f)); EXPECT_EQ(decoded.codec, 0);
    f.contentHash = decoded.contentHash; f.kind = w::Kind::NotModified;
    EXPECT_EQ(w::encode(f), nm); EXPECT_EQ(w::decode(nm).contentHash, f.contentHash);
    f.kind = w::Kind::Error; f.error = w::Error::Unavailable; f.message = "unavailable";
    EXPECT_EQ(w::encode(f), error); EXPECT_EQ(w::decode(error).error, w::Error::Unavailable);
    for (const auto& bytes : {chunk, nm, error}) {
        auto env = w::decodeEnvelope(heatmap::encodeChunkEnvelope(42, bytes));
        EXPECT_EQ(env.requestId, 42u); EXPECT_TRUE(std::holds_alternative<w::Frame>(env.frame));
    }
    heatmap::ChunkFrame legacy; legacy.kind = heatmap::ChunkKind::Error;
    auto envelope = w::decodeEnvelope(heatmap::encodeChunkEnvelope(7, heatmap::encodeChunk(legacy)));
    EXPECT_TRUE(std::holds_alternative<heatmap::ChunkFrame>(envelope.frame));
}
TEST(ChunkFrame, HashCoversOnlyFieldsFiveThroughElevenVersionAndRawPayload) {
    const auto f = example(); const auto hash = w::contentHash(f);
    auto changed = f; changed.kind = w::Kind::NotModified; changed.sealed = false;
    changed.revision = 90; changed.epoch++; changed.codec = 1;
    EXPECT_EQ(w::contentHash(changed), hash);
    for (int field = 5; field <= 15; ++field) {
        changed = f;
        switch (field) {
        case 5: changed.key.instrument = "coinbase:ETH-USD"; break;
        case 6: changed.key.channel = "flow.rows"; break;
        case 7: ++changed.key.levelMs; break;
        case 8: ++changed.key.priceStep; break;
        case 9: ++changed.key.startMs; break;
        case 10: ++changed.endMs; break;
        case 11: ++changed.committedThroughMs; break;
        case 15: ++changed.payloadVersion; break;
        default: continue;
        }
        EXPECT_NE(w::contentHash(changed), hash) << field;
    }
    changed = f; changed.payload.back() ^= 1; EXPECT_NE(w::contentHash(changed), hash);
}
TEST(ChunkFrame, CompressionRequiresTenPercentSavingsAndRoundTrips) {
    auto f = example(); f.payload.assign(4096, 17);
    auto decoded = w::decode(w::encode(f));
    EXPECT_EQ(decoded.codec, 1); EXPECT_EQ(decoded.payload, f.payload);
    std::mt19937 rng(42); for (auto& b : f.payload) b = uint8_t(rng());
    decoded = w::decode(w::encode(f)); EXPECT_EQ(decoded.codec, 0); EXPECT_EQ(decoded.payload, f.payload);
    // Exercise both sides of the exact threshold with partially random data.
    bool below = false, above = false;
    for (size_t zeros = 300; zeros < 600; ++zeros) {
        auto data = f.payload; std::fill_n(data.begin(), zeros, 0);
        std::vector<uint8_t> z(ZSTD_compressBound(data.size()));
        const auto n = ZSTD_compress(z.data(), z.size(), data.data(), data.size(), 1);
        f.payload = data;
        EXPECT_EQ(w::decode(w::encode(f)).codec, n * 10 <= data.size() * 9 ? 1 : 0);
        below |= n * 10 > data.size() * 9; above |= n * 10 <= data.size() * 9;
    }
    EXPECT_TRUE(below); EXPECT_TRUE(above);
}
TEST(ChunkFrame, DecoderRejectsEveryTruncationAndInvalidTransportField) {
    auto f = example(); const auto good = w::encode(f); ASSERT_EQ(good.size(), Body + f.payload.size());
    for (size_t n = 0; n < good.size(); ++n) EXPECT_THROW(w::decode(std::span(good).first(n)), std::invalid_argument) << n;
    for (auto [at, value, size] : std::vector<std::tuple<size_t,uint64_t,size_t>>{
        {0, 0, 1}, {4, 2, 2}, {6, 2, 1}, {7, 0, 1}, {8, 73, 1}, {25, 33, 1},
        {38, 123, 8}, {46, 1, 8}, {54, UINT64_MAX, 8}, {Extent, 0, 8}, {Extent+8, 0, 8},
        {Extent+16, 1, 8}, {Hash, 0, 8}, {Version, 2, 2}, {Codec, 2, 1},
        {Raw, w::MaxFrameBytes+1, 4}, {Stored, UINT32_MAX, 4}, {Raw, 7, 4}}) {
        auto bad = good; put(bad, at, value, size); EXPECT_THROW(w::decode(bad), std::invalid_argument) << at;
    }
    auto trailing = good; trailing.push_back(0); EXPECT_THROW(w::decode(trailing), std::invalid_argument);
    auto huge = good; huge.resize(w::MaxFrameBytes); EXPECT_THROW(w::decode(huge), std::invalid_argument);
    huge.resize(w::MaxFrameBytes+1); EXPECT_THROW(w::decodeEnvelope(huge), std::invalid_argument);
    // str8 counts are checked against remaining bytes before string allocation.
    for (size_t size : {size_t(9), size_t(25)}) EXPECT_THROW(w::decode(std::span(good).first(size)), std::invalid_argument);
    f.kind = w::Kind::Error;
    auto control = w::encode(f); control.push_back(0); EXPECT_THROW(w::decode(control), std::invalid_argument);
    f.kind = w::Kind::NotModified;
    control = w::encode(f); control.push_back(0); EXPECT_THROW(w::decode(control), std::invalid_argument);
}
TEST(ChunkFrame, ZstdDeclaredSizeAndStoredSizeAreCheckedBeforeAllocation) {
    auto f = example(); f.payload.assign(8192, 1); const auto good = w::encode(f); ASSERT_EQ(good[Codec], 1);
    auto bad = good; put(bad, Raw, 8000, 4); EXPECT_THROW(w::decode(bad), std::invalid_argument);
    bad = good; put(bad, Raw, w::MaxFrameBytes+1, 4); EXPECT_THROW(w::decode(bad), std::invalid_argument);
    bad = good; bad[Body] ^= 1; EXPECT_THROW(w::decode(bad), std::invalid_argument);
    bad = good; bad.push_back(0); put(bad, Stored, bad.size()-Body, 4); EXPECT_THROW(w::decode(bad), std::invalid_argument);
    // Unknown content size is not accepted as a license to allocate raw length.
    ZSTD_CCtx* ctx = ZSTD_createCCtx(); ASSERT_NE(ctx, nullptr);
    ASSERT_FALSE(ZSTD_isError(ZSTD_CCtx_setParameter(ctx, ZSTD_c_contentSizeFlag, 0)));
    std::vector<uint8_t> z(ZSTD_compressBound(f.payload.size()));
    const auto n = ZSTD_compress2(ctx, z.data(), z.size(), f.payload.data(), f.payload.size()); ZSTD_freeCCtx(ctx);
    ASSERT_FALSE(ZSTD_isError(n)); bad.assign(good.begin(), good.begin()+Body); bad.insert(bad.end(), z.begin(), z.begin()+n);
    put(bad, Stored, n, 4); EXPECT_THROW(w::decode(bad), std::invalid_argument);
}
TEST(FlowChunkJson, BoundedRequestsAndSubscriptions) {
    nlohmann::json good = {{"req", uint64_t(1)}, {"instrument", "coinbase:BTC-USD"}, {"channel", "flow.rows"},
        {"level_ms", 60000}, {"price_step", 1}, {"starts", {946684800000LL}}, {"have_hash", {"0000000000000000"}}};
    protocol::flowchunks::Request q; EXPECT_FALSE(protocol::flowchunks::parseRequest(good,q));
    ASSERT_TRUE(q.have[0]); EXPECT_EQ(*q.have[0], 0u);
    for (const auto& patch : std::vector<nlohmann::json>{
        {{"req", -1}}, {{"instrument", "coinbase:../BTC"}}, {{"instrument", "bogus:BTC"}},
        {{"channel", "trades.big"}}, {{"level_ms", UINT64_MAX}}, {{"level_ms", 1000}},
        {{"price_step", 2}}, {{"starts", {UINT64_MAX}}}, {{"starts", {INT64_MIN}}}, {{"starts", {946684800001LL}}},
        {{"starts", nlohmann::json::array()}}, {{"starts", std::vector<int64_t>(65,946684800000LL)}},
        {{"have_hash", {"123"}}}, {{"have_hash", {"zzzzzzzzzzzzzzzz"}}}, {{"have_hash", {"", ""}}}}) {
        auto j = good; j.update(patch); q = {}; EXPECT_TRUE(protocol::flowchunks::parseRequest(j,q)) << j;
    }
    good["instrument"] = "kalshi:KXBTC-26"; q = {}; EXPECT_FALSE(protocol::flowchunks::parseRequest(good,q));
    for (const auto& ids : std::vector<nlohmann::json>{nlohmann::json::array(), {"coinbase:BTC-USD"}}) {
        std::vector<std::string> parsed; EXPECT_FALSE(protocol::flowchunks::parseSubscribe({{"instruments",ids}}, parsed));
    }
    for (const auto& ids : std::vector<nlohmann::json>{{"coinbase:BTC-USD", "coinbase:BTC-USD"}, {"BTC-USD"}, std::vector<std::string>(9,"coinbase:BTC-USD")}) {
        std::vector<std::string> parsed; EXPECT_TRUE(protocol::flowchunks::parseSubscribe({{"instruments",ids}}, parsed));
    }
}
