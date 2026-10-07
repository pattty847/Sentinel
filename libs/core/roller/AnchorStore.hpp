#pragma once
#include "JournalReader.hpp"

namespace sentinel::roller {
// Derived cache only; no RAWL2 records or exchange events are created here.
struct AnchorSection {
    uint32_t version = 1;
    std::vector<uint8_t> bytes;
};
struct AnchorIdentity {
    std::string product;
    int64_t dayMs = 0, fromMs = 0, toMs = 0;
    uint64_t configHash = 0;
    bool operator==(const AnchorIdentity&) const = default;
};
struct ReplayAnchor {
    AnchorIdentity identity;
    int64_t boundaryMs = 0;
    JournalPos pos; // inclusive: state after this durable record
    JournalPos gridSnapshot; // original exchange snapshot, possibly in day N-1
    double referenceMid = 0;
    nlohmann::json metadata; // original snapshot's product metadata
    AnchorSection feed, book, recorder;
};
class AnchorStore {
public:
    static constexpr uint32_t Version = 1;
    static constexpr size_t MaxBytes = 128 * 1024 * 1024;
    static std::filesystem::path path(const std::filesystem::path& root, const ReplayAnchor&);
    static std::vector<uint8_t> encode(const ReplayAnchor&);
    // Validates framing, complete CRC, compression, schema, identities and all
    // section versions before returning. Opaque section semantics belong to
    // their consumers, which must validate before installing any state.
    static ReplayAnchor decode(const std::vector<uint8_t>&);
    static ReplayAnchor load(const std::filesystem::path& file);
    static std::vector<std::filesystem::path> candidates(const std::filesystem::path& root,
        const std::string& product, int64_t dayMs);
    // Journal-time retention: midnight survives forever. Never traverses other stores.
    static void prune(const std::filesystem::path& root, const std::string& product, int64_t nowMs);
    static void write(const std::filesystem::path& root, const ReplayAnchor&);
    // Missing/corrupt/incompatible cache is a miss, with an explicit reason.
    // Cross-run ceiling ordering belongs to the journal inventory: conservatively
    // reject a different run here when a ceiling is supplied.
    static std::optional<ReplayAnchor> read(const std::filesystem::path& file,
        const AnchorIdentity& expected, int64_t maxBoundaryMs,
        std::optional<JournalPos> ceiling = {}, std::string* rejection = nullptr);
};
} // namespace sentinel::roller
