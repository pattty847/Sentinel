#include "LevelLogFormat.hpp"
#include "Crc32.hpp"
#include "SentinelLogging.hpp"
#include <algorithm>
#include <array>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace sentinel::levelstore {
namespace log_detail {
constexpr std::array<uint8_t, 4> kMagic{'L', 'C', 'L', 'G'};
void check(bool ok, const std::string &why) {
    if (!ok) throw std::runtime_error("LevelLog: " + why);
}
void validateProduct(const std::string &s) {
    check(!s.empty() && s.size() <= 256 && s != "." && s != "..", "invalid product");
    for (unsigned char c : s)
        check((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.', "invalid product");
}
void validateRecord(const LevelLogRecord &r) {
    const auto &b = r.binding;
    validateProduct(b.symbol);
    check(b.floorMs >= 0 && b.floorMs % 60'000 == 0 && b.ceilingMs > b.floorMs &&
              b.priceDigits >= -1 && b.priceDigits <= 18,
          "invalid binding");
    const auto t = r.minute();
    check(t >= b.floorMs && t < b.ceilingMs && t % 60'000 == 0, "minute outside binding");
    if (std::holds_alternative<BookImage>(r.value)) check(t == b.floorMs, "image not at binding floor");
}
void advance(ScanResult &s, const LevelLogRecord &r) {
    validateRecord(r);
    if (std::holds_alternative<BookImage>(r.value)) {
        check(!s.binding || r.binding.floorMs >= s.binding->ceilingMs, "overlapping day-start image");
        s.binding = r.binding;
        s.lastMinute.reset();
    } else {
        check(s.binding && *s.binding == r.binding, "block without matching day-start image");
        check(!s.lastMinute || r.minute() > *s.lastMinute, "unordered minute");
        s.lastMinute = r.minute();
    }
    s.last = LevelLogTail{r.binding, r.minute(), std::holds_alternative<BookImage>(r.value)};
}
ByteWriter payload(const LevelLogRecord &r) {
    ByteWriter w;
    w.str(r.binding.symbol);
    w.svarint(r.binding.floorMs);
    w.svarint(r.binding.ceilingMs);
    w.svarint(r.binding.priceDigits);
    if (const auto *i = std::get_if<BookImage>(&r.value)) encodeImage(w, *i);
    else encodeBlock(w, std::get<MinuteBlock>(r.value));
    check(w.bytes.size() <= kMaxPayload, "payload too large");
    return w;
}
ByteWriter header(uint8_t type, size_t length) {
    ByteWriter w;
    for (auto c : kMagic) w.u8(c);
    w.u8(1); // format version
    w.u8(type);
    w.u8(0);
    w.u8(0);
    w.fixed32(static_cast<uint32_t>(length));
    return w;
}
std::vector<fs::path> paths(const fs::path &root, const std::string &product) {
    validateProduct(product);
    std::vector<fs::path> result;
    const auto dir = root / product;
    if (!fs::exists(dir)) return result;
    for (const auto &entry : fs::directory_iterator(dir)) {
        const auto name = entry.path().filename().string();
        if (name.starts_with("._") || entry.path().extension() != ".lcl") continue;
        check(name.size() == 24 && std::all_of(name.begin(), name.begin() + 20,
                  [](char c) { return c >= '0' && c <= '9'; }) && entry.is_regular_file(),
              "invalid hour path=" + entry.path().string());
        result.push_back(entry.path());
    }
    std::sort(result.begin(), result.end());
    return result;
}
namespace {
struct ReadIo : std::runtime_error { using std::runtime_error::runtime_error; };
std::vector<uint8_t> readAt(std::ifstream &f, uint64_t offset, size_t n) {
    std::vector<uint8_t> b(n);
    f.clear();
    f.seekg(static_cast<std::streamoff>(offset));
    f.read(reinterpret_cast<char *>(b.data()), static_cast<std::streamsize>(n));
    if (!f) throw ReadIo("read failed offset=" + std::to_string(offset));
    return b;
}
struct Frame {
    bool incomplete = false;
    uint64_t end = 0;
    LevelLogRecord record;
};
Frame frame(std::ifstream &f, uint64_t pos, uint64_t end) {
    const auto h = readAt(f, pos, static_cast<size_t>(std::min<uint64_t>(kHeader, end - pos)));
    check(std::equal(h.begin(), h.begin() + std::min<size_t>(4, h.size()), kMagic.begin()), "bad magic");
    if (h.size() < kHeader) return {true, end, {}};
    ByteReader hr(h);
    (void)hr.fixed32();
    check(hr.u8() == 1, "unsupported version");
    const auto type = hr.u8();
    check(type == 1 || type == 2, "bad record type");
    check(hr.u8() == 0 && hr.u8() == 0, "reserved header bits");
    const auto n = hr.fixed32();
    check(n > 0 && n <= kMaxPayload, "invalid payload length");
    if (n + uint64_t(kHeader) + 4 > end - pos) return {true, end, {}};
    const auto p = readAt(f, pos + kHeader, n);
    const auto crcBytes = readAt(f, pos + kHeader + n, 4);
    ByteReader cr(crcBytes);
    const auto crc = sentinel::crc32Update(sentinel::crc32(h.data(), h.size()), p.data(), p.size());
    check(cr.fixed32() == crc, "checksum mismatch");
    ByteReader r(p);
    LevelLogRecord record;
    record.binding.symbol = r.str(256);
    record.binding.floorMs = r.svarint();
    record.binding.ceilingMs = r.svarint();
    const auto digits = r.svarint();
    check(digits >= -1 && digits <= 18, "invalid price digits");
    record.binding.priceDigits = static_cast<int>(digits);
    if (type == 1) record.value = decodeImage(r);
    else record.value = decodeBlock(r);
    check(r.done(), "trailing payload bytes");
    validateRecord(record);
    return {false, pos + kHeader + n + 4, std::move(record)};
}
// Scan bounded chunks for fully validated candidates, never trust a damaged length.
// This is classification only: no candidate after damage is ever returned to a caller.
bool validAfter(std::ifstream &f, uint64_t pos, uint64_t end) {
    while (end - pos >= 4) {
        const auto bytes = readAt(f, pos, static_cast<size_t>(std::min<uint64_t>(64 * 1024, end - pos)));
        for (size_t i = 0; i + 4 <= bytes.size(); ++i) {
            if (!std::equal(kMagic.begin(), kMagic.end(), bytes.begin() + i)) continue;
            try {
                if (!frame(f, pos + i, end).incomplete) return true;
            } catch (const ReadIo &) {
                throw;
            } catch (const std::runtime_error &) {
                // A magic-like payload sequence alone is not a valid candidate.
            }
        }
        pos += bytes.size() - 3;
    }
    return false;
}
} // namespace
ScanResult scan(const fs::path &root, const std::string &product,
                const std::function<void(const LevelLogRecord &, const Position &)> &sink) {
    ScanResult state;
    for (const auto &path : paths(root, product)) {
        std::ifstream f(path, std::ios::binary);
        check(f.is_open(), "open path=" + path.string());
        const auto end = fs::file_size(path);
        if (end && state.tail)
            check(false, "path=" + state.tail->path.string() + " offset=" + std::to_string(state.tail->offset) +
                             " incomplete frame followed by later hour data path=" + path.string());
        uint64_t pos = 0;
        try {
            while (pos < end) {
                auto next = frame(f, pos, end);
                if (next.incomplete) {
                    check(!validAfter(f, pos + 1, end), "incomplete frame followed by valid data");
                    state.tail = Position{path, pos};
                    break;
                }
                check(next.record.binding.symbol == product, "product mismatch");
                check(LevelLogReader::hourPath(root, product, next.record.minute()) == path, "hour mismatch");
                advance(state, next.record);
                if (sink) sink(next.record, {path, pos});
                pos = next.end;
            }
        } catch (const std::exception &e) {
            const auto why = "path=" + path.string() + " offset=" + std::to_string(pos) + " reason=" + e.what();
            sLog_Error("LevelLog: corrupt " << why);
            throw std::runtime_error(why);
        }
    }
    return state;
}
// A validated predecessor proves the next byte is disposable without reading it.
// If a gap prevents this proof, decode until a valid record establishes the cut;
// damage in that ambiguous region fails closed rather than guessing its timestamp.
CutPlan planCut(const fs::path &root, const std::string &product, int64_t minute, CutImageAtBoundary image) {
    CutPlan plan;
    const auto cutPath = LevelLogReader::hourPath(root, product, minute);
    bool reached = false, keptImage = false;
    for (const auto &path : paths(root, product)) {
        const auto end = fs::file_size(path);
        if (reached || path > cutPath ||
            (path == cutPath && minute % kHour == 0 && image == CutImageAtBoundary::Remove) || end == 0) {
            plan.files.push_back({{path, 0}, end});
            continue;
        }
        std::ifstream f(path, std::ios::binary);
        check(f.is_open(), "open path=" + path.string());
        uint64_t pos = 0;
        try {
            while (pos < end) {
                const auto next = frame(f, pos, end);
                check(!next.incomplete, "incomplete frame before cut boundary");
                check(next.record.binding.symbol == product, "product mismatch");
                check(LevelLogReader::hourPath(root, product, next.record.minute()) == path, "hour mismatch");
                const bool keepImage = image == CutImageAtBoundary::Keep &&
                    next.record.minute() == minute && std::holds_alternative<BookImage>(next.record.value);
                if (next.record.minute() >= minute && !keepImage) {
                    reached = true;
                    break;
                }
                advance(plan.retained, next.record);
                pos = next.end;
                keptImage = keptImage || keepImage;
                if (keepImage || (image == CutImageAtBoundary::Remove &&
                    std::holds_alternative<MinuteBlock>(next.record.value) && next.record.minute() == minute - 60'000)) {
                    reached = true;
                    break;
                }
            }
            if (pos < end) plan.files.push_back({{path, pos}, end});
        } catch (const std::exception &e) {
            const auto why = "path=" + path.string() + " offset=" + std::to_string(pos) + " reason=" + e.what();
            sLog_Error("LevelLog: corrupt retained prefix " << why);
            throw std::runtime_error(why);
        }
    }
    check(image != CutImageAtBoundary::Keep || keptImage,
          "path=" + cutPath.string() + " offset=0 required image at cut is absent");
    return plan;
}
} // namespace log_detail
int64_t LevelLogRecord::minute() const {
    if (const auto *i = std::get_if<BookImage>(&value)) return i->atMs;
    return std::get<MinuteBlock>(value).bucketStartMs;
}
LevelLogReader::LevelLogReader(std::filesystem::path root, std::string product)
    : root_(std::move(root)), product_(std::move(product)) { log_detail::validateProduct(product_); }
std::filesystem::path LevelLogReader::hourPath(const std::filesystem::path &root,
                                               const std::string &product, int64_t minute) {
    log_detail::validateProduct(product);
    log_detail::check(minute >= 0, "negative timestamp");
    std::ostringstream name;
    name << std::setw(20) << std::setfill('0') << minute / log_detail::kHour << ".lcl";
    return root / product / name.str();
}
void LevelLogReader::read(const std::function<void(const LevelLogRecord &)> &sink) const {
    log_detail::scan(root_, product_, [&](const auto &r, const auto &) { sink(r); });
}
std::optional<LevelLogRecord> LevelLogReader::imageForHour(int64_t hourMs) const {
    log_detail::check(hourMs >= 0 && hourMs % log_detail::kHour == 0, "unaligned hour");
    std::optional<LevelLogRecord> image;
    read([&](const auto &r) {
        if (std::holds_alternative<BookImage>(r.value) && r.minute() / log_detail::kHour <= hourMs / log_detail::kHour &&
            r.binding.ceilingMs > hourMs) image = r;
    });
    return image;
}
} // namespace sentinel::levelstore
