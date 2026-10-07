#include "AnchorStore.hpp"
#include "servermodel/HmcolFormat.hpp"
#include "servermodel/PersistenceIo.hpp"
#include <QDateTime>
#include <QTimeZone>
#include <QSaveFile>
#include <zstd.h>
#include <algorithm>
#include <cmath>
#include <chrono>
#ifdef _WIN32
#include <io.h>
#endif
#include <fstream>

namespace sentinel::roller {
namespace {
using nlohmann::json;
constexpr int64_t Day = 86'400'000, Quarter = 900'000;
constexpr std::string_view Magic = "SANCHR01";
void require(bool ok, const char* reason) {
    if (!ok) throw std::runtime_error(std::string("anchor: ") + reason);
}
void validate(const ReplayAnchor& a) {
    const auto& i = a.identity;
    capture::validateSymbol(i.product);
    require(i.dayMs >= 946684800000LL && i.dayMs < 7289654400000LL && i.dayMs % Day == 0,
            "invalid day");
    require(i.fromMs >= i.dayMs && i.toMs <= i.dayMs + Day && i.fromMs < i.toMs &&
            i.fromMs % 60'000 == 0 && i.toMs % 60'000 == 0, "invalid range");
    require(a.boundaryMs >= i.dayMs && a.boundaryMs < i.dayMs + Day && a.boundaryMs % Quarter == 0,
            "invalid boundary");
    for (const auto* p : {&a.pos, &a.gridSnapshot})
        require(p->product == i.product && !p->run.empty() && p->run.size() <= 256, "position identity");
    require(std::isfinite(a.referenceMid) && a.referenceMid > 0, "invalid grid reference");
    require(a.metadata.is_object() && a.metadata.at("product_id") == i.product &&
            a.metadata.at("quote_increment").is_string() && a.metadata.at("base_increment").is_string(),
            "grid metadata");
    size_t size = 0;
    for (const auto* section : {&a.feed, &a.book, &a.recorder}) {
        require(section->version == 1, "unsupported section version");
        require(section->bytes.size() <= AnchorStore::MaxBytes - size, "sections too large");
        size += section->bytes.size();
    }
}
void put(std::vector<uint8_t>& b, uint32_t n) {
    for (unsigned i = 0; i < 4; ++i) b.push_back(uint8_t(n >> (i * 8)));
}
uint32_t get(const std::vector<uint8_t>& b, size_t p) {
    require(p <= b.size() && b.size() - p >= 4, "truncated header");
    uint32_t n = 0;
    for (unsigned i = 0; i < 4; ++i) n |= uint32_t(b[p+i]) << (i*8);
    return n;
}
json section(const AnchorSection& s) { return {{"version",s.version},{"bytes",json::binary(s.bytes)}}; }
AnchorSection section(const json& j) {
    require(j.at("version") == 1 && j.at("bytes").is_binary(), "section schema/version");
    return {1, j.at("bytes").get_binary()};
}
}
std::filesystem::path AnchorStore::path(const std::filesystem::path& root, const ReplayAnchor& a) {
    validate(a);
    const auto dt = QDateTime::fromMSecsSinceEpoch(a.boundaryMs, QTimeZone::UTC);
    return root/a.identity.product/"anchors"/dt.toString("yyyy-MM-dd").toStdString()/
        (dt.toString("HHmm").toStdString() + ".anchor");
}
std::vector<uint8_t> AnchorStore::encode(const ReplayAnchor& a) {
    validate(a);
    const auto& i = a.identity;
    const json j = {{"product",i.product},{"day",i.dayMs},{"from",i.fromMs},{"to",i.toMs},
        {"configHash",i.configHash},{"boundary",a.boundaryMs},{"pos",a.pos},
        {"gridSnapshot",a.gridSnapshot},{"referenceMid",a.referenceMid},{"metadata",a.metadata},
        {"feed",section(a.feed)},{"book",section(a.book)},{"recorder",section(a.recorder)}};
    const auto raw = json::to_cbor(j);
    require(raw.size() <= MaxBytes, "payload too large");
    std::vector<uint8_t> compressed(ZSTD_compressBound(raw.size()));
    const auto n = ZSTD_compress(compressed.data(), compressed.size(), raw.data(), raw.size(), 3);
    require(!ZSTD_isError(n) && n <= MaxBytes, "compression failed/oversize");
    std::vector<uint8_t> out(Magic.begin(), Magic.end());
    put(out, Version); put(out, uint32_t(raw.size())); put(out, uint32_t(n));
    out.insert(out.end(), compressed.begin(), compressed.begin() + n);
    put(out, hmcol::crc32(out.data(), out.size()));
    return out;
}
ReplayAnchor AnchorStore::decode(const std::vector<uint8_t>& bytes) {
    require(bytes.size() >= 24 && bytes.size() <= MaxBytes + 24, "file size");
    require(std::equal(Magic.begin(), Magic.end(), bytes.begin()) && get(bytes,8) == Version, "magic/version");
    const auto rawSize = get(bytes,12), packedSize = get(bytes,16);
    require(rawSize && rawSize <= MaxBytes && packedSize == bytes.size()-24, "payload length");
    require(get(bytes,bytes.size()-4) == hmcol::crc32(bytes.data(),bytes.size()-4), "CRC mismatch");
    require(ZSTD_findFrameCompressedSize(bytes.data()+20, packedSize) == packedSize, "compression framing");
    std::vector<uint8_t> raw(rawSize);
    require(ZSTD_decompress(raw.data(),raw.size(),bytes.data()+20,packedSize) == rawSize, "decompression failed");
    const auto j = json::from_cbor(raw);
    ReplayAnchor a;
    a.identity = {j.at("product"),j.at("day"),j.at("from"),j.at("to"),j.at("configHash")};
    a.boundaryMs = j.at("boundary"); a.pos = j.at("pos").get<JournalPos>();
    a.gridSnapshot = j.at("gridSnapshot").get<JournalPos>();
    a.referenceMid = j.at("referenceMid"); a.metadata = j.at("metadata");
    a.feed = section(j.at("feed")); a.book = section(j.at("book")); a.recorder = section(j.at("recorder"));
    validate(a);
    return a;
}
AnchorStore::WriteStats AnchorStore::write(const std::filesystem::path& root, const ReplayAnchor& a) {
    const auto started = std::chrono::steady_clock::now();
    const auto bytes = encode(a); // validate before even creating directories
    const auto encoded = std::chrono::steady_clock::now();
    const auto file = path(root,a);
    std::vector<std::filesystem::path> missing;
    auto parent = std::filesystem::absolute(file.parent_path());
    while (!std::filesystem::exists(parent)) { missing.push_back(parent); parent = parent.parent_path(); }
    std::filesystem::create_directories(file.parent_path());
    int error = 0;
    for (auto it = missing.rbegin(); it != missing.rend(); ++it)
        require(persistence::syncDirectory(*it,error) && persistence::syncDirectory(it->parent_path(),error),
                "directory fsync failed");
    QSaveFile out(QString::fromStdString(file.string()));
    out.setDirectWriteFallback(false);
    require(out.open(QIODevice::WriteOnly) &&
        out.write(reinterpret_cast<const char*>(bytes.data()),qint64(bytes.size())) == qint64(bytes.size()) && out.flush(),
        "temporary write failed");
#ifndef _WIN32
    require(persistence::syncFileDescriptor(out.handle(),error), "temporary fsync failed");
#else
    require(::_commit(out.handle()) == 0, "temporary fsync failed");
#endif
    require(out.commit(), "rename failed");
    require(persistence::syncFilePath(file,error) && persistence::syncDirectory(file.parent_path(),error),
            "durable rename failed");
    const auto done = std::chrono::steady_clock::now();
    return {bytes.size(),std::chrono::duration<double,std::milli>(encoded-started).count(),
        std::chrono::duration<double,std::milli>(done-encoded).count()};
}
ReplayAnchor AnchorStore::load(const std::filesystem::path& file) {
        const auto size = std::filesystem::file_size(file);
        require(size <= MaxBytes + 24, "file too large");
        std::ifstream in(file,std::ios::binary);
        std::vector<uint8_t> bytes(size);
        require(bool(in.read(reinterpret_cast<char*>(bytes.data()),std::streamsize(size))), "file read failed");
        require(in.peek() == std::char_traits<char>::eof(), "file grew during read");
        return decode(bytes);
}
std::vector<std::filesystem::path> AnchorStore::candidates(const std::filesystem::path& root,
        const std::string& product, int64_t dayMs) {
    capture::validateSymbol(product);
    const auto day = QDateTime::fromMSecsSinceEpoch(dayMs,QTimeZone::UTC).toString("yyyy-MM-dd").toStdString();
    const auto dir = root/product/"anchors"/day;
    std::vector<std::filesystem::path> out;
    if (std::filesystem::is_symlink(dir) || !std::filesystem::is_directory(dir)) return out;
    for (const auto& f : std::filesystem::directory_iterator(dir))
        if (!f.is_symlink() && f.is_regular_file() && f.path().extension() == ".anchor") out.push_back(f.path());
    std::sort(out.rbegin(),out.rend());
    return out;
}
std::error_code AnchorStore::prune(const std::filesystem::path& root, const std::string& product, int64_t nowMs) {
    namespace fs = std::filesystem;
    capture::validateSymbol(product);
    std::error_code firstError;
    const auto remember = [&](std::error_code ec) { if (ec && !firstError) firstError = ec; };
    const auto status = [&](const fs::path& path) {
        std::error_code ec;
        auto s = fs::symlink_status(path,ec);
        if (ec != std::errc::no_such_file_or_directory) remember(ec);
        return s;
    };
    const auto dir = root/product/"anchors";
    if (!fs::is_directory(status(root/product)) || !fs::is_directory(status(dir))) return firstError;
    // Temp-file age uses wall time; historical journal rebuilds must also clean
    // abandoned QSaveFile files. Completed anchors retain journal-time policy.
    const auto staleBefore = fs::file_time_type::clock::now() - std::chrono::hours(1);
    std::error_code ec;
    auto days = fs::directory_iterator(dir,ec);
    remember(ec);
    for (; days != fs::directory_iterator{}; days.increment(ec), remember(ec)) {
        const auto d = days->path();
        if (!fs::is_directory(status(d))) continue; // includes symlink exclusion
        const auto dateText = QString::fromStdString(d.filename().string());
        const auto date = QDate::fromString(dateText,"yyyy-MM-dd");
        if (!date.isValid() || date.toString("yyyy-MM-dd") != dateText) continue;
        const auto day = QDateTime(date,QTime(0,0),QTimeZone::UTC).toMSecsSinceEpoch();
        auto files = fs::directory_iterator(d,ec);
        remember(ec);
        for (; files != fs::directory_iterator{}; files.increment(ec), remember(ec)) {
            const auto file = files->path();
            const auto name = file.filename().string();
            if (!fs::is_regular_file(status(file)) || name.size() < 11 || name.substr(4,7) != ".anchor") continue;
            const auto time = QTime::fromString(QString::fromStdString(name.substr(0,4)),"HHmm");
            if (!time.isValid() || time.minute() % 15) continue;
            if (name.size() == 11) {
                if (name == "0000.anchor" || day + time.msecsSinceStartOfDay() >= nowMs - 2*Day) continue;
            } else {
                if (name.size() <= 12 || name[11] != '.') continue;
                const auto modified = fs::last_write_time(file,ec);
                remember(ec);
                if (ec || modified >= staleBefore) continue;
            }
            fs::remove(file,ec);
            remember(ec);
        }
    }
    return firstError;
}
std::optional<ReplayAnchor> AnchorStore::read(const std::filesystem::path& file, const AnchorIdentity& expected,
        int64_t maxBoundaryMs, std::optional<JournalPos> ceiling, std::string* rejection) {
    if (rejection) rejection->clear();
    try {
        auto a = load(file);
        require(a.identity == expected, "policy/range identity mismatch");
        require(a.boundaryMs <= maxBoundaryMs, "boundary ahead of target");
        if (ceiling) require(a.pos.product == ceiling->product && a.pos.run == ceiling->run &&
            std::pair(a.pos.block,a.pos.record) <= std::pair(ceiling->block,ceiling->record), "position ahead of ceiling");
        return a;
    } catch (const std::exception& e) {
        if (rejection) *rejection = e.what();
        return {};
    }
}
} // namespace sentinel::roller
