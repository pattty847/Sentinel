// Validate the complete header, table and all sections before exposing a day.
#include "FlowDayFile.hpp"
#include "FlowChunkPrimitives.hpp"
#include "InstrumentSpec.hpp"
#include <algorithm>
namespace sentinel::flow::chunks {
namespace {
void validateInstrument(const std::string& instrument) {
    const auto colon = instrument.find(':');
    if (colon == std::string::npos || instrument.size() > 72) detail::In::fail("day instrument");
    const auto venue = instrument.substr(0, colon);
    if (venue != "coinbase" && venue != "kalshi") detail::In::fail("day venue");
    validateInstrumentId(instrument.substr(colon + 1), venue == "coinbase" ? Venue::Coinbase : Venue::Kalshi);
}
}

// Serializes the supplied sections and metadata; parseDayFile is the full
// load-validation boundary, including semantic checks of every sealed section.
std::vector<uint8_t> encodeDayFile(const DayFile& d) {
    validateInstrument(d.instrument);
    for (size_t i = 26; i < DaySections; ++i)
        if (d.sections[i].sealed || !d.sections[i].payload.empty())
            throw std::invalid_argument("flow big trades are not implemented");
    detail::Out table;
    uint64_t offset = 0; // relative to the end of the table, patched below
    std::vector<uint64_t> starts;
    for (const auto& s : d.sections) { starts.push_back(offset); offset += s.payload.size(); }
    detail::Out h;
    for (char ch : {'F', 'L', 'C', '1'}) h.b.push_back(uint8_t(ch));
    h.fixed(1, 2);           // file version
    h.fixed(0, 2);           // header length, patched below
    h.fixed(0, 4);           // header CRC, patched below
    if (d.instrument.empty() || d.instrument.size() > 72) throw std::invalid_argument("flow chunk instrument");
    h.fixed(d.instrument.size(), 1);
    h.b.insert(h.b.end(), d.instrument.begin(), d.instrument.end());
    h.fixed(uint64_t(d.dayStartMs), 8);
    for (auto v : {d.grid.rowTickUnits, d.grid.priceScale, d.grid.baseScale, d.grid.quoteScale}) h.fixed(uint64_t(v), 8);
    h.fixed(d.ruleVersion, 1); h.fixed(d.sealWindowMs, 4);
    h.fixed(d.lateTrades, 4); h.fixed(d.rejectedTrades, 4); h.fixed(d.regressions, 4);
    h.fixed(d.ruleIdentity, 8);
    h.fixed(0, 4);           // table CRC, patched below
    const size_t headerLength = h.b.size();
    const uint64_t base = headerLength + DaySections * TableEntryBytes;
    for (size_t i = 0; i < DaySections; ++i) {
        const auto& s = d.sections[i];
        table.fixed(s.payload.empty() ? 0 : base + starts[i], 4);
        table.fixed(s.summaryLength, 4); table.fixed(s.payload.size(), 4);
        table.fixed(s.payload.empty() ? 0 : crc32(s.payload), 4);
        table.fixed(s.sealed, 1); table.fixed(s.sourcePrefix, 8); table.fixed(s.stepExponent, 1);
    }
    if (base + offset > UINT32_MAX) throw std::invalid_argument("flow chunk day file too large");
    const auto tableCrc = crc32(table.b);
    for (int i = 0; i < 4; ++i) h.b[headerLength - 4 + size_t(i)] = uint8_t(tableCrc >> (8 * i));
    h.b[6] = uint8_t(headerLength); h.b[7] = uint8_t(headerLength >> 8);
    const auto crc = crc32(h.b);
    for (int i = 0; i < 4; ++i) h.b[8 + i] = uint8_t(crc >> (8 * i));
    h.b.insert(h.b.end(), table.b.begin(), table.b.end());
    for (const auto& s : d.sections) h.b.insert(h.b.end(), s.payload.begin(), s.payload.end());
    return std::move(h.b);
}

// The payload key of a flow entry (0-25) of a day file.
struct EntryKey { int64_t levelMs, startMs; uint32_t slotCount; };
EntryKey flowEntryKey(size_t entry, int64_t dayStartMs) {
    constexpr int64_t Minute = 60'000, Hour = 60 * Minute, Day = 24 * Hour;
    if (entry < 24) return {Minute, dayStartMs + int64_t(entry) * Hour, 60};
    if (entry == 24) return {Hour, dayStartMs, 24};
    return {Day, dayStartMs, 1};
}

// Validates the header CRC, the table CRC, the table (sections back to back in
// table order; a sealed entry has a payload, an absent one none; a flow entry's
// summary length is a decodable summary prefix of a payload decodable at its
// step; a trades.big or absent entry has summary length 0 and step exponent 0)
// and every section CRC.
DayFile parseDayFile(std::span<const uint8_t> bytes) {
    using detail::In;
    In in{bytes};
    for (char ch : {'F', 'L', 'C', '1'}) if (in.fixed(1) != uint8_t(ch)) In::fail("day magic");
    if (in.fixed(2) != 1) In::fail("day version");
    const auto headerLength = in.fixed(2);
    const auto storedCrc = uint32_t(in.fixed(4));
    if (headerLength < 16 || headerLength > bytes.size()) In::fail("day header length");
    std::vector<uint8_t> header(bytes.begin(), bytes.begin() + ptrdiff_t(headerLength));
    std::fill(header.begin() + 8, header.begin() + 12, 0);
    if (crc32(header) != storedCrc) In::fail("day header crc");
    DayFile d;
    const auto len = in.fixed(1);
    if (len == 0 || len > 72 || len > bytes.size() - in.p) In::fail("day instrument");
    d.instrument.assign(reinterpret_cast<const char*>(bytes.data() + in.p), len);
    in.p += len;
    validateInstrument(d.instrument);
    d.dayStartMs = int64_t(in.fixed(8));
    d.grid = {int64_t(in.fixed(8)), int64_t(in.fixed(8)), int64_t(in.fixed(8)), int64_t(in.fixed(8))};
    if (d.grid.rowTickUnits <= 0 || d.grid.priceScale <= 0 || d.grid.baseScale <= 0 || d.grid.quoteScale <= 0)
        In::fail("day grid");
    d.ruleVersion = uint8_t(in.fixed(1)); d.sealWindowMs = uint32_t(in.fixed(4));
    d.lateTrades = uint32_t(in.fixed(4)); d.rejectedTrades = uint32_t(in.fixed(4)); d.regressions = uint32_t(in.fixed(4));
    d.ruleIdentity = in.fixed(8);
    const auto tableCrc = uint32_t(in.fixed(4));
    if (in.p != headerLength || d.dayStartMs % 86'400'000 || d.dayStartMs > INT64_MAX - 86'400'000) In::fail("day header");
    uint64_t next = headerLength + DaySections * TableEntryBytes;
    if (next > bytes.size()) In::fail("day table truncated");
    if (crc32(bytes.subspan(headerLength, DaySections * TableEntryBytes)) != tableCrc) In::fail("day table crc");
    for (size_t i = 0; i < DaySections; ++i) {
        auto& s = d.sections[i];
        const auto offset = in.fixed(4), summary = in.fixed(4), length = in.fixed(4), crc = in.fixed(4);
        const auto sealed = in.fixed(1);
        s.sourcePrefix = in.fixed(8);
        const auto stepExponent = in.fixed(1);
        const bool flow = i < 26;
        // trades.big entries stay zero until their payload and source rules land.
        if ((i >= 24 && s.sourcePrefix) || (!flow && (sealed || length)) || sealed > 1 || bool(sealed) != (length > 0) || (length ? offset != next : offset != 0) ||
            length > bytes.size() - next || length > 16 * 1024 * 1024 || (flow && sealed ? stepExponent > MaxStepExponent : stepExponent != 0) ||
            (flow ? (sealed && (summary == 0 || summary > length)) || (!sealed && summary)
                  : summary != 0)) In::fail("day table");
        s.sealed = sealed; s.summaryLength = uint32_t(summary); s.stepExponent = uint8_t(stepExponent);
        s.payload.assign(bytes.begin() + ptrdiff_t(next), bytes.begin() + ptrdiff_t(next + length));
        if (length && crc32(s.payload) != crc) In::fail("day section crc");
        if (flow && sealed) {
            const auto k = flowEntryKey(i, d.dayStartMs);
            decode(std::span(s.payload).first(summary), k.levelMs, k.startMs, k.slotCount, true);
            decode(s.payload, k.levelMs, k.startMs, k.slotCount, false, uint64_t(1) << stepExponent);
        }
        next += length;
    }
    if (next != bytes.size()) In::fail("day trailing bytes");
    return d;
}

}
