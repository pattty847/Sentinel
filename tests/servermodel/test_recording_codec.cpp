#include <gtest/gtest.h>

#include "servermodel/RecordingCodec.hpp"

using namespace recording;

TEST(RecordingCodec, SizeRoundTripsWithinPrecision) {
    for (double size : {1e-6, 3.3e-5, 0.01, 0.5, 1.0, 7.25, 120.0, 5000.0, 250000.0}) {
        const uint16_t code = encodeSize(size);
        ASSERT_GT(code, 0) << size;
        const double back = decodeSize(code);
        EXPECT_NEAR(back / size, 1.0, 0.0005) << size;  // half a code step is ~0.042%
    }
}

TEST(RecordingCodec, EmptyAndInvalidSizesAreZero) {
    EXPECT_EQ(encodeSize(0.0), 0);
    EXPECT_EQ(encodeSize(-1.0), 0);
    EXPECT_EQ(encodeSize(std::nan("")), 0);
    EXPECT_EQ(encodeSize(INFINITY), 0);
    EXPECT_DOUBLE_EQ(decodeSize(0), 0.0);
    EXPECT_DOUBLE_EQ(decodeSize(kSideAskBit), 0.0);  // side bit alone is still empty
}

TEST(RecordingCodec, TinyAndHugeSizesClampInsteadOfVanishing) {
    EXPECT_EQ(encodeSize(1e-12), 1);
    EXPECT_EQ(encodeSize(1e12), kMaxCode);
}

TEST(RecordingCodec, CodesAreMonotonicSoMaxOfCodesIsMaxOfSizes) {
    uint16_t prev = 0;
    for (double size = 1e-6; size < 1e6; size *= 1.37) {
        const uint16_t code = encodeSize(size);
        EXPECT_GE(code, prev);
        prev = code;
    }
}

TEST(RecordingCodec, SideBitDoesNotChangeMagnitude) {
    const uint16_t code = encodeSize(42.0);
    const uint16_t ask = withSide(code, true);
    EXPECT_TRUE(isAsk(ask));
    EXPECT_FALSE(isAsk(withSide(code, false)));
    EXPECT_DOUBLE_EQ(decodeSize(ask), decodeSize(code));
}

TEST(RecordingCodec, VarintRoundTripAndTruncation) {
    std::vector<uint8_t> buf;
    const uint64_t values[] = {0, 1, 127, 128, 300, 1u << 20, 0xFFFFFFFFull, ~0ull};
    for (uint64_t v : values) putVarint(buf, v);
    size_t pos = 0;
    for (uint64_t v : values) {
        uint64_t got = 0;
        ASSERT_TRUE(getVarint(buf.data(), buf.size(), pos, got));
        EXPECT_EQ(got, v);
    }
    EXPECT_EQ(pos, buf.size());

    std::vector<uint8_t> cut = {0x80, 0x80};  // continuation bits with no terminator
    pos = 0;
    uint64_t got = 0;
    EXPECT_FALSE(getVarint(cut.data(), cut.size(), pos, got));
}

TEST(RecordingCodec, ZigzagRoundTrip) {
    for (int64_t v : {0LL, 1LL, -1LL, 63LL, -64LL, 1LL << 40, -(1LL << 40)}) {
        EXPECT_EQ(unzigzag(zigzag(v)), v);
    }
    EXPECT_LT(zigzag(-1), 4u);
}

TEST(RecordingCodec, RejectsOverflowingVarintTenthByte) {
    std::vector<uint8_t> input(10, 0xFF);
    input.back() = 2;
    size_t pos = 0;
    uint64_t result = 0;
    EXPECT_FALSE(getVarint(input.data(), input.size(), pos, result));
}
