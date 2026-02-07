// Sentinel — Protocol Validation Tests
// Role: Verify schema gating, bounds guardrails, and base64 estimate logic (INV-020, FM-014/015)
#include <gtest/gtest.h>
#include "protocol/ProtocolValidation.hpp"

using namespace protocol::validation;

// =============================================================================
// Schema Version Gating (INV-020)
// =============================================================================

TEST(ProtocolValidation, SchemaMatchAccepted) {
    nlohmann::json msg = {{"type", "heatmap_slice"}, {"schema_version", 1}};
    EXPECT_TRUE(isSchemaCompatible(msg, 1));
}

TEST(ProtocolValidation, SchemaMismatchRejected) {
    nlohmann::json msg = {{"type", "heatmap_slice"}, {"schema_version", 99}};
    EXPECT_FALSE(isSchemaCompatible(msg, 1));
}

TEST(ProtocolValidation, SchemaMissingRejected) {
    nlohmann::json msg = {{"type", "heatmap_slice"}};
    EXPECT_FALSE(isSchemaCompatible(msg, 1));
    EXPECT_EQ(extractSchemaVersion(msg), -1);
}

TEST(ProtocolValidation, SchemaNonIntegerRejected) {
    nlohmann::json msg = {{"type", "heatmap_slice"}, {"schema_version", "one"}};
    EXPECT_FALSE(isSchemaCompatible(msg, 1));

    nlohmann::json msg2 = {{"type", "heatmap_slice"}, {"schema_version", 1.5}};
    EXPECT_FALSE(isSchemaCompatible(msg2, 1));

    nlohmann::json msg3 = {{"type", "heatmap_slice"}, {"schema_version", nullptr}};
    EXPECT_FALSE(isSchemaCompatible(msg3, 1));
}

// =============================================================================
// Grid Height Bounds (FM-015)
// =============================================================================

TEST(ProtocolValidation, GridHeightValidRange) {
    EXPECT_TRUE(isGridHeightValid(1));
    EXPECT_TRUE(isGridHeightValid(2048));
    EXPECT_TRUE(isGridHeightValid(protocol::SentinelProtocol::kMaxGridHeight));
}

TEST(ProtocolValidation, GridHeightAbsurdRejected) {
    EXPECT_FALSE(isGridHeightValid(0));
    EXPECT_FALSE(isGridHeightValid(-1));
    EXPECT_FALSE(isGridHeightValid(-999999));
    EXPECT_FALSE(isGridHeightValid(protocol::SentinelProtocol::kMaxGridHeight + 1));
    EXPECT_FALSE(isGridHeightValid(999999999));
}

// =============================================================================
// Base64 Estimate + Payload Bounds
// =============================================================================

TEST(ProtocolValidation, Base64EstimateCorrect) {
    // "AAAA" = 4 base64 chars = 3 decoded bytes
    EXPECT_EQ(estimateBase64DecodedBytes("AAAA"), 3u);
    // "AA==" = 4 chars, 2 padding = 1 decoded byte
    EXPECT_EQ(estimateBase64DecodedBytes("AA=="), 1u);
    // "AAA=" = 4 chars, 1 padding = 2 decoded bytes
    EXPECT_EQ(estimateBase64DecodedBytes("AAA="), 2u);
    // empty
    EXPECT_EQ(estimateBase64DecodedBytes(""), 0u);
}

TEST(ProtocolValidation, PayloadSizeBoundsEnforced) {
    EXPECT_TRUE(isPayloadSizeValid(0));
    EXPECT_TRUE(isPayloadSizeValid(protocol::SentinelProtocol::kMaxPayloadBytes));
    EXPECT_FALSE(isPayloadSizeValid(protocol::SentinelProtocol::kMaxPayloadBytes + 1));
    EXPECT_FALSE(isPayloadSizeValid(999999999));
}

TEST(ProtocolValidation, LargeBase64EstimateExceedsBounds) {
    // 400000 base64 chars -> ~300000 decoded bytes > 256KB limit
    std::string huge(400000, 'A');
    size_t estimated = estimateBase64DecodedBytes(huge);
    EXPECT_GT(estimated, static_cast<size_t>(protocol::SentinelProtocol::kMaxPayloadBytes));
    EXPECT_FALSE(isPayloadSizeValid(estimated));
}

// =============================================================================
// Footprint Stub Validation (INV-020, INV-021)
// =============================================================================

TEST(ProtocolValidation, FootprintSchemaGating) {
    const int kFpVer = protocol::SentinelProtocol::kFootprintSchemaVersion;

    // Valid footprint schema
    nlohmann::json good = {{"type", "footprint_slice"}, {"schema_version", kFpVer}};
    EXPECT_TRUE(isSchemaCompatible(good, kFpVer));

    // Wrong version
    nlohmann::json bad = {{"type", "footprint_slice"}, {"schema_version", kFpVer + 1}};
    EXPECT_FALSE(isSchemaCompatible(bad, kFpVer));

    // Missing schema_version
    nlohmann::json missing = {{"type", "footprint_slice"}};
    EXPECT_FALSE(isSchemaCompatible(missing, kFpVer));
}

TEST(ProtocolValidation, FootprintQ16PayloadShape) {
    // INV-021: footprint q16 delta = 2 bytes per price level (int16_t).
    // Payload must be exactly gridHeight * sizeof(int16_t).
    constexpr int bytesPerLevel = sizeof(int16_t);

    // Typical grid heights
    EXPECT_EQ(2048 * bytesPerLevel, 4096);
    EXPECT_TRUE(isPayloadSizeValid(2048 * bytesPerLevel));

    // Max valid grid at 2 bytes/level = 65536 * 2 = 128KB, under 256KB limit
    const size_t maxPayload = protocol::SentinelProtocol::kMaxGridHeight * bytesPerLevel;
    EXPECT_EQ(maxPayload, 131072u);
    EXPECT_TRUE(isPayloadSizeValid(maxPayload));

    // Absurd grid_height that exceeds bounds
    EXPECT_FALSE(isGridHeightValid(protocol::SentinelProtocol::kMaxGridHeight + 1));
}
