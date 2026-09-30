#include "heatmap/BinCell.hpp"
#include "heatmap/ChunkCodec.hpp"
#include "heatmap/TimeComposer.hpp"
#include "servermodel/RecordingChunks.hpp"
#include "servermodel/RecordingLive.hpp"
#include "servermodel/RecordingPage.hpp"
#include <QTemporaryDir>
#include <gtest/gtest.h>
#include <zstd.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <limits>
#include <thread>

namespace {
using namespace recording;
using namespace heatmap;
constexpr int64_t epoch = kHmc2MinMs;
using Clock = std::chrono::steady_clock;
double ms(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}
uint32_t read32(const std::vector<uint8_t>& wire, size_t offset) {
    uint32_t n = 0;
    for (unsigned i = 0; i < 4; ++i) n |= uint32_t(wire.at(offset+i)) << (8*i);
    return n;
}
void write32(std::vector<uint8_t>& wire, size_t offset, uint32_t n) {
    for (unsigned i = 0; i < 4; ++i) wire.at(offset+i) = uint8_t(n >> (8*i));
}
void write64(std::vector<uint8_t>& wire, size_t offset, uint64_t n) {
    for (unsigned i = 0; i < 8; ++i) wire.at(offset+i) = uint8_t(n >> (8*i));
}
struct WireOffsets { size_t hash, columns, entries, rawLen, zLen, payload; };
WireOffsets offsets(const std::vector<uint8_t>& wire) {
    const size_t source = 9 + wire.at(8);
    const size_t hash = source + 1 + wire.at(source) + 24 + 40 + 8 + 8 +
        (wire.at(6) == uint8_t(ChunkKind::LiveColumn) ? 8 : 0);
    return {hash, hash+8, hash+12, hash+16, hash+20, hash+24};
}
uint64_t chunkHash(const std::vector<uint8_t>& wire, size_t prefixLen, const std::vector<uint8_t>& raw) {
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < prefixLen; ++i) { h ^= wire[i]; h *= 1099511628211ULL; }
    for (auto b : raw) { h ^= b; h *= 1099511628211ULL; }
    return h;
}
std::vector<uint8_t> rewritePayload(std::vector<uint8_t> wire,
                                    const std::function<void(std::vector<uint8_t>&)>& edit) {
    const auto o = offsets(wire);
    std::vector<uint8_t> raw(read32(wire, o.rawLen));
    EXPECT_EQ(ZSTD_decompress(raw.data(), raw.size(), wire.data()+o.payload, read32(wire, o.zLen)), raw.size());
    edit(raw);
    std::vector<uint8_t> zipped(ZSTD_compressBound(raw.size()));
    const auto len = ZSTD_compress(zipped.data(), zipped.size(), raw.data(), raw.size(), 3);
    EXPECT_FALSE(ZSTD_isError(len));
    zipped.resize(len);
    write64(wire, o.hash, chunkHash(wire, o.hash, raw));
    write32(wire, o.rawLen, uint32_t(raw.size()));
    write32(wire, o.zLen, uint32_t(zipped.size()));
    wire.resize(o.payload);
    wire.insert(wire.end(), zipped.begin(), zipped.end());
    return wire;
}
Hmc2Record minuteRecord(int i, const std::string& layer = "deep") {
    Hmc2Record r;
    const int tick = layer == "near" ? 1 : i < 30 ? 1 : 2;
    r.header = {"BTC-USD", layer, kMinuteMs, 100., 100 * tick, {}, uint64_t(tick)};
    r.bucketStartMs = epoch + int64_t(i) * kMinuteMs;
    r.observedMs = i % 7 ? 60000 : 30000;
    r.flags = (r.observedMs < 60000 ? kPartial : 0) | (i == 61 ? kResynced : 0);
    r.bidRowLo = r.askRowLo = 100 / tick;
    r.bidRowHi = r.askRowHi = 106 / tick - 1;
    r.entries = {{101 / tick, false, encodeSize(2. + i / 100.), 0, r.observedMs},
                 {104 / tick, true, encodeSize(3. + i / 100.), 0, r.observedMs}};
    std::sort(r.entries.begin(), r.entries.end(), [](const auto& a, const auto& b) {
        return std::pair(a.row, a.isAsk) < std::pair(b.row, b.isAsk);
    });
    return r;
}
Hmc2Record hourRecord() {
    auto r = minuteRecord(0);
    r.header.tfMs = kHourMs;
    r.bucketStartMs = epoch;
    r.observedMs = 3'000'000;
    r.flags = kPartial | kLateEvents;
    r.entries = {{101, false, encodeSize(2), 0, 2'000'000},
                 {104, true, encodeSize(3), 0, 3'000'000}};
    r.coverage = {{100, 102, false, 2'000'000}, {103, 105, false, 3'000'000},
                  {100, 105, true, 3'000'000}};
    return r;
}
void same(const SparseColumns& a, const SparseColumns& b) {
    ASSERT_EQ(a.symbol, b.symbol); ASSERT_EQ(a.layer, b.layer); ASSERT_EQ(a.tfMs, b.tfMs);
    ASSERT_EQ(a.startMs, b.startMs); ASSERT_EQ(a.endMs, b.endMs);
    ASSERT_EQ(a.scannedRanges, b.scannedRanges);
    ASSERT_EQ(a.columns.size(), b.columns.size());
    for (size_t i = 0; i < a.columns.size(); ++i) {
        const auto& x = a.columns[i]; const auto& y = b.columns[i];
        EXPECT_EQ(x.bucketStartMs, y.bucketStartMs); EXPECT_EQ(x.observedMs, y.observedMs);
        EXPECT_EQ(x.flags, y.flags); ASSERT_EQ(x.native.size(), y.native.size());
        for (size_t j = 0; j < x.native.size(); ++j) {
            const auto& n = x.native[j]; const auto& m = y.native[j];
            EXPECT_EQ(n.grid.configHash, m.grid.configHash);
            EXPECT_EQ(n.grid.rowTickUnits, m.grid.rowTickUnits);
            EXPECT_EQ(n.grid.priceScale, m.grid.priceScale);
            EXPECT_EQ(n.sizeScale.floor, m.sizeScale.floor);
            EXPECT_EQ(n.sizeScale.codesPerOctave, m.sizeScale.codesPerOctave);
            EXPECT_EQ(n.baseRow, m.baseRow); EXPECT_EQ(n.observedMs, m.observedMs);
            EXPECT_EQ(n.composed, m.composed);
            for (int side = 0; side < 2; ++side) {
                ASSERT_EQ(n.coverage[side].size(), m.coverage[side].size());
                for (size_t k = 0; k < n.coverage[side].size(); ++k) {
                    EXPECT_EQ(n.coverage[side][k].lo, m.coverage[side][k].lo);
                    EXPECT_EQ(n.coverage[side][k].hi, m.coverage[side][k].hi);
                    EXPECT_EQ(n.coverage[side][k].coveredMs, m.coverage[side][k].coveredMs);
                }
            }
            ASSERT_EQ(n.entries.size(), m.entries.size());
            for (size_t k = 0; k < n.entries.size(); ++k) {
                EXPECT_EQ(n.entries[k].rowSide, m.entries[k].rowSide);
                EXPECT_EQ(n.entries[k].code, m.entries[k].code);
            }
            EXPECT_EQ(n.entryCoveredMs, m.entryCoveredMs);
            EXPECT_EQ(n.numerators, m.numerators);
        }
    }
}
class ChunkTest : public testing::Test {
protected:
    QTemporaryDir temp;
    std::filesystem::path root() const { return temp.path().toStdString(); }
    void seed() {
        Hmc2Store store(root());
        for (int i = 0; i < 120; ++i) {
            if (i == 9 || (i >= 40 && i < 45) || i == 68) continue;
            store.append(minuteRecord(i)); store.append(minuteRecord(i, "near"));
        }
        store.append(hourRecord());
    }
};
TEST_F(ChunkTest, RoundTripGapsGridChangesCoverageAndOpenSealedState) {
    seed(); Hmc2Reader reader(root());
    ChunkKey first{"BTC-USD", "hmc2.deep", kMinuteMs, epoch};
    ChunkKey second{"BTC-USD", "hmc2.deep", kMinuteMs, epoch + kHourMs};
    const auto a = buildChunk(reader, first), b = buildChunk(reader, second);
    ASSERT_EQ(a.scannedRanges.size(), 1); EXPECT_EQ(a.scannedRanges[0].startMs, epoch);
    EXPECT_EQ(a.scannedRanges[0].endMs, epoch + kHourMs);
    EXPECT_EQ(bucketState(a, epoch + 9 * kMinuteMs), BucketState::Gap);
    EXPECT_EQ(bucketState(a, epoch + 10 * kMinuteMs), BucketState::Present);
    EXPECT_EQ(bucketState(a, epoch + kHourMs), BucketState::NotLoaded);
    const BookRecorder::Watermarks partialWatermarks{epoch + 30*kMinuteMs + 1, 0};
    const auto prefix = buildChunk(reader, first, partialWatermarks);
    ASSERT_EQ(prefix.scannedRanges.size(), 1);
    EXPECT_EQ(prefix.scannedRanges[0].endMs, epoch + 30*kMinuteMs);
    EXPECT_EQ(bucketState(prefix, epoch + 31*kMinuteMs), BucketState::NotLoaded);
    EXPECT_NE(a.columns.front().native.front().grid.configHash,
              b.columns.front().native.front().grid.configHash);
    auto open = chunkState(first, partialWatermarks, 17);
    EXPECT_FALSE(open.sealed); EXPECT_EQ(open.revision, 17);
    ChunkFrame f{ChunkKind::Chunk, first, open, prefix};
    auto encoded = encodeChunk(f); auto decoded = decodeChunk(encoded);
    same(prefix, decoded.columns);
    auto envelope = decodeChunkEnvelope(encodeChunkEnvelope(77, encoded));
    EXPECT_EQ(envelope.requestId, 77);
    same(prefix, envelope.chunk.columns);
    EXPECT_EQ(decoded.state.committedThroughMs, open.committedThroughMs);
    EXPECT_EQ(decoded.state.revision, 17); EXPECT_FALSE(decoded.state.sealed);
    EXPECT_NE(decoded.contentHash, 0);
    auto overScannedWire = encoded;
    write64(overScannedWire, offsets(encoded).hash-16, uint64_t(epoch+29*kMinuteMs));
    overScannedWire = rewritePayload(std::move(overScannedWire), [](auto&) {});
    EXPECT_THROW(decodeChunk(overScannedWire), std::invalid_argument);
    auto sealed = chunkState(first, {epoch + kHourMs, 0}, 19);
    EXPECT_TRUE(sealed.sealed); EXPECT_EQ(sealed.revision, 0);
    f.state = sealed; f.columns = a;
    auto sealedWire = encodeChunk(f);
    same(a, decodeChunk(sealedWire).columns);
    auto incomplete = f;
    incomplete.columns.scannedRanges = {{epoch, epoch + 30*kMinuteMs}};
    incomplete.columns.columns.erase(std::remove_if(incomplete.columns.columns.begin(),
        incomplete.columns.columns.end(), [&](const auto& c) { return c.bucketStartMs >= epoch + 30*kMinuteMs; }),
        incomplete.columns.columns.end());
    EXPECT_THROW(encodeChunk(incomplete), std::invalid_argument);
    auto overClaim = f;
    overClaim.state = {false, epoch + 30*kMinuteMs, 1};
    EXPECT_THROW(encodeChunk(overClaim), std::invalid_argument);
    ChunkFrame other{ChunkKind::Chunk, second, chunkState(second, {epoch + 2*kHourMs, 0}, 1), b};
    auto otherWire = encodeChunk(other);
    EncodedChunkLru cache(std::max(sealedWire.size(), otherWire.size()) + 10);
    EXPECT_EQ(cache.bytes(), 0);
    EXPECT_THROW(cache.put(encoded), std::invalid_argument);
    cache.put(std::move(sealedWire)); EXPECT_TRUE(cache.get(first));
    EXPECT_GT(cache.bytes(), 0);
    EXPECT_EQ(cache.size(), 1);
    auto cached = cache.get(first);
    ASSERT_TRUE(cached);
    const auto response1 = encodeChunkEnvelope(1, *cached);
    const auto response2 = encodeChunkEnvelope(2, *cached);
    EXPECT_NE(response1, response2);
    EXPECT_EQ(decodeChunkEnvelope(response1).requestId, 1);
    EXPECT_EQ(decodeChunkEnvelope(response2).requestId, 2);
    EXPECT_TRUE(std::equal(response1.begin()+14, response1.end(), response2.begin()+14));
    cache.put(std::move(otherWire)); EXPECT_FALSE(cache.get(first));
    EXPECT_EQ(cache.size(), 1);
    auto version = encoded; version[4] = 1; // v1 (layer codes) is refused, no shim
    EXPECT_THROW(decodeChunk(version), std::invalid_argument);
    encoded.back() ^= 1;
    EXPECT_THROW(decodeChunk(encoded), std::invalid_argument);
}
TEST_F(ChunkTest, HourCoverageSidecarsRoundTripAndComposedV1IsRejected) {
    seed(); Hmc2Reader reader(root());
    ChunkKey hourKey{"BTC-USD", "hmc2.deep", kHourMs, epoch};
    auto hours = buildChunk(reader, hourKey);
    const BookRecorder::Watermarks minuteOnly{epoch+kDayMs, epoch};
    EXPECT_FALSE(chunkState(hourKey, minuteOnly, 5).sealed);
    EXPECT_TRUE(buildChunk(reader, hourKey, minuteOnly).scannedRanges.empty());
    ASSERT_EQ(hours.columns.size(), 1);
    EXPECT_EQ(hours.columns[0].native[0].entryCoveredMs.size(), 2);
    EXPECT_EQ(hours.columns[0].native[0].coverage[0].size(), 2);
    ChunkFrame f{ChunkKind::Chunk, hourKey, chunkState(hourKey, {epoch+kDayMs, epoch+kDayMs}, 0), hours};
    std::vector<uint8_t> hourWire;
    ASSERT_NO_THROW(hourWire = encodeChunk(f));
    ASSERT_NO_THROW(same(hours, decodeChunk(hourWire).columns));
    auto minutes = buildChunk(reader, {"BTC-USD", "hmc2.deep", kMinuteMs, epoch});
    auto composed = compose(minutes, 5 * kMinuteMs);
    ASSERT_FALSE(composed.columns.empty());
    EXPECT_TRUE(composed.columns.front().native.front().composed);
    // v1 has no portable encoding for in-memory long-double numerators.
    auto composedHour = compose(minutes, kHourMs);
    ASSERT_EQ(composedHour.columns.size(), 1);
    EXPECT_EQ(composedHour.startMs, epoch);
    EXPECT_EQ(composedHour.endMs, epoch + kHourMs);
    EXPECT_NO_THROW(validate(composedHour));
    composedHour.endMs = epoch + kDayMs; // partial scan inside a day-sized hour chunk
    ChunkFrame cf{ChunkKind::Chunk, {"BTC-USD", "hmc2.deep", kHourMs, epoch},
                  {false, epoch+kHourMs, 0}, composedHour};
    EXPECT_THROW(encodeChunk(cf), std::invalid_argument);
}
TEST_F(ChunkTest, MalformedHeadersAndPayloadsAreBounded) {
    seed(); Hmc2Reader reader(root());
    const ChunkKey key{"BTC-USD", "hmc2.deep", kMinuteMs, epoch};
    auto wire = encodeChunk({ChunkKind::Chunk, key, {true, epoch+kHourMs, 0}, buildChunk(reader, key)});
    const auto o = offsets(wire);
    for (size_t len = 0; len < wire.size(); ++len)
        EXPECT_THROW(decodeChunk(std::span(wire.data(), len)), std::invalid_argument) << "prefix=" << len;
    auto enveloped = encodeChunkEnvelope(9, wire);
    for (size_t len = 0; len < enveloped.size(); ++len)
        EXPECT_THROW(decodeChunkEnvelope(std::span(enveloped.data(), len)), std::invalid_argument)
            << "envelope_prefix=" << len;
    for (size_t offset = 0; offset < o.payload; ++offset) {
        auto flipped = wire; flipped[offset] ^= 0x80;
        EXPECT_THROW(decodeChunk(flipped), std::invalid_argument) << "header_offset=" << offset;
    }
    auto hugeRaw = wire; write32(hugeRaw, o.rawLen, 16u*1024u*1024u + 1);
    EXPECT_THROW(decodeChunk(hugeRaw), std::invalid_argument);
    auto wrongFrameSize = wire; write32(wrongFrameSize, o.rawLen, read32(wire, o.rawLen)+1);
    EXPECT_THROW(decodeChunk(wrongFrameSize), std::invalid_argument);
    auto hugeEntries = wire; write32(hugeEntries, o.entries, 0xffffffffu);
    EXPECT_THROW(decodeChunk(hugeEntries), std::invalid_argument);
    auto hugeColumns = wire; write32(hugeColumns, o.columns, 0xffffffffu);
    EXPECT_THROW(decodeChunk(hugeColumns), std::invalid_argument);
    auto badCoverage = rewritePayload(wire, [](auto& raw) { raw.at(98) = raw.at(99) = 0xff; });
    EXPECT_THROW(decodeChunk(badCoverage), std::invalid_argument);
    auto composedV1 = rewritePayload(wire, [](auto& raw) { raw.at(96) = 1; });
    EXPECT_THROW(decodeChunk(composedV1), std::invalid_argument);
    auto badRanges = rewritePayload(wire, [](auto& raw) {
        for (size_t i = 10; i < 18; ++i) raw.at(i) = 0;
    });
    EXPECT_THROW(decodeChunk(badRanges), std::invalid_argument); // hash is correct
    auto tooManyRanges = rewritePayload(wire, [](auto& raw) { raw.at(0) = raw.at(1) = 0xff; });
    EXPECT_THROW(decodeChunk(tooManyRanges), std::invalid_argument);
    auto prematureSeal = wire;
    write64(prematureSeal, o.hash-16, uint64_t(epoch+kHourMs-1));
    prematureSeal = rewritePayload(std::move(prematureSeal), [](auto&) {});
    EXPECT_THROW(decodeChunk(prematureSeal), std::invalid_argument);
}
TEST(ChunkCodecControl, SourceIdsNotModifiedAndErrorFramesAreExactAndBounded) {
    // Only neutral source ids travel; HMC2 layer names are not valid ids.
    EXPECT_EQ(chunkSpanMs("hmc2.near", kMinuteMs), kHourMs);
    EXPECT_EQ(chunkSpanMs("hmc2.near", kHourMs), 0);
    EXPECT_EQ(chunkSpanMs("hmc2.deep", kHourMs), kDayMs);
    EXPECT_EQ(chunkSpanMs("deep", kMinuteMs), 0);
    EXPECT_THROW(chunkEndMs({"BTC-USD", "deep", kMinuteMs, epoch}), std::invalid_argument);
    ChunkFrame nm;
    nm.kind = ChunkKind::NotModified;
    nm.key = {"BTC-USD", "hmc2.near", kMinuteMs, epoch};
    nm.state = {false, epoch + 30 * kMinuteMs, 31};
    nm.contentHash = 0x0123456789abcdefULL;
    const auto nmWire = encodeChunk(nm);
    const auto nmBack = decodeChunkEnvelope(encodeChunkEnvelope(5, nmWire));
    EXPECT_EQ(nmBack.requestId, 5);
    EXPECT_EQ(nmBack.chunk.kind, ChunkKind::NotModified);
    EXPECT_EQ(nmBack.chunk.key, nm.key);
    EXPECT_EQ(nmBack.chunk.state.committedThroughMs, nm.state.committedThroughMs);
    EXPECT_EQ(nmBack.chunk.state.revision, 31);
    EXPECT_EQ(nmBack.chunk.contentHash, nm.contentHash);
    for (size_t len = 0; len < nmWire.size(); ++len)
        EXPECT_THROW(decodeChunk(std::span(nmWire.data(), len)), std::invalid_argument) << len;
    auto trailing = nmWire; trailing.push_back(0);
    EXPECT_THROW(decodeChunk(trailing), std::invalid_argument);
    auto badSeal = nm; badSeal.state = {true, epoch + kHourMs, 3};
    EXPECT_THROW(encodeChunk(badSeal), std::invalid_argument);
    auto badKey = nm; badKey.key.startMs += kMinuteMs;
    EXPECT_THROW(encodeChunk(badKey), std::invalid_argument);

    ChunkFrame err;
    err.kind = ChunkKind::Error;
    err.key = {"NOT A SYMBOL", "whatever", 7, -1}; // echoed as sent
    err.error = ChunkError::Busy;
    err.message = "session chunk budget exhausted";
    const auto errWire = encodeChunk(err);
    const auto errBack = decodeChunk(errWire);
    EXPECT_EQ(errBack.kind, ChunkKind::Error);
    EXPECT_EQ(errBack.key, err.key);
    EXPECT_EQ(errBack.error, ChunkError::Busy);
    EXPECT_EQ(errBack.message, err.message);
    EXPECT_STREQ(chunkErrorName(errBack.error), "busy");
    for (size_t len = 0; len < errWire.size(); ++len)
        EXPECT_THROW(decodeChunk(std::span(errWire.data(), len)), std::invalid_argument) << len;
    auto unknownCode = err; unknownCode.error = ChunkError(99);
    EXPECT_THROW(encodeChunk(unknownCode), std::invalid_argument);
    auto longMessage = err; longMessage.message.assign(256, 'x');
    EXPECT_THROW(encodeChunk(longMessage), std::invalid_argument);
    auto reserved = errWire; reserved[6] = uint8_t(ChunkKind::LiveColumn);
    EXPECT_THROW(decodeChunk(reserved), std::invalid_argument);
}
TEST(ChunkCodecControl, NotModifiedTimestampBoundsBeforeArithmetic) {
    // Exercise both native levels and both edges of the supported timestamp range.
    for (const int64_t level : {kMinuteMs, kHourMs}) {
        ChunkFrame frame;
        frame.kind = ChunkKind::NotModified;
        frame.key = {"BTC-USD", "hmc2.deep", level, epoch};
        const int64_t span = chunkSpanMs(frame.key.source, level);
        const auto validWire = encodeChunk(frame);
        const size_t startAt = 8 + 1 + frame.key.symbol.size() + 1 + frame.key.source.size() + 8;
        const int64_t last = kHmc2EndMs - span;
        const int64_t max = std::numeric_limits<int64_t>::max();
        const int64_t min = std::numeric_limits<int64_t>::min();
        for (const int64_t start : {max, min, max - span + 1, max - span, min + span,
                                   epoch - span, epoch - 1, epoch, epoch + 1,
                                   last - 1, last, last + 1, kHmc2EndMs}) {
            SCOPED_TRACE(::testing::Message() << "level=" << level << " start=" << start);
            frame.key.startMs = start;
            // Handcraft bytes from a valid frame; do not rely on the encoder to reject them.
            auto wire = validWire;
            write64(wire, startAt, uint64_t(start));
            if (start == epoch || start == last) {
                EXPECT_EQ(decodeChunk(wire).key.startMs, start);
                EXPECT_EQ(decodeChunk(encodeChunk(frame)).key.startMs, start);
            } else {
                EXPECT_THROW(decodeChunk(wire), std::invalid_argument);
                EXPECT_THROW(encodeChunk(frame), std::invalid_argument);
            }
        }
        frame.key.startMs = epoch;
        auto unknownSource = validWire;
        unknownSource.at(8 + 1 + frame.key.symbol.size() + 1) = 'x';
        EXPECT_THROW(decodeChunk(unknownSource), std::invalid_argument);
        auto badLevel = validWire;
        write64(badLevel, startAt - 8, uint64_t(max));
        EXPECT_THROW(decodeChunk(badLevel), std::invalid_argument);
        frame.key.levelMs = max;
        EXPECT_THROW(encodeChunk(frame), std::invalid_argument);
        frame.key.levelMs = level;
        frame.key.source = "unknown";
        EXPECT_THROW(encodeChunk(frame), std::invalid_argument);
    }
}

TEST_F(ChunkTest, SharedCacheReplacesByBytesAndRetainsBorrowedBuffers) {
    seed(); Hmc2Reader reader(root());
    const ChunkKey key{"BTC-USD", "hmc2.deep", kMinuteMs, epoch};
    ChunkFrame a{ChunkKind::Chunk, key, {true, epoch+kHourMs, 0}, buildChunk(reader, key)};
    auto oldWire = encodeChunk(a);
    auto b = a; b.columns.columns.front().flags |= kLateEvents;
    auto newWire = encodeChunk(b);
    EncodedChunkLru tooSmall(oldWire.size()-1);
    tooSmall.put(oldWire);
    EXPECT_EQ(tooSmall.bytes(), 0);
    EXPECT_EQ(tooSmall.size(), 0);
    EncodedChunkLru cache(std::max(oldWire.size(), newWire.size()));
    cache.put(oldWire);
    auto borrowed = cache.get(key);
    ASSERT_TRUE(borrowed);
    EXPECT_EQ(cache.bytes(), oldWire.size());
    cache.put(newWire);
    ASSERT_TRUE(cache.get(key));
    EXPECT_NE(borrowed, cache.get(key));
    EXPECT_EQ(*borrowed, oldWire);
    EXPECT_EQ(cache.bytes(), newWire.size());
    cache.put(oldWire);
    std::atomic_bool bad = false;
    std::thread readerThread([&] {
        for (int i = 0; i < 40; ++i) {
            auto p = cache.get(key);
            if (!p || (p != borrowed && *p != oldWire && *p != newWire)) bad = true;
        }
    });
    for (int i = 0; i < 40; ++i) cache.put(i % 2 ? oldWire : newWire);
    readerThread.join();
    EXPECT_FALSE(bad.load());
    EXPECT_EQ(cache.size(), 1);
}
TEST_F(ChunkTest, DecodeEncodeBuildChunkMatchesBuildPage) {
    seed(); Hmc2Reader reader(root());
    std::array<SparseColumns, 2> chunks{
        buildChunk(reader, {"BTC-USD", "hmc2.deep", kMinuteMs, epoch}),
        buildChunk(reader, {"BTC-USD", "hmc2.deep", kMinuteMs, epoch+kHourMs})};
    for (auto& c : chunks) {
        ChunkKey key{c.symbol, "hmc2." + c.layer, c.tfMs, c.startMs};
        c = decodeChunk(encodeChunk({ChunkKind::Chunk, key, {true, c.endMs, 0}, c})).columns;
    }
    for (int64_t tf : {kMinuteMs, 5*kMinuteMs}) {
        BuildRequest q;
        q.symbol = "BTC-USD"; q.tfMs = tf; q.endMs = epoch + 2*kHourMs - tf;
        q.count = uint32_t(2*kHourMs/tf); q.priceLo = 100; q.priceHi = 106;
        q.rows = 3; q.displayTick = 2.; q.budgets = {};
        const auto page = buildPage(reader, q);
        ASSERT_EQ(page.status, BuildStatus::Complete) << page.message;
        ASSERT_EQ(page.layer, "deep");
        const auto composed = compose(chunks, tf);
        std::map<int64_t, const SparseColumn*> byTime;
        for (const auto& c : composed.columns) byTime[c.bucketStartMs] = &c;
        for (const auto& expected : page.columns) {
            auto it = byTime.find(expected.bucketStartMs);
            if (it == byTime.end()) {
                EXPECT_EQ(expected.observedMs, 0);
                EXPECT_EQ(bucketState(composed, expected.bucketStartMs), BucketState::Gap);
                continue;
            }
            const auto& actual = *it->second;
            EXPECT_EQ(actual.observedMs, expected.observedMs);
            EXPECT_EQ(actual.flags, expected.flags);
            const auto cells = binColumn(actual, page.band.lo,
                page.band.lo + page.band.tick * page.band.rows, page.band.tick, page.sizeScale);
            ASSERT_EQ(cells.size(), expected.cells.size());
            for (size_t row = 0; row < cells.size(); ++row) {
                EXPECT_EQ(cells[row].code, expected.cells[row]);
                EXPECT_EQ(cells[row].valid, bool((expected.validity[row/8] >> (row%8)) & 1));
            }
        }
    }
}
TEST_F(ChunkTest, RecorderLevelWatermarksKeepUnwrittenHourNotLoaded) {
    LiveCache live;
    int64_t local = epoch;
    RecorderConfig cfg{root(), 100., {}, {{"deep", 500, .5, 2., true}}, 2000, 2'000'000};
    cfg.publisher = [&](RecordPtr r) { live.publish(std::move(r)); };
    BookRecorder recorder(std::move(cfg), [&] { return local; });
    recorder.onSnapshot("BTC-USD", local, {{true, 99., 2.}, {false, 101., 4.}});
    recorder.drainForTest();
    const ChunkKey hourKey{"BTC-USD", "hmc2.deep", kHourMs, epoch};
    local = epoch + kHourMs + 1999;
    recorder.onTick(local);
    recorder.drainForTest();
    const auto before = recorder.watermarks("BTC-USD", "deep");
    EXPECT_EQ(before.minuteThroughMs, epoch + 59*kMinuteMs);
    EXPECT_EQ(before.hourThroughMs, epoch);
    Hmc2Reader reader(root());
    const auto open = buildChunk(reader, hourKey, before);
    EXPECT_EQ(bucketState(open, epoch), BucketState::NotLoaded);
    EXPECT_EQ(live.snapshot("BTC-USD", "deep").committedThroughMs,
              epoch + 59*kMinuteMs);
    local = epoch + kHourMs + 2000;
    recorder.onTick(local);
    recorder.drainForTest();
    const auto after = recorder.watermarks("BTC-USD", "deep");
    EXPECT_EQ(after.minuteThroughMs, epoch + kHourMs);
    EXPECT_EQ(after.hourThroughMs, epoch + kHourMs);
    EXPECT_EQ(live.snapshot("BTC-USD", "deep").committedThroughMs, epoch + kHourMs);
    const auto persisted = buildChunk(reader, hourKey, after);
    EXPECT_EQ(bucketState(persisted, epoch), BucketState::Present);
    ASSERT_EQ(persisted.scannedRanges.size(), 1);
    EXPECT_EQ(persisted.scannedRanges[0].endMs, epoch + kHourMs);
}
TEST(ChunkBench, LastComplete24Hours) {
    if (!std::getenv("SENTINEL_CHUNK_BENCH") ||
        std::string(std::getenv("SENTINEL_CHUNK_BENCH")) != "1") GTEST_SKIP();
    const auto root = std::filesystem::path("/Volumes/T7/sentinel-data/recording");
    ASSERT_TRUE(std::filesystem::exists(root));
    for (const std::string layer : {"deep", "near"}) {
        Hmc2Reader availabilityReader(root); ReadControl control;
        const auto avail = availabilityReader.availability("BTC-USD", layer, kMinuteMs, control);
        ASSERT_EQ(control.status, ReadStatus::Complete);
        ASSERT_TRUE(avail.latestMs);
        const auto end = floorDiv(*avail.latestMs, kHourMs) * kHourMs;
        ASSERT_GE(end - 24*kHourMs, kHmc2MinMs);
        size_t totalBytes = 0; double coldMs = 0, encMs = 0, decMs = 0;
        for (int i = 0; i < 24; ++i) {
            ChunkKey key{"BTC-USD", std::string("hmc2.") + layer, kMinuteMs, end - (24-i)*kHourMs};
            const auto start = Clock::now();
            Hmc2Reader cold(root);
            const auto chunk = buildChunk(cold, key);
            const auto readEnd = Clock::now();
            auto wire = encodeChunk({ChunkKind::Chunk, key, {true, end, 0}, chunk});
            const auto encoded = Clock::now();
            auto decoded = decodeChunk(wire);
            const auto decodedAt = Clock::now();
            ASSERT_EQ(decoded.columns.columns.size(), chunk.columns.size());
            totalBytes += wire.size(); coldMs += ms(start, readEnd);
            encMs += ms(readEnd, encoded); decMs += ms(encoded, decodedAt);
        }
        std::cout << "CHUNK_BENCH layer=" << layer << " hours=24 bytes_per_hour=" << totalBytes/24
                  << " encode_ms_per_hour=" << encMs/24 << " decode_ms_per_hour=" << decMs/24
                  << " cold_reader_ms_per_hour=" << coldMs/24 << '\n';
    }
}
} // namespace

TEST(ChunkCodecLive, RoundTripFinalPendingOpenAcrossHourBoundary) {
    LiveCache cache;
    cache.publish(std::make_shared<Hmc2Record>(minuteRecord(58)));
    for (int i : {59, 60}) {
        auto r = minuteRecord(i);
        r.flags |= kProvisional;
        r.committedThroughMs = epoch + 59 * kMinuteMs;
        r.observedMs = i == 60 ? 12345 : 60000;
        if (r.observedMs < 60000) r.flags |= kPartial;
        std::reverse(r.entries.begin(), r.entries.end());
        cache.publish(std::make_shared<Hmc2Record>(r));
    }
    RawTailBuilder builder;
    const auto result = builder.build("BTC-USD", "hmc2.deep", cache.snapshot("BTC-USD", "deep"), epoch);
    ASSERT_TRUE(result.bytes);
    const auto decoded = decodeChunkEnvelope(encodeChunkEnvelope(123, *result.bytes));
    EXPECT_EQ(decoded.requestId, 123);
    const auto& f = decoded.chunk;
    EXPECT_EQ(f.kind, ChunkKind::LiveColumn);
    EXPECT_EQ(f.key.startMs, epoch + kHourMs);
    EXPECT_EQ(f.columns.startMs, epoch + 58 * kMinuteMs);
    EXPECT_EQ(f.columns.endMs, epoch + 61 * kMinuteMs);
    EXPECT_EQ(f.state.committedThroughMs, epoch + 59 * kMinuteMs);
    EXPECT_EQ(f.state.revision, 3);
    EXPECT_FALSE(f.state.sealed);
    ASSERT_EQ(f.columns.columns.size(), 3);
    EXPECT_FALSE(f.columns.columns.front().flags & kProvisional);
    EXPECT_TRUE(f.columns.columns[1].flags & kProvisional);
    EXPECT_EQ(f.columns.columns.back().observedMs, 12345);
    EXPECT_TRUE(f.columns.columns.back().flags & kPartial);
    EXPECT_TRUE(f.columns.columns.back().flags & kProvisional);
    EXPECT_EQ(f.columns.scannedRanges, (std::vector<SparseColumns::TimeRange>{{epoch + 58*kMinuteMs, epoch + 61*kMinuteMs}}));
    EXPECT_EQ(encodeChunk(f), *result.bytes);
    same(decodeChunk(*result.bytes).columns, f.columns);
}

TEST(ChunkCodecLive, MalformedAndOverflowingFramesRejectedWithValidHashes) {
    LiveCache cache;
    auto record = minuteRecord(60);
    record.flags |= kProvisional | kPartial;
    record.observedMs = 12345;
    record.committedThroughMs = record.bucketStartMs;
    cache.publish(std::make_shared<Hmc2Record>(record));
    RawTailBuilder builder;
    const auto frame = builder.build("BTC-USD", "hmc2.deep", cache.snapshot("BTC-USD", "deep"), epoch);
    ASSERT_TRUE(frame.bytes);
    const auto wire = *frame.bytes;
    const auto o = offsets(wire);
    const size_t levelAt = 10 + record.header.symbol.size() + std::string("hmc2.deep").size();
    const auto max = std::numeric_limits<int64_t>::max(), min = std::numeric_limits<int64_t>::min();
    for (size_t length = 0; length < wire.size(); ++length)
        EXPECT_THROW(decodeChunk(std::span(wire.data(), length)), std::invalid_argument) << length;
    auto rejectHeader = [&](size_t at, int64_t value) {
        auto broken = wire;
        write64(broken, at, uint64_t(value));
        broken = rewritePayload(std::move(broken), [](auto&) {}); // authentic hash; semantic check must fail
        EXPECT_THROW(decodeChunk(broken), std::invalid_argument) << "offset=" << at << " value=" << value;
    };
    for (int64_t value : {min, max, kHourMs, int64_t{0}}) rejectHeader(levelAt, value);
    for (int64_t value : {min, max, epoch-1, epoch+1, kHmc2EndMs}) rejectHeader(levelAt+8, value);
    for (int64_t value : {min, max, epoch+kHourMs, epoch+2*kHourMs+1, epoch+2*kHourMs+kMinuteMs})
        rejectHeader(levelAt+16, value);
    for (int64_t value : {min, max, epoch-kMinuteMs, epoch+1, epoch+61*kMinuteMs})
        rejectHeader(levelAt+24, value);
    for (int64_t value : {min, max, epoch-1, epoch+1, epoch+61*kMinuteMs}) rejectHeader(o.hash-16, value);
    rejectHeader(o.hash-8, 0); // series revision is never zero
    auto sealed = wire; sealed[7] = 1;
    sealed = rewritePayload(std::move(sealed), [](auto&) {});
    EXPECT_THROW(decodeChunk(sealed), std::invalid_argument);
    // One range (18 bytes), then bucket(8), observed(8), flags(4).
    for (uint32_t flags : {uint32_t(kPartial), uint32_t(kPartial|kProvisional|0x80000000u)}) {
        const auto broken = rewritePayload(wire, [flags](auto& raw) { write32(raw, 34, flags); });
        EXPECT_THROW(decodeChunk(broken), std::invalid_argument) << flags;
    }
    const auto duration = rewritePayload(wire, [](auto& raw) { write64(raw, 26, UINT64_MAX); });
    EXPECT_THROW(decodeChunk(duration), std::invalid_argument);
    const auto overflow = rewritePayload(wire, [](auto& raw) { write64(raw, 18, uint64_t(INT64_MAX)); });
    EXPECT_THROW(decodeChunk(overflow), std::invalid_argument);
    // A present column must be scanned; cache holes cannot be asserted as empty minutes.
    auto holes = wire;
    write64(holes, levelAt+24, uint64_t(epoch+59*kMinuteMs));
    holes = rewritePayload(std::move(holes), [](auto& raw) { write64(raw, 2, uint64_t(epoch+59*kMinuteMs)); });
    EXPECT_THROW(decodeChunk(holes), std::invalid_argument);
    auto enormous = wire; write32(enormous, o.rawLen, 16u*1024u*1024u+1);
    EXPECT_THROW(decodeChunk(enormous), std::invalid_argument);
    enormous = wire; write32(enormous, o.columns, UINT32_MAX);
    EXPECT_THROW(decodeChunk(enormous), std::invalid_argument);
    auto badEntries = rewritePayload(wire, [](auto& raw) { raw.back() = 0xff; });
    EXPECT_THROW(decodeChunk(badEntries), std::invalid_argument);
}

TEST(ChunkCodecLive, FullLatenessWindowAndFinalOnlyRolloverStayBounded) {
    LiveCache cache;
    for (int i = 60; i <= 120; ++i) {
        auto r = minuteRecord(i);
        r.flags |= kProvisional;
        r.committedThroughMs = epoch+60*kMinuteMs;
        cache.publish(std::make_shared<Hmc2Record>(r));
    }
    RawTailBuilder builder;
    const auto result = builder.build("BTC-USD", "hmc2.deep", cache.snapshot("BTC-USD", "deep"), epoch);
    ASSERT_TRUE(result.bytes);
    const auto frame = decodeChunk(*result.bytes);
    EXPECT_EQ(frame.columns.columns.size(), 61);
    EXPECT_EQ(frame.key.startMs, epoch+2*kHourMs);
    EXPECT_EQ(frame.columns.startMs, epoch+kHourMs);
    EXPECT_EQ(frame.columns.endMs, epoch+2*kHourMs+kMinuteMs);
    auto tooFar = *result.bytes;
    const size_t tailAt = 10 + frame.key.symbol.size() + frame.key.source.size() + 24;
    write64(tooFar, tailAt, uint64_t(epoch)); // aligned and supported, but two chunks back
    tooFar = rewritePayload(std::move(tooFar), [](auto&) {});
    EXPECT_THROW(decodeChunk(tooFar), std::invalid_argument);
    LiveCache finalOnly;
    finalOnly.publish(std::make_shared<Hmc2Record>(minuteRecord(59)));
    const auto final = decodeChunk(*builder.build("BTC-USD", "hmc2.deep", finalOnly.snapshot("BTC-USD", "deep"), epoch).bytes);
    EXPECT_EQ(final.key.startMs, epoch+kHourMs);
    EXPECT_EQ(final.columns.endMs, epoch+61*kMinuteMs);
    ASSERT_EQ(final.columns.columns.size(), 1);
    EXPECT_EQ(final.columns.columns.front().bucketStartMs, epoch+59*kMinuteMs);
    EXPECT_FALSE(final.columns.columns.front().flags & kProvisional);
    EXPECT_EQ(bucketState(final.columns, epoch+60*kMinuteMs), BucketState::NotLoaded);
}
