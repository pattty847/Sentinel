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

namespace recording {
namespace {
namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;
using namespace sentinel::persistence;
constexpr uint16_t kSchema = 2;
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
    put(b, kSchema);
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
Hmc2Header readHeader(std::ifstream &f, uint64_t &pos) {
    Bytes b;
    if (!readAt(f, 0, b, kPrefix))
        throw Corrupt("incomplete header");
    Cursor c{b};
    if (c.get<uint32_t>() != kHmc2Magic || c.get<uint16_t>() != kSchema)
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
void encodeRecord(const Hmc2Record &r, Bytes &b) {
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
    check(r.entries.size() <= (kHmc2MaxRawLen - 84) / 5, "too many entries");
    put(b, static_cast<uint32_t>(r.entries.size()));
    int64_t previous = 0;
    bool previousAsk = false, first = true;
    for (const auto &e : r.entries) {
        check(first || e.row > previous || (e.row == previous && !previousAsk && e.isAsk),
              "entries must be unique and sorted by row/side");
        check(first || previous >= 0 || e.row <= INT64_MAX + previous, "row delta overflow");
        check(e.twapCode <= kMaxCode && e.peakCode <= kMaxCode, "code contains side bit");
        putVarint(b, zigzag(e.row - previous));
        put(b, withSide(e.twapCode, e.isAsk));
        put(b, e.peakCode);
        previous = e.row;
        previousAsk = e.isAsk;
        first = false;
    }
    check(b.size() <= kHmc2MaxRawLen, "raw record exceeds 16 MiB");
}
Hmc2Record decodeRecord(const Hmc2Header &h, const Bytes &b) {
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
    if (r.observedMs > h.tfMs || r.bucketStartMs % h.tfMs != 0 || !std::isfinite(r.midOpen) ||
        !std::isfinite(r.midClose) || !std::isfinite(r.midMin) || !std::isfinite(r.midMax))
        throw Corrupt("record semantics");
    const auto n = c.get<uint32_t>();
    if (n > (b.size() - c.p) / 5)
        throw Corrupt("entry count");
    r.entries.reserve(n);
    int64_t prev = 0;
    bool prevAsk = false;
    for (uint32_t i = 0; i < n; ++i) {
        uint64_t v;
        if (!getVarint(b.data(), b.size(), c.p, v))
            throw Corrupt("row varint");
        const int64_t delta = unzigzag(v);
        if (i && (delta < 0 || prev > INT64_MAX - delta))
            throw Corrupt("row delta");
        const int64_t row = prev + delta;
        const auto twap = c.get<uint16_t>(), peak = c.get<uint16_t>();
        const bool ask = isAsk(twap);
        if (peak > kMaxCode || (i && row == prev && (prevAsk || !ask)))
            throw Corrupt("entry order/code");
        r.entries.push_back({row, ask, static_cast<uint16_t>(twap & kMaxCode), peak});
        prev = row;
        prevAsk = ask;
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
std::vector<fs::path> files(const fs::path &dir, const std::string &day = {}, bool tolerant = false) {
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
uint64_t nextMagic(std::ifstream &f, uint64_t pos, uint64_t end, Bytes &scratch) {
    static constexpr std::array<uint8_t, 4> magic{'H', 'C', 'R', '2'};
    while (end - pos >= magic.size()) {
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
template <class Sink> void scan(const fs::path &path, bool repair, Sink sink) {
    std::ifstream f(path, std::ios::binary);
    check(f.is_open(), "open " + path.string());
    uint64_t pos = 0;
    const auto h = readHeader(f, pos);
    std::error_code sizeError;
    const auto end = fs::file_size(path, sizeError);
    check(!sizeError, "file size " + path.string() + " error=" + sizeError.message());
    Bytes framing, compressed, raw, scratch;
    uint64_t tail = end;
    bool damaged = false;
    while (pos < end) {
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
            pos = nextMagic(f, pos + 1, end, scratch);
            continue;
        }
        const auto clen = c.get<uint32_t>(), rlen = c.get<uint32_t>(), crc = c.get<uint32_t>();
        if (rlen < 84 || rlen > kHmc2MaxRawLen || clen == 0 || clen > ZSTD_compressBound(kHmc2MaxRawLen)) {
            damaged = true;
            pos = nextMagic(f, pos + 1, end, scratch);
            continue;
        }
        if (clen > end - pos - 16) {
            if (!damaged)
                tail = std::min(tail, pos);
            damaged = true;
            pos = nextMagic(f, pos + 1, end, scratch);
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
        if (good) {
            std::optional<Hmc2Record> record;
            try {
                record = decodeRecord(h, raw);
            } catch (const Corrupt &e) {
                sLog_Warning("Hmc2Store: invalid payload path=" << path.string() << " error=" << e.what());
            }
            if (record) {
                if (damaged)
                    sLog_Warning("Hmc2Store: skipped interior corruption path=" << path.string() << " resume=" << pos);
                sink(std::move(*record));
                pos += 16 + clen;
                tail = end;
                damaged = false;
                continue;
            }
        }
        damaged = true;
        pos = nextMagic(f, pos + 1, end, scratch);
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

struct Hmc2Store::Impl {
    fs::path root;
    LockHandle lock = noLock;
    struct Writer {
        Hmc2Header header;
        fs::path path;
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
                reuse = headerBody(readHeader(f, p)) == headerBody(r.header);
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
        found = i.writers.insert_or_assign(base, Impl::Writer{r.header, path}).first;
    }
    auto &w = found->second;
    check(std::isfinite(r.midOpen) && std::isfinite(r.midClose) && std::isfinite(r.midMin) && std::isfinite(r.midMax),
          "nonfinite mid metadata");
    encodeRecord(r, i.raw);
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
            scan(path, false, [&](Hmc2Record &&r) {
                if (r.header.symbol == symbol && r.header.layer == layer && r.header.tfMs == tf &&
                    r.bucketStartMs >= start && r.bucketStartMs < end)
                    records.insert_or_assign(r.bucketStartMs, std::move(r));
            });
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
