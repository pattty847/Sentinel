#include <gtest/gtest.h>

#include "Crc32.hpp"

#include <array>
#include <cstring>
#include <string>
#include <vector>

using namespace sentinel;

// ---------- CRC32 ----------

TEST(Crc32, EmptyInputIsZero) {
    EXPECT_EQ(crc32(nullptr, 0), 0u);
}

TEST(Crc32, KnownVectors) {
    // Standard CRC-32 (zlib/PNG/IEEE) check values.
    const char* a = "a";
    EXPECT_EQ(crc32(a, 1), 0xE8B7BE43u);

    const char* abc = "abc";
    EXPECT_EQ(crc32(abc, 3), 0x352441C2u);

    const char* check = "123456789";
    EXPECT_EQ(crc32(check, 9), 0xCBF43926u);
}

TEST(Crc32, IncrementalMatchesOneShot) {
    const std::string s = "the quick brown fox jumps over the lazy dog";
    const uint32_t whole = crc32(s.data(), s.size());

    uint32_t streamed = 0;
    streamed = crc32Update(streamed, s.data(), 10);
    streamed = crc32Update(streamed, s.data() + 10, 15);
    streamed = crc32Update(streamed, s.data() + 25, s.size() - 25);
    EXPECT_EQ(whole, streamed);
}

