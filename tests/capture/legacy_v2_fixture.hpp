#pragma once
// Test-only RAWL2 v2 writer. Production capture writes v1 only (one product per
// connection, 2026-10-02); the verifier keeps its v2 reader for the archive
// written 2026-09-30..10-02. This reproduces that writer's routing (one
// connection, several products, range receipts, connection-wide gap markers)
// synchronously so the verifier's v2 path stays covered by fixtures.
#include "capture/CaptureSession.hpp"
#include <vector>

namespace sentinel::capture {
struct LegacyV2FixtureWriter {
    static std::unique_ptr<Writer> make(WriterConfig config, nlohmann::json metadata);
};
// Writes every record to every product stream as the v2 Session did, then
// seals all files. Records must already be in submit order; a CaptureStopped
// record (if any) is used as the terminal marker. Throws on any writer error.
void writeLegacyV2(std::vector<ProductCapture> products, const std::vector<Record>& records);
} // namespace sentinel::capture
