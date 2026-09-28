#include "Hmc2Store.hpp"
#include "HmcolFormat.hpp"
#include "PersistenceIo.hpp"
#include "SentinelLogging.hpp"
#include <zstd.h>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <utility>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <deque>

namespace recording {
namespace {
namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;
using namespace sentinel::persistence;
// The deployed absolute format already used schema 2. Schema 1 is also
// accepted as absolute; schema 3 is the first temporal-delta format.
constexpr uint16_t kSchema = 3;
constexpr uint16_t kHourSchema = 4;
uint16_t writeSchema(const Hmc2Header &h) { return h.tfMs == 3'600'000 ? kHourSchema : kSchema; }
constexpr int64_t kKeyframeMs = 15 * 60'000;
constexpr size_t kMaxEntries = (kHmc2MaxRawLen - 93) / 5;
constexpr size_t kMaxWriters = 64;
constexpr size_t kPrefix = 14; // magic:u32, version:u16, length:u32, crc:u32
constexpr uint32_t kMaxHeader = 65536;
struct Corrupt : std::runtime_error {
    using std::runtime_error::runtime_error;
};

[[noreturn]] void fail(const std::string &message) {
    sLog_Error("Hmc2Store: " << message);
    throw std::runtime_error("Hmc2Store: " + message);
}
void check(bool ok, const std::string &message) {
    if (!ok)
        fail(message);
}
bool safeName(const std::string &s) {
    return !s.empty() && s.size() <= 255 && s != "." && s != ".." &&
           std::all_of(s.begin(), s.end(), [](unsigned char c) {
               return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
                      c == '_' || c == '.';
           });
}
void validate(const Hmc2Header &h) {
    check(safeName(h.symbol) && safeName(h.layer), "invalid symbol/layer path component");
    check((h.tfMs == 60'000 || h.tfMs == 3'600'000) && h.rowTickUnits > 0 && std::isfinite(h.priceScale) &&
              h.priceScale > 0 && std::isfinite(h.sizeScale.floor) && h.sizeScale.floor > 0 &&
              std::isfinite(h.sizeScale.codesPerOctave) && h.sizeScale.codesPerOctave > 0,
          "invalid header configuration");
}
template <class T> void put(Bytes &b, T x) {
    using U = std::conditional_t<sizeof(T) == 8, uint64_t, std::conditional_t<sizeof(T) == 4, uint32_t, uint16_t>>;
    U u = std::bit_cast<U>(x);
    for (size_t i = 0; i < sizeof(T); ++i)
        b.push_back(static_cast<uint8_t>(u >> (i * 8)));
}
struct Cursor {
    const Bytes &b;
    size_t p = 0;
    template <class T> T get() {
        if (p > b.size() || sizeof(T) > b.size() - p)
            throw Corrupt("short field");
        using U = std::conditional_t<sizeof(T) == 8, uint64_t, std::conditional_t<sizeof(T) == 4, uint32_t, uint16_t>>;
        U u = 0;
        for (size_t i = 0; i < sizeof(T); ++i)
            u |= static_cast<U>(b[p++]) << (8 * i);
        return std::bit_cast<T>(u);
    }
    std::string string() {
        const auto n = get<uint16_t>();
        if (n > b.size() - p)
            throw Corrupt("short string");
        std::string s(reinterpret_cast<const char *>(b.data() + p), n);
        p += n;
        return s;
    }
};
void putString(Bytes &b, const std::string &s) {
    put(b, static_cast<uint16_t>(s.size()));
    b.insert(b.end(), s.begin(), s.end());
}
Bytes headerBody(const Hmc2Header &h) {
    Bytes b;
    putString(b, h.symbol);
    putString(b, h.layer);
    put(b, h.tfMs);
    put(b, h.priceScale);
    put(b, h.rowTickUnits);
    put(b, h.sizeScale.floor);
    put(b, h.sizeScale.codesPerOctave);
    put(b, h.configHash);
    return b;
}
Bytes encodeHeader(const Hmc2Header &h) {
    auto body = headerBody(h);
    Bytes b;
    put(b, kHmc2Magic);
    put(b, writeSchema(h));
    put(b, static_cast<uint32_t>(kPrefix + body.size()));
    put(b, uint32_t{0});
    b.insert(b.end(), body.begin(), body.end());
    const uint32_t crc = hmcol::crc32(b.data(), b.size());
    for (size_t i = 0; i < 4; ++i)
        b[10 + i] = static_cast<uint8_t>(crc >> (8 * i));
    return b;
}
bool readAt(std::ifstream &f, uint64_t pos, Bytes &b, size_t n) {
    b.resize(n);
    f.clear();
    f.seekg(static_cast<std::streamoff>(pos));
    return static_cast<bool>(f.read(reinterpret_cast<char *>(b.data()), static_cast<std::streamsize>(n)));
}
Hmc2Header readHeader(std::ifstream &f, uint64_t &pos, uint16_t &schema) {
    Bytes b;
    if (!readAt(f, 0, b, kPrefix))
        throw Corrupt("incomplete header");
    Cursor c{b};
    const auto magic = c.get<uint32_t>();
    schema = c.get<uint16_t>();
    if (magic != kHmc2Magic || schema < 1 || schema > kHourSchema)
        throw Corrupt("header magic/schema");
    const auto n = c.get<uint32_t>();
    const auto crc = c.get<uint32_t>();
    if (n < kPrefix || n > kMaxHeader || !readAt(f, 0, b, n))
        throw Corrupt("header length");
    std::fill(b.begin() + 10, b.begin() + 14, 0);
    if (hmcol::crc32(b.data(), b.size()) != crc)
        throw Corrupt("header CRC");
    c.p = kPrefix;
    Hmc2Header h;
    h.symbol = c.string();
    h.layer = c.string();
    h.tfMs = c.get<int64_t>();
    h.priceScale = c.get<double>();
    h.rowTickUnits = c.get<int64_t>();
    h.sizeScale.floor = c.get<double>();
    h.sizeScale.codesPerOctave = c.get<double>();
    h.configHash = c.get<uint64_t>();
    if (c.p != b.size())
        throw Corrupt("header trailing bytes");
    try {
        validate(h);
    } catch (const std::runtime_error &e) {
        throw Corrupt(e.what());
    }
    pos = n;
    return h;
}
auto key(const Hmc2Entry &e) {
    return std::pair(e.row, e.isAsk);
}
void validateEntries(const std::vector<Hmc2Entry> &entries) {
    check(entries.size() <= kMaxEntries, "too many entries");
    for (size_t j = 0; j < entries.size(); ++j) {
        const auto &e = entries[j];
        check(!j || key(entries[j - 1]) < key(e), "entries must be unique and sorted by row/side");
        check(!j || entries[j - 1].row >= 0 || e.row <= INT64_MAX + entries[j - 1].row, "row delta overflow");
        check(e.twapCode <= kMaxCode && e.peakCode <= kMaxCode, "code contains side bit");
    }
}
void encodeRecord(const Hmc2Record &r, Bytes &b, const Hmc2Record *base) {
    // An instantaneous peak can legitimately have zero integrated TWAP. Use
    // an absolute record to preserve it without colliding with delta removals.
    if (base && std::any_of(r.entries.begin(), r.entries.end(), [](const auto &e) { return e.twapCode == 0; }))
        base = nullptr;
    b.clear();
    put(b, r.bucketStartMs);
    put(b, r.observedMs);
    put(b, r.flags);
    put(b, r.bidRowLo);
    put(b, r.bidRowHi);
    put(b, r.askRowLo);
    put(b, r.askRowHi);
    put(b, r.midOpen);
    put(b, r.midClose);
    put(b, r.midMin);
    put(b, r.midMax);
    put(b, uint32_t{0});      // changed-entry count, patched below
    b.push_back(base ? 1 : 0); // 0 keyframe, 1 delta; separate from observation flags
    put(b, base ? base->bucketStartMs : int64_t{0});
    uint32_t count = 0;
    int64_t previous = 0;
    bool overflow = false;
    auto writeEntry = [&](const Hmc2Entry &e, uint16_t oldTwap) {
        if (count && previous < 0 && e.row > INT64_MAX + previous) {
            overflow = true;
            return;
        }
        putVarint(b, zigzag(e.row - previous));
        b.push_back(e.isAsk ? 1 : 0);
        putVarint(b, zigzag(int64_t{e.twapCode} - oldTwap));
        putVarint(b, zigzag(int64_t{e.peakCode} - e.twapCode));
        if (r.header.tfMs == 3'600'000)
            put(b, e.coveredMs);
        previous = e.row;
        ++count;
    };
    size_t old = 0;
    for (const auto &e : r.entries) {
        if (base) {
            while (old < base->entries.size() && key(base->entries[old]) < key(e)) {
                const auto &gone = base->entries[old++];
                writeEntry({gone.row, gone.isAsk, 0, 0}, gone.twapCode);
            }
        }
        const auto *prior = base && old < base->entries.size() && key(base->entries[old]) == key(e)
                                ? &base->entries[old++]
                                : nullptr;
        if (!prior || prior->twapCode != e.twapCode || prior->peakCode != e.peakCode)
            writeEntry(e, prior ? prior->twapCode : 0);
    }
    if (base) {
        while (old < base->entries.size()) {
            const auto &gone = base->entries[old++];
            writeEntry({gone.row, gone.isAsk, 0, 0}, gone.twapCode);
        }
    }
    if (r.header.tfMs == 3'600'000) {
        put(b, static_cast<uint32_t>(r.coverage.size()));
        for (const auto &run : r.coverage) {
            put(b, run.lo);
            put(b, run.hi);
            b.push_back(run.isAsk ? 1 : 0);
            put(b, run.coveredMs);
        }
    }
    // A sparse subset can have an unrepresentable row delta even though the
    // complete state does not. Large replacements can also exceed the raw cap.
    if (base && (overflow || b.size() > kHmc2MaxRawLen)) {
        encodeRecord(r, b, nullptr);
        return;
    }
    check(!overflow && b.size() <= kHmc2MaxRawLen, "raw record exceeds bounds");
    for (size_t j = 0; j < 4; ++j)
        b[80 + j] = static_cast<uint8_t>(count >> (8 * j));
}
Hmc2Record decodeRecord(const Hmc2Header &h, uint16_t schema, const Bytes &b, const Hmc2Record *base,
                        bool &missingBase) {
    Cursor c{b};
    Hmc2Record r;
    r.header = h;
    r.bucketStartMs = c.get<int64_t>();
    r.observedMs = c.get<uint32_t>();
    r.flags = c.get<uint32_t>();
    r.bidRowLo = c.get<int64_t>();
    r.bidRowHi = c.get<int64_t>();
    r.askRowLo = c.get<int64_t>();
    r.askRowHi = c.get<int64_t>();
    r.midOpen = c.get<double>();
    r.midClose = c.get<double>();
    r.midMin = c.get<double>();
    r.midMax = c.get<double>();
    if (r.bucketStartMs < kHmc2MinMs || r.bucketStartMs >= kHmc2EndMs || r.observedMs > h.tfMs ||
        r.bucketStartMs % h.tfMs != 0 || !std::isfinite(r.midOpen) ||
        !std::isfinite(r.midClose) || !std::isfinite(r.midMin) || !std::isfinite(r.midMax))
        throw Corrupt("record semantics");
    const auto n = c.get<uint32_t>();
    bool deltaRecord = false;
    if (schema >= kSchema) {
        if (c.p == b.size() || b[c.p] > 1)
            throw Corrupt("record kind");
        deltaRecord = b[c.p++] == 1;
        const auto predecessor = c.get<int64_t>();
        if ((!deltaRecord && predecessor != 0) ||
            (deltaRecord && (predecessor != r.bucketStartMs - h.tfMs ||
                             predecessor / kKeyframeMs != r.bucketStartMs / kKeyframeMs)))
            throw Corrupt("delta predecessor/boundary");
        missingBase = deltaRecord && (!base || base->bucketStartMs != predecessor);
    }
    if (schema == kHourSchema && (h.tfMs != 3'600'000 || deltaRecord))
        throw Corrupt("coverage schema requires absolute hours");
    if (h.tfMs == 3'600'000 && schema < kHourSchema)
        r.flags |= kApproximateCoverage;
    if (n > (b.size() - c.p) / (schema >= kSchema ? 4 : 5))
        throw Corrupt("entry count");
    const size_t maxEntries = schema < kSchema ? (kHmc2MaxRawLen - 84) / 5 : kMaxEntries;
    r.entries.reserve(std::min<size_t>(maxEntries, n + (deltaRecord && base ? base->entries.size() : 0)));
    size_t old = 0;
    auto add = [&](const Hmc2Entry &e) {
        if (r.entries.size() >= maxEntries)
            throw Corrupt("decoded entry count");
        r.entries.push_back(e);
    };
    auto signedVarint = [&]() {
        uint64_t v;
        if (!getVarint(b.data(), b.size(), c.p, v))
            throw Corrupt("entry varint");
        return unzigzag(v);
    };
    int64_t prev = 0;
    bool prevAsk = false;
    for (uint32_t j = 0; j < n; ++j) {
        const int64_t delta = signedVarint();
        if (j && (delta < 0 || prev > INT64_MAX - delta))
            throw Corrupt("row delta");
        const int64_t row = prev + delta;
        bool ask;
        int64_t twap, peak;
        if (schema < kSchema) {
            const auto code = c.get<uint16_t>();
            ask = isAsk(code);
            twap = code & kMaxCode;
            peak = c.get<uint16_t>();
        } else {
            if (c.p == b.size() || b[c.p] > 1)
                throw Corrupt("entry side");
            ask = b[c.p++] == 1;
            const auto twapDelta = signedVarint(), peakOffset = signedVarint();
            if (twapDelta < -kMaxCode || twapDelta > kMaxCode || peakOffset < -kMaxCode || peakOffset > kMaxCode)
                throw Corrupt("code offset");
            uint16_t prior = 0;
            if (deltaRecord && !missingBase) {
                while (old < base->entries.size() && key(base->entries[old]) < std::pair(row, ask))
                    add(base->entries[old++]);
                if (old < base->entries.size() && key(base->entries[old]) == std::pair(row, ask))
                    prior = base->entries[old++].twapCode;
            }
            twap = prior + twapDelta;
            peak = twap + peakOffset;
        }
        const auto covered = schema == kHourSchema ? c.get<uint32_t>() : r.observedMs;
        if (schema == kHourSchema && (!covered || covered > r.observedMs))
            throw Corrupt("entry coverage");
        if (j && row == prev && (prevAsk || !ask))
            throw Corrupt("entry order");
        if (!missingBase) {
            if (twap < 0 || twap > kMaxCode || peak < 0 || peak > kMaxCode ||
                (deltaRecord && twap == 0 && peak != 0))
                throw Corrupt("entry code/removal");
            if (!deltaRecord || twap != 0)
                add({row, ask, static_cast<uint16_t>(twap), static_cast<uint16_t>(peak), covered});
        }
        prev = row;
        prevAsk = ask;
    }
    if (deltaRecord && !missingBase)
        while (old < base->entries.size())
            add(base->entries[old++]);
    if (schema == kHourSchema) {
        const auto count = c.get<uint32_t>();
        if (count > (b.size() - c.p) / 21)
            throw Corrupt("coverage run count");
        for (uint32_t j = 0; j < count; ++j) {
            CoverageRun run;
            run.lo = c.get<int64_t>();
            run.hi = c.get<int64_t>();
            if (c.p == b.size() || b[c.p] > 1)
                throw Corrupt("coverage side");
            run.isAsk = b[c.p++] == 1;
            run.coveredMs = c.get<uint32_t>();
            if (run.lo > run.hi || !run.coveredMs || run.coveredMs > r.observedMs ||
                (!r.coverage.empty() && (run.isAsk < r.coverage.back().isAsk ||
                  (run.isAsk == r.coverage.back().isAsk && run.lo <= r.coverage.back().hi))))
                throw Corrupt("coverage run semantics");
            r.coverage.push_back(run);
        }
        for (const auto &e : r.entries) {
            const auto it = std::lower_bound(r.coverage.begin(), r.coverage.end(), std::pair(e.isAsk, e.row),
                [](const auto &run, const auto &key) { return std::pair(run.isAsk, run.hi) < key; });
            if (it == r.coverage.end() || it->isAsk != e.isAsk || it->lo > e.row || it->coveredMs != e.coveredMs)
                throw Corrupt("entry/run coverage mismatch");
        }
    }
    if (c.p != b.size())
        throw Corrupt("record trailing bytes");
    return r;
}
void sync(const fs::path &p, bool directory = false) {
    int error = 0;
    const bool ok = directory ? syncDirectory(p, error) : syncFilePath(p, error);
    check(ok, "sync path=" + p.string() + " error=" + std::to_string(error));
}
// Each store re-syncs the existing directory chain on first use, including
// directories left behind by an earlier process that crashed before syncing.
void mkdirs(const fs::path &p, std::set<fs::path> &synced) {
    const auto absolute = fs::absolute(p).lexically_normal();
    if (synced.contains(absolute))
        return;
    const auto parent = absolute.parent_path();
    if (parent != absolute)
        mkdirs(parent, synced);
    std::error_code ec;
    fs::create_directory(absolute, ec);
    check(!ec, "create directory " + absolute.string() + " error=" + ec.message());
    sync(absolute, true);
    if (parent != absolute)
        sync(parent, true);
    synced.insert(absolute);
}
std::string dayName(int64_t ms) {
    check(ms >= kHmc2MinMs && ms < kHmc2EndMs, "timestamp outside UTC years 2000-2200");
    using namespace std::chrono;
    const year_month_day ymd{floor<days>(sys_time<milliseconds>{milliseconds{ms}})};
    check(ymd.ok(), "bucket outside supported calendar");
    std::ostringstream s;
    s << std::setfill('0') << std::setw(4) << static_cast<int>(ymd.year()) << '-' << std::setw(2)
      << static_cast<unsigned>(ymd.month()) << '-' << std::setw(2) << static_cast<unsigned>(ymd.day());
    return s.str();
}
uint32_t generation(const fs::path &p) {
    auto stem = p.stem().string();
    const auto at = stem.find(".g");
    if (at == std::string::npos)
        return 0;
    try {
        return static_cast<uint32_t>(std::stoul(stem.substr(at + 2)));
    } catch (...) {
        return 0;
    }
}
std::vector<fs::path> files(const fs::path &dir, const std::string &day = {}, bool tolerant = false,
                            const std::function<bool()> &poll = {}) {
    std::vector<fs::path> out;
    auto problem = [&](const fs::path &path, const std::error_code &ec) {
        if (tolerant)
            sLog_Warning("Hmc2Store: skipped path=" << path.string() << " error=" << ec.message());
        else
            fail("enumerate " + path.string() + " error=" + ec.message());
    };
    std::error_code ec;
    const bool exists = fs::exists(dir, ec);
    if (ec) {
        problem(dir, ec);
        return out;
    }
    if (!exists)
        return out;
    fs::directory_iterator it(dir, ec), end;
    if (ec) {
        problem(dir, ec);
        return out;
    }
    while (it != end) {
        if (poll && !poll()) return {};
        const auto path = it->path();
        const auto name = path.filename().string();
        const bool regular = it->is_regular_file(ec);
        if (ec)
            problem(path, ec);
        else if (regular && path.extension() == ".hmc2" &&
                 (day.empty() || name == day + ".hmc2" || name.starts_with(day + ".g")))
            out.push_back(path);
        it.increment(ec);
        if (ec) {
            problem(dir, ec);
            break;
        }
    }
    std::sort(out.begin(), out.end(), [](const auto &a, const auto &b) {
        return std::tuple(a.filename().string().substr(0, 10), generation(a)) <
               std::tuple(b.filename().string().substr(0, 10), generation(b));
    });
    return out;
}
// Scan damage in chunks rather than issuing a seek/read syscall for every byte.
uint64_t nextMagic(std::ifstream &f, uint64_t pos, uint64_t end, Bytes &scratch,
                   const std::function<bool()> &poll = {}) {
    static constexpr std::array<uint8_t, 4> magic{'H', 'C', 'R', '2'};
    while (end - pos >= magic.size()) {
        if (poll && !poll()) return end;
        const size_t n = static_cast<size_t>(std::min<uint64_t>(65536, end - pos));
        if (!readAt(f, pos, scratch, n))
            fail("read during record resync");
        const auto it = std::search(scratch.begin(), scratch.end(), magic.begin(), magic.end());
        if (it != scratch.end())
            return pos + static_cast<uint64_t>(it - scratch.begin());
        pos += n - 3;
    }
    return end;
}
// A resync candidate is accepted only after framing, CRC, zstd and payload validation.
// A length running past EOF is terminal only if no later valid candidate exists.
template <class Sink> void scan(const fs::path &path, bool repair, Sink sink,
                               int64_t decodeStart = kHmc2MinMs, int64_t decodeEnd = kHmc2EndMs,
                               const std::function<void(const Hmc2Header &, uint16_t, const Bytes &, uint64_t, bool)> &metadataSink = {},
                               const std::function<bool()> &poll = {}) {
    std::ifstream f(path, std::ios::binary);
    check(f.is_open(), "open " + path.string());
    uint64_t pos = 0;
    uint16_t schema;
    const auto h = readHeader(f, pos, schema);
    std::optional<Hmc2Record> previous;
    std::error_code sizeError;
    const auto end = fs::file_size(path, sizeError);
    check(!sizeError, "file size " + path.string() + " error=" + sizeError.message());
    Bytes framing, compressed, raw, scratch;
    uint64_t tail = end;
    bool damaged = false;
    while (pos < end) {
        if (poll && !poll())
            return;
        if (damaged)
            previous.reset();
        if (end - pos < 16) {
            if (!damaged) {
                // At a known frame boundary, only a matching magic prefix can
                // be an incomplete frame header; arbitrary garbage is retained.
                readAt(f, pos, framing, static_cast<size_t>(end - pos));
                constexpr std::array<uint8_t, 4> magic{'H', 'C', 'R', '2'};
                if (std::equal(framing.begin(), framing.begin() + std::min<size_t>(4, framing.size()), magic.begin()))
                    tail = pos;
                else
                    damaged = true;
            }
            break;
        }
        if (!readAt(f, pos, framing, 16))
            fail("read " + path.string());
        Cursor c{framing};
        if (c.get<uint32_t>() != kHmc2RecordMagic) {
            damaged = true;
            pos = nextMagic(f, pos + 1, end, scratch, poll);
            continue;
        }
        const auto clen = c.get<uint32_t>(), rlen = c.get<uint32_t>(), crc = c.get<uint32_t>();
        if (rlen < 84 || rlen > kHmc2MaxRawLen || clen == 0 || clen > ZSTD_compressBound(kHmc2MaxRawLen)) {
            damaged = true;
            pos = nextMagic(f, pos + 1, end, scratch, poll);
            continue;
        }
        if (clen > end - pos - 16) {
            if (!damaged)
                tail = std::min(tail, pos);
            damaged = true;
            pos = nextMagic(f, pos + 1, end, scratch, poll);
            continue;
        }
        if (!readAt(f, pos + 16, compressed, clen))
            fail("read payload " + path.string());
        bool good = hmcol::crc32(compressed.data(), compressed.size()) == crc;
        if (good) {
            raw.resize(rlen);
            const auto frameSize = ZSTD_findFrameCompressedSize(compressed.data(), compressed.size());
            const auto n = ZSTD_decompress(raw.data(), raw.size(), compressed.data(), compressed.size());
            good = !ZSTD_isError(n) && n == rlen && frameSize == clen;
        }
        if (good && metadataSink) {
            metadataSink(h, schema, raw, pos, damaged);
            pos += 16 + clen;
            damaged = false;
            continue;
        }
        if (good) {
            std::optional<Hmc2Record> record;
            try {
                // No on-disk index: framing/zstd are scanned forward, but entry
                // reconstruction starts at most 15 minutes before the query.
                Cursor metadata{raw};
                const auto bucket = metadata.get<int64_t>();
                if (!repair && (bucket < decodeStart || bucket >= decodeEnd)) {
                    previous.reset();
                    pos += 16 + clen;
                    continue;
                }
                bool missingBase = false;
                record = decodeRecord(h, schema, raw, previous ? &*previous : nullptr, missingBase);
                if (missingBase) {
                    sLog_Warning("Hmc2Store: skipped delta without predecessor path=" << path.string()
                                 << " bucket=" << bucket);
                    previous.reset();
                    record.reset();
                    // A structurally valid orphan still establishes a frame
                    // boundary. Never truncate it as an incomplete write.
                    pos += 16 + clen;
                    tail = end;
                    damaged = false;
                    continue;
                }
            } catch (const Corrupt &e) {
                sLog_Warning("Hmc2Store: invalid payload path=" << path.string() << " error=" << e.what());
            }
            if (record) {
                if (damaged)
                    sLog_Warning("Hmc2Store: skipped interior corruption path=" << path.string() << " resume=" << pos);
                previous = *record;
                sink(std::move(*record));
                pos += 16 + clen;
                tail = end;
                damaged = false;
                continue;
            }
        }
        damaged = true;
        pos = nextMagic(f, pos + 1, end, scratch, poll);
    }
    if (damaged) {
        sLog_Warning("Hmc2Store: corrupt bytes skipped path=" << path.string());
        if (tail == end)
            sLog_Warning(
                "Hmc2Store: kept unverified tail after interior damage; any incomplete terminal frame is retained path="
                << path.string());
    }
    f.close();
    if (repair && tail < end) {
        fs::resize_file(path, tail);
        sync(path);
        sLog_Warning("Hmc2Store: truncated incomplete terminal frame path=" << path.string() << " offset=" << tail);
    }
}
} // namespace

bool ReadControl::poll() {
    if (status != ReadStatus::Complete)
        return false;
    if (stop.stop_requested())
        status = ReadStatus::Cancelled;
    else if (static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now() - started).count()) >= limits.maxWallMs)
        status = ReadStatus::Budget;
    return status == ReadStatus::Complete;
}
bool ReadControl::charge(uint64_t entries) {
    if (!poll())
        return false;
    if (sourceRecords >= limits.maxSourceRecords || entriesVisited > limits.maxEntriesVisited ||
        entries > limits.maxEntriesVisited - entriesVisited) {
        status = ReadStatus::Budget;
        return false;
    }
    ++sourceRecords;
    entriesVisited += entries;
    return true;
}
struct Hmc2Reader::Impl {
    fs::path root;
    struct Frame {
        int64_t bucket;
        uint64_t offset, work;
        bool delta, chain;
    };
    struct Index {
        Hmc2Header header;
        uint16_t schema = 0;
        uintmax_t size = 0;
        fs::file_time_type modified;
        std::vector<Frame> frames;
        std::shared_ptr<Hmc2Record> last;
        size_t lastIndex = SIZE_MAX;
    };
    std::map<fs::path, Index> indexes;
    struct Replay { fs::path path; size_t at; std::shared_ptr<Hmc2Record> record; };
    std::deque<Replay> replay; // one bounded keyframe interval, useful for backward pages
    size_t replayEntries = 0;
    void remember(const fs::path &path, size_t at, const std::shared_ptr<Hmc2Record> &record) {
        replay.push_back({path, at, record});
        replayEntries += record->entries.size() + record->coverage.size();
        while (replay.size() > 16 || replayEntries > 262144) {
            replayEntries -= replay.front().record->entries.size() + replay.front().record->coverage.size();
            replay.pop_front();
        }
        // File metadata must not pin 64 potentially maximal decoded frames.
        // Keep the current record and only records in the bounded replay window.
        for (auto &[_, index] : indexes)
            if (index.last && index.last != record &&
                std::none_of(replay.begin(), replay.end(), [&](const auto &r) { return r.record == index.last; })) {
                index.last.reset();
                index.lastIndex = SIZE_MAX;
            }
    }
    struct CachedAvailability {
        std::vector<std::tuple<fs::path, uintmax_t, fs::file_time_type>> signature;
        SeriesAvailability value;
    };
    std::map<fs::path, CachedAvailability> available;
    explicit Impl(fs::path p) : root(std::move(p)) {}
    Index *index(const fs::path &path, ReadControl &control) {
        if (!control.poll())
            return nullptr;
        try {
            const auto size = fs::file_size(path);
            const auto modified = fs::last_write_time(path);
            if (auto it = indexes.find(path); it != indexes.end()) {
                if (it->second.size == size && it->second.modified == modified)
                    return &it->second;
                indexes.erase(it);
            }
            std::erase_if(replay, [&](const auto &r) {
                if (r.path != path)
                    return false;
                replayEntries -= r.record->entries.size() + r.record->coverage.size();
                return true;
            });
            Index next;
            next.size = size;
            next.modified = modified;
            scan(path, false, [](Hmc2Record &&) {}, kHmc2MinMs, kHmc2EndMs,
                [&](const auto &h, uint16_t schema, const Bytes &raw, uint64_t offset, bool damaged) {
                    if (!control.charge(0)) return;
                    next.header = h;
                    next.schema = schema;
                    try {
                        Cursor c{raw};
                        const auto bucket = c.get<int64_t>();
                        c.p = 80;
                        const auto count = c.get<uint32_t>();
                        bool delta = false;
                        int64_t predecessor = 0;
                        if (schema >= kSchema) {
                            if (c.p == raw.size() || raw[c.p] > 1)
                                throw Corrupt("indexed record kind");
                            delta = raw[c.p++] == 1;
                            predecessor = c.get<int64_t>();
                        }
                        const bool chain = !damaged && !next.frames.empty() &&
                                           next.frames.back().bucket == predecessor &&
                                           predecessor == bucket - h.tfMs &&
                                           predecessor / kKeyframeMs == bucket / kKeyframeMs;
                        if (bucket < kHmc2MinMs || bucket >= kHmc2EndMs || bucket % h.tfMs)
                            throw Corrupt("indexed bucket");
                        next.frames.push_back({bucket, offset, count + (schema == kHourSchema ? raw.size() / 21 : 0), delta, chain});
                    } catch (const Corrupt &) {
                        // Preserve a physical-chain barrier; it can never match a legal bucket.
                        next.frames.push_back({0, offset, 0, false, false});
                    }
                }, [&] {
                    if (!control.poll()) return false;
                    if (control.sourceRecords >= control.limits.maxSourceRecords) {
                        control.status = ReadStatus::Budget;
                        return false;
                    }
                    return true;
                });
            if (!control.poll())
                return nullptr;
            if (indexes.size() >= 64)
                indexes.erase(indexes.begin());
            return &indexes.emplace(path, std::move(next)).first->second;
        } catch (const Corrupt &e) {
            sLog_Warning("Hmc2Reader: skipped corrupt header path=" << path.string() << " error=" << e.what());
            return nullptr;
        } catch (const std::exception &e) {
            sLog_Warning("Hmc2Reader: index failed path=" << path.string() << " error=" << e.what());
            control.status = ReadStatus::IoError;
            return nullptr;
        }
    }
    const Hmc2Record *load(const fs::path &path, Index &idx, size_t at, ReadControl &control) {
        if (!control.poll())
            return nullptr;
        for (const auto &cached : replay)
            if (cached.path == path && cached.at == at) {
                idx.last = cached.record;
                idx.lastIndex = at;
                break;
            }
        if (idx.last && idx.lastIndex == at) {
            // Cache hits still consume source/entry budget: projection is not free.
            return control.charge(idx.last->entries.size() + idx.last->coverage.size()) ? &*idx.last : nullptr;
        }
        const auto &frame = idx.frames[at];
        if (!frame.bucket)
            return nullptr;
        const Hmc2Record *base = nullptr;
        if (frame.delta) {
            if (!frame.chain)
                return nullptr;
            if (idx.last && idx.lastIndex == at - 1)
                base = &*idx.last;
            else
                base = load(path, idx, at - 1, control);
            if (!base)
                return nullptr;
        }
        if (!control.charge(frame.work + (base ? base->entries.size() : 0)))
            return nullptr;
        try {
            std::ifstream f(path, std::ios::binary);
            Bytes framing, compressed, raw;
            if (!readAt(f, frame.offset, framing, 16))
                throw Corrupt("indexed frame disappeared");
            Cursor c{framing};
            if (c.get<uint32_t>() != kHmc2RecordMagic)
                throw Corrupt("indexed magic");
            const auto clen = c.get<uint32_t>(), rlen = c.get<uint32_t>(), crc = c.get<uint32_t>();
            if (rlen > kHmc2MaxRawLen || clen > ZSTD_compressBound(kHmc2MaxRawLen) ||
                !readAt(f, frame.offset + 16, compressed, clen) || hmcol::crc32(compressed.data(), clen) != crc)
                throw Corrupt("indexed frame changed");
            raw.resize(rlen);
            const auto n = ZSTD_decompress(raw.data(), rlen, compressed.data(), clen);
            if (ZSTD_isError(n) || n != rlen || ZSTD_findFrameCompressedSize(compressed.data(), clen) != clen)
                throw Corrupt("indexed zstd");
            bool missing = false;
            auto record = decodeRecord(idx.header, idx.schema, raw, base, missing);
            if (missing)
                return nullptr;
            idx.last = std::make_shared<Hmc2Record>(std::move(record));
            idx.lastIndex = at;
            remember(path, at, idx.last);
            return control.poll() ? &*idx.last : nullptr;
        } catch (const Corrupt &e) {
            sLog_Warning("Hmc2Reader: skipped payload path=" << path.string() << " error=" << e.what());
            idx.last.reset();
            idx.lastIndex = SIZE_MAX;
            return nullptr;
        }
    }
    fs::path directory(const std::string &symbol, const std::string &layer, int64_t tf) {
        check(safeName(symbol) && safeName(layer) && (tf == 60'000 || tf == 3'600'000), "invalid reader query");
        return root / symbol / (layer + "-" + std::to_string(tf));
    }
};
Hmc2Reader::Hmc2Reader(fs::path root) : impl_(std::make_unique<Impl>(std::move(root))) {}
Hmc2Reader::~Hmc2Reader() = default;
ScanResult Hmc2Reader::visit(const std::string &symbol, const std::string &layer, int64_t tf,
                           int64_t start, int64_t end, const std::function<void(const Hmc2Record &)> &visitor,
                           ReadControl &control) {
    start = std::clamp(start, kHmc2MinMs, kHmc2EndMs);
    end = std::clamp(end, start, kHmc2EndMs);
    ScanResult result{start, start};
    auto &i = *impl_;
    const auto paths = files(i.directory(symbol, layer, tf), {}, false, [&] { return control.poll(); });
    // Only metadata for the requested days is retained. Iterate a day at a time
    // so arbitrarily long scans never retain an unbounded record map.
    for (int64_t day = start - start % 86'400'000; day < end; day += 86'400'000) {
        if (!control.poll())
            break;
        const auto name = dayName(day);
        std::map<int64_t, std::vector<std::pair<fs::path, size_t>>> buckets;
        for (const auto &path : paths) {
            if (path.filename().string().substr(0, 10) != name)
                continue;
            auto *idx = i.index(path, control);
            if (!idx) {
                if (!control.poll()) break;
                continue;
            }
            if (idx->header.symbol != symbol || idx->header.layer != layer || idx->header.tfMs != tf)
                continue;
            for (size_t n = 0; n < idx->frames.size(); ++n) {
                const auto t = idx->frames[n].bucket;
                if (t >= start && t < end)
                    buckets[t].emplace_back(path, n);
            }
        }
        if (!control.poll())
            break;
        for (const auto &[bucket, candidates] : buckets) {
            // Prove only the gap up to this bucket until its winning record is read.
            result.scannedEndMs = bucket;
            for (auto it = candidates.rbegin(); it != candidates.rend(); ++it) {
                auto *idx = i.index(it->first, control);
                if (!idx) {
                    if (!control.poll()) break;
                    continue;
                }
                if (const auto *record = i.load(it->first, *idx, it->second, control)) {
                    visitor(*record);
                    result.scannedEndMs = std::min(end, bucket + tf);
                    break;
                }
                if (!control.poll())
                    break;
            }
            if (!control.poll())
                break;
            result.scannedEndMs = std::min(end, bucket + tf);
        }
        if (!control.poll())
            break;
        result.scannedEndMs = std::min(end, day + 86'400'000);
    }
    result.status = control.status;
    return result;
}
SeriesAvailability Hmc2Reader::availability(const std::string &symbol, const std::string &layer, int64_t tf,
                                           ReadControl &control) {
    auto &i = *impl_;
    const auto dir = i.directory(symbol, layer, tf);
    Impl::CachedAvailability next;
    if (!control.poll())
        return {};
    for (const auto &path : files(dir, {}, false, [&] { return control.poll(); })) {
        if (!control.poll())
            return {};
        next.signature.emplace_back(path, fs::file_size(path), fs::last_write_time(path));
    }
    if (auto it = i.available.find(dir); it != i.available.end() && it->second.signature == next.signature)
        return it->second.value;
    // Edge files only in the usual case. Numeric generations are deduplicated by visit.
    for (bool latest : {false, true}) {
        std::set<std::string> days;
        for (const auto &[path, size, modified] : next.signature)
            days.insert(path.filename().string().substr(0, 10));
        while (!days.empty() && control.poll()) {
            const auto day = latest ? *days.rbegin() : *days.begin();
            std::vector<std::pair<int64_t, std::pair<fs::path, size_t>>> candidates;
            for (const auto &[path, size, modified] : next.signature) {
                if (path.filename().string().substr(0, 10) != day)
                    continue;
                if (auto *idx = i.index(path, control)) {
                    if (idx->header.symbol == symbol && idx->header.layer == layer && idx->header.tfMs == tf)
                        for (size_t n = 0; n < idx->frames.size(); ++n)
                            if (idx->frames[n].bucket)
                                candidates.push_back({idx->frames[n].bucket, {path, n}});
                }
            }
            // Stable order retains numeric generation and append order at equal buckets.
            std::stable_sort(candidates.begin(), candidates.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
            if (latest)
                std::reverse(candidates.begin(), candidates.end());
            for (const auto &[bucket, where] : candidates) {
                auto *idx = i.index(where.first, control);
                if (!idx)
                    break;
                if (const auto *r = i.load(where.first, *idx, where.second, control)) {
                    if (latest) {
                        next.value.latestMs = bucket;
                        next.value.latestHeader = r->header;
                    } else
                        next.value.oldestMs = bucket;
                    break;
                }
            }
            if (latest ? next.value.latestMs.has_value() : next.value.oldestMs.has_value())
                break;
            days.erase(day);
        }
    }
    if (control.poll()) {
        if (i.available.size() >= 64)
            i.available.erase(i.available.begin());
        i.available.insert_or_assign(dir, next);
    }
    return next.value;
}

struct Hmc2Store::Impl {
    fs::path root;
    LockHandle lock = noLock;
    struct Writer {
        Hmc2Header header;
        fs::path path;
        std::optional<Hmc2Record> previous;
    };
    std::map<fs::path, Writer> writers;
    Bytes raw, compressed, frame;
    std::set<fs::path> syncedDirectories;
    std::function<void()> afterFrameHeader;
    explicit Impl(fs::path p) : root(std::move(p)) {
        raw.reserve(256 * 1024);
        compressed.reserve(256 * 1024);
        mkdirs(root, syncedDirectories);
        int error = 0;
        lock = acquireFileLock(root / ".lock", error);
        check(lock != noLock, "root lock unavailable path=" + root.string() + " error=" + std::to_string(error));
        try {
            sync(root, true);
        } catch (...) {
            releaseFileLock(lock);
            throw;
        }
        sLog_Data("Hmc2Store: writer lock acquired root=" << root.string());
    }
    ~Impl() {
        releaseFileLock(lock);
    }
};
Hmc2Store::Hmc2Store(fs::path root) : impl_(std::make_unique<Impl>(std::move(root))) {}
Hmc2Store::~Hmc2Store() = default;
fs::path Hmc2Store::filePath(const fs::path &root, const Hmc2Header &h, int64_t ms, uint32_t gen) {
    validate(h);
    return root / h.symbol / (h.layer + "-" + std::to_string(h.tfMs)) /
           (dayName(ms) + (gen ? ".g" + std::to_string(gen) : "") + ".hmc2");
}
uint64_t Hmc2Store::configHash(const Hmc2Header &h, double low, double high) {
    auto copy = h;
    copy.configHash = 0;
    auto b = headerBody(copy);
    put(b, low);
    put(b, high);
    uint64_t hash = 14695981039346656037ull;
    for (auto v : b) {
        hash ^= v;
        hash *= 1099511628211ull;
    }
    return hash;
}
void Hmc2Store::append(const Hmc2Record &r) {
    validate(r.header);
    check(r.bucketStartMs >= kHmc2MinMs && r.bucketStartMs < kHmc2EndMs, "bucket outside UTC years 2000-2200");
    check(r.observedMs > 0 && r.observedMs <= r.header.tfMs && r.bucketStartMs % r.header.tfMs == 0,
          "invalid bucket/observation");
    auto &i = *impl_;
    const auto base = filePath(i.root, r.header, r.bucketStartMs);
    auto found = i.writers.find(base);
    if (found == i.writers.end() || headerBody(found->second.header) != headerBody(r.header)) {
        mkdirs(base.parent_path(), i.syncedDirectories);
        auto candidates = files(base.parent_path(), dayName(r.bucketStartMs));
        fs::path path = base;
        bool reuse = false;
        if (!candidates.empty()) {
            const auto &latest = candidates.back();
            try {
                std::ifstream f(latest, std::ios::binary);
                uint64_t p;
                check(f.is_open(), "open " + latest.string());
                uint16_t schema;
                reuse = headerBody(readHeader(f, p, schema)) == headerBody(r.header) && schema == writeSchema(r.header);
            } catch (const Corrupt &e) {
                sLog_Warning("Hmc2Store: unusable header path=" << latest.string() << " error=" << e.what());
            }
            path = reuse ? latest : filePath(i.root, r.header, r.bucketStartMs, generation(latest) + 1);
        }
        if (reuse)
            scan(path, true, [](Hmc2Record &&) {});
        else {
            const auto bytes = encodeHeader(r.header);
            int error = 0;
            const bool created = writeNewFileExclusive(path, bytes, error);
            check(created, "exclusive-create header " + path.string() + " error=" + std::to_string(error));
            sync(path);
            sLog_Data("Hmc2Store: new generation path=" << path.string() << " config=" << r.header.configHash);
        }
        // A complete header may have survived a failed first creation before
        // its directory entry was synced. Cover that case on reuse as well.
        sync(path.parent_path(), true);
        if (i.writers.size() >= kMaxWriters && found == i.writers.end())
            i.writers.erase(i.writers.begin());
        found = i.writers.insert_or_assign(base, Impl::Writer{r.header, path, {}}).first;
    }
    auto &w = found->second;
    check(std::isfinite(r.midOpen) && std::isfinite(r.midClose) && std::isfinite(r.midMin) && std::isfinite(r.midMax),
          "nonfinite mid metadata");
    validateEntries(r.entries);
    // Copy before writing so allocation failure cannot leave a committed record
    // with stale cache state. Only a successful durable append advances the base.
    auto next = r;
    const bool delta = w.previous && w.previous->bucketStartMs == r.bucketStartMs - r.header.tfMs &&
                       w.previous->bucketStartMs / kKeyframeMs == r.bucketStartMs / kKeyframeMs;
    encodeRecord(r, i.raw, delta ? &*w.previous : nullptr);
    if (r.header.tfMs == 3'600'000) {
        bool missing = false;
        decodeRecord(r.header, kHourSchema, i.raw, nullptr, missing);
    }
    i.compressed.resize(ZSTD_compressBound(i.raw.size()));
    const auto n = ZSTD_compress(i.compressed.data(), i.compressed.size(), i.raw.data(), i.raw.size(), 3);
    check(!ZSTD_isError(n), "zstd compression failed");
    i.compressed.resize(n);
    i.frame.clear();
    put(i.frame, kHmc2RecordMagic);
    put(i.frame, static_cast<uint32_t>(n));
    put(i.frame, static_cast<uint32_t>(i.raw.size()));
    put(i.frame, hmcol::crc32(i.compressed.data(), n));
    std::error_code sizeError;
    const auto offset = fs::file_size(w.path, sizeError);
    if (sizeError) {
        const auto path = w.path;
        i.writers.erase(found);
        fail("pre-append file size " + path.string() + " error=" + sizeError.message());
    }
    try {
        std::ofstream f(w.path, std::ios::binary | std::ios::app);
        f.write(reinterpret_cast<const char *>(i.frame.data()), static_cast<std::streamsize>(i.frame.size()));
        if (auto hook = std::exchange(i.afterFrameHeader, {})) {
            f.flush();
            check(static_cast<bool>(f), "append header " + w.path.string());
            hook();
        }
        f.write(reinterpret_cast<const char *>(i.compressed.data()), static_cast<std::streamsize>(n));
        f.flush();
        check(static_cast<bool>(f), "append " + w.path.string());
        f.close();
        check(static_cast<bool>(f), "close " + w.path.string());
        sync(w.path);
        w.previous = std::move(next);
    } catch (...) {
        const auto path = w.path;
        i.writers.erase(found);
        std::error_code rollbackError;
        fs::resize_file(path, offset, rollbackError);
        if (rollbackError) {
            sLog_Error("Hmc2Store: append rollback failed path=" << path.string() << " offset=" << offset
                                                                 << " error=" << rollbackError.message());
        } else {
            try {
                sync(path);
            } catch (const std::exception &e) {
                sLog_Error("Hmc2Store: append rollback sync failed path=" << path.string() << " error=" << e.what());
            }
        }
        throw;
    }
}
void Hmc2Store::afterFrameHeaderForTest(std::function<void()> hook) {
    impl_->afterFrameHeader = std::move(hook);
}
std::vector<Hmc2Record> Hmc2Store::readRange(const fs::path &root, const std::string &symbol, const std::string &layer,
                                             int64_t tf, int64_t start, int64_t end) {
    check(safeName(symbol) && safeName(layer) && tf > 0, "invalid query");
    std::map<int64_t, Hmc2Record> records;
    start = std::max(start, kHmc2MinMs);
    end = std::min(end, kHmc2EndMs);
    if (start >= end)
        return {};
    const auto firstDay = dayName(start), lastDay = dayName(end - 1);
    for (const auto &path : files(root / symbol / (layer + "-" + std::to_string(tf)), {}, true)) {
        const auto day = path.filename().string().substr(0, 10);
        if (day < firstDay || day > lastDay)
            continue;
        try {
            scan(
                path, false,
                [&](Hmc2Record &&r) {
                    if (r.header.symbol == symbol && r.header.layer == layer && r.header.tfMs == tf &&
                        r.bucketStartMs >= start && r.bucketStartMs < end)
                        records.insert_or_assign(r.bucketStartMs, std::move(r));
                },
                start - start % kKeyframeMs, end);
        } catch (const std::exception &e) {
            sLog_Warning("Hmc2Store: skipped file path=" << path.string() << " error=" << e.what());
        }
    }
    std::vector<Hmc2Record> out;
    out.reserve(records.size());
    for (auto &[_, r] : records)
        out.push_back(std::move(r));
    return out;
}
} // namespace recording
