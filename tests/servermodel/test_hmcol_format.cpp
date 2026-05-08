#include <gtest/gtest.h>

#include "servermodel/HmcolFormat.hpp"

#include <array>
#include <cstring>
#include <string>
#include <vector>

using namespace hmcol;

// ---------- CRC32 ----------

TEST(HmcolCrc32, EmptyInputIsZero) {
    EXPECT_EQ(crc32(nullptr, 0), 0u);
}

TEST(HmcolCrc32, KnownVectors) {
    // Standard CRC-32 (zlib/PNG/IEEE) check values.
    const char* a = "a";
    EXPECT_EQ(crc32(a, 1), 0xE8B7BE43u);

    const char* abc = "abc";
    EXPECT_EQ(crc32(abc, 3), 0x352441C2u);

    const char* check = "123456789";
    EXPECT_EQ(crc32(check, 9), 0xCBF43926u);
}

TEST(HmcolCrc32, IncrementalMatchesOneShot) {
    const std::string s = "the quick brown fox jumps over the lazy dog";
    const uint32_t whole = crc32(s.data(), s.size());

    uint32_t streamed = 0;
    streamed = crc32Update(streamed, s.data(), 10);
    streamed = crc32Update(streamed, s.data() + 10, 15);
    streamed = crc32Update(streamed, s.data() + 25, s.size() - 25);
    EXPECT_EQ(whole, streamed);
}

// ---------- FileHeader ----------

TEST(HmcolFileHeader, FixedSize) {
    EXPECT_EQ(sizeof(FileHeader), 64u);
}

TEST(HmcolFileHeader, RoundTripCrc) {
    FileHeader h{};
    h.magic = kMagic;
    h.version = kVersion;
    h.timeframeMs = 60000;
    h.gridHeight = 2048;
    h.intensityFormat = kIntensityFormatU16;
    h.liquidityFormat = kLiquidityFormatU16;
    h.dayStartMs = 1'714'176'000'000; // some UTC midnight
    const std::string sym = "BTC-USD";
    std::memcpy(h.symbol, sym.data(), sym.size());

    finalizeFileHeaderCrc(h);
    EXPECT_TRUE(verifyFileHeader(h));
}

TEST(HmcolFileHeader, RejectsTamperedField) {
    FileHeader h{};
    h.magic = kMagic;
    h.version = kVersion;
    h.timeframeMs = 60000;
    h.gridHeight = 2048;
    h.intensityFormat = kIntensityFormatU16;
    h.liquidityFormat = kLiquidityFormatNone;
    h.dayStartMs = 0;
    finalizeFileHeaderCrc(h);
    ASSERT_TRUE(verifyFileHeader(h));

    h.gridHeight = 4096; // change after CRC was computed
    EXPECT_FALSE(verifyFileHeader(h));
}

TEST(HmcolFileHeader, RejectsBadMagic) {
    FileHeader h{};
    h.magic = 0xDEADBEEFu;
    h.version = kVersion;
    finalizeFileHeaderCrc(h);
    EXPECT_FALSE(verifyFileHeader(h));
}

TEST(HmcolFileHeader, RejectsWrongVersion) {
    FileHeader h{};
    h.magic = kMagic;
    h.version = 999;
    finalizeFileHeaderCrc(h);
    EXPECT_FALSE(verifyFileHeader(h));
}

// ---------- RecordHeader ----------

TEST(HmcolRecordHeader, FixedSize) {
    EXPECT_EQ(sizeof(RecordHeader), 56u);
}

TEST(HmcolRecordHeader, EmptySlotIsRejected) {
    RecordHeader r{}; // zeroed -> bucketStartMs == kEmptySlotSentinel
    EXPECT_EQ(r.bucketStartMs, kEmptySlotSentinel);

    std::array<uint16_t, 8> dummy{};
    EXPECT_FALSE(verifyRecord(r, dummy.data(), dummy.size() * sizeof(uint16_t)));
}

TEST(HmcolRecordHeader, ValidRecordVerifies) {
    constexpr int32_t gridHeight = 4;
    std::vector<uint16_t> intensity(gridHeight, 0);
    intensity[0] = 100;
    intensity[1] = 250;
    intensity[2] = 0;
    intensity[3] = 65535;

    RecordHeader r{};
    r.bucketStartMs = 1'714'176'000'000 + 60000; // bucket 1 of the day
    r.bucketEndMs   = r.bucketStartMs + 60000;
    r.minPrice = 100000.0;
    r.maxPrice = 102048.0;
    r.tickSize = (r.maxPrice - r.minPrice) / gridHeight;
    r.liquidityScale = 1.0;
    r.flags = 0;
    r.recordCrc32 = crc32(intensity.data(), intensity.size() * sizeof(uint16_t));

    EXPECT_TRUE(verifyRecord(r, intensity.data(), intensity.size() * sizeof(uint16_t)));
}

TEST(HmcolRecordHeader, TamperedPayloadFails) {
    constexpr int32_t gridHeight = 4;
    std::vector<uint16_t> intensity(gridHeight, 7);

    RecordHeader r{};
    r.bucketStartMs = 1;
    r.recordCrc32 = crc32(intensity.data(), intensity.size() * sizeof(uint16_t));
    ASSERT_TRUE(verifyRecord(r, intensity.data(), intensity.size() * sizeof(uint16_t)));

    intensity[2] = 9; // flip a byte
    EXPECT_FALSE(verifyRecord(r, intensity.data(), intensity.size() * sizeof(uint16_t)));
}

// ---------- Slot math ----------

TEST(HmcolSlotMath, RecordStrideMatchesLayout) {
    EXPECT_EQ(recordStride(2048, kLiquidityFormatU16),
              sizeof(RecordHeader) + 2u * 2048u * sizeof(uint16_t));
    EXPECT_EQ(recordStride(2048, kLiquidityFormatNone),
              sizeof(RecordHeader) + 2048u * sizeof(uint16_t));
}

TEST(HmcolSlotMath, SlotOffsetSkipsHeader) {
    EXPECT_EQ(slotOffset(0, 100, kLiquidityFormatNone), sizeof(FileHeader));
    EXPECT_EQ(slotOffset(1, 100, kLiquidityFormatNone),
              sizeof(FileHeader) + recordStride(100, kLiquidityFormatNone));
    EXPECT_EQ(slotOffset(7, 4, kLiquidityFormatU16),
              sizeof(FileHeader) + 7u * recordStride(4, kLiquidityFormatU16));
}

TEST(HmcolSlotMath, SlotForBucket1m) {
    constexpr int64_t day = 1'714'176'000'000; // some UTC midnight
    constexpr int64_t tf = 60000;
    EXPECT_EQ(slotForBucket(day, day, tf), 0);
    EXPECT_EQ(slotForBucket(day + tf, day, tf), 1);
    EXPECT_EQ(slotForBucket(day + 60 * tf, day, tf), 60);
    EXPECT_EQ(slotForBucket(day + 1440 * tf - tf, day, tf), 1439); // last 1m slot of day
}

TEST(HmcolSlotMath, DayStartForEpochUtcMidnight) {
    constexpr int64_t kDay = 86'400'000;
    // 1970-01-02 00:00:00 UTC
    EXPECT_EQ(dayStartForEpoch(kDay), kDay);
    // 1970-01-02 12:34:56.789
    const int64_t mid = kDay + 12 * 3600'000 + 34 * 60'000 + 56'789;
    EXPECT_EQ(dayStartForEpoch(mid), kDay);
    // exactly midnight rounds to itself
    EXPECT_EQ(dayStartForEpoch(0), 0);
    // negative
    EXPECT_EQ(dayStartForEpoch(-1), -kDay);
    EXPECT_EQ(dayStartForEpoch(-kDay), -kDay);
}
