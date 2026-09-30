#include "RawCapture.hpp"
#include "SentinelLogging.hpp"
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QStorageInfo>
#include <QUuid>
#include <QTimeZone>
#include "servermodel/HmcolFormat.hpp"
#include "servermodel/PersistenceIo.hpp"
#include <zstd.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <limits>
#include <stdexcept>
#ifdef _WIN32
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace sentinel::capture {
namespace {
constexpr std::string_view Magic = "RAWL2\r\n\1";
constexpr uint32_t MaxHeaderBytes = 1024 * 1024;
constexpr uint32_t BlockHeaderBytes = 48;
constexpr uint32_t IndexEntryBytes = 44;
constexpr int64_t HourNs = 3600LL * 1000000000LL;

[[noreturn]] void fail(const std::string& message) { throw std::runtime_error("RAWL2: " + message); }
uint32_t crc(std::string_view bytes) {
    return hmcol::crc32(bytes.data(), bytes.size());
}
void put32(std::string& bytes, uint32_t value) {
    for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<char>(value >> (i * 8)));
}
void put64(std::string& bytes, uint64_t value) {
    for (int i = 0; i < 8; ++i) bytes.push_back(static_cast<char>(value >> (i * 8)));
}
uint64_t get(std::string_view bytes, size_t& pos, size_t width) {
    if (pos > bytes.size() || width > bytes.size() - pos) fail("truncated field");
    uint64_t value = 0;
    for (size_t i = 0; i < width; ++i) value |= uint64_t(static_cast<unsigned char>(bytes[pos++])) << (i * 8);
    return value;
}
std::string read(QFile& file, qint64 count) {
    const auto bytes = file.read(count);
    if (bytes.size() != count) fail("read failure: " + file.errorString().toStdString());
    return bytes.toStdString();
}
void syncFd(int fd) {
#ifdef _WIN32
    if (::_commit(fd) != 0) fail("commit failed: " + std::string(std::strerror(errno)));
#else
    int error = 0;
    if (!sentinel::persistence::syncFileDescriptor(fd, error))
        fail("durable sync failed: " + std::string(std::strerror(error)));
#endif
}
void syncDirectory(const QString& directory) {
#ifndef _WIN32
    const int fd = ::open(QFile::encodeName(directory).constData(), O_RDONLY | O_DIRECTORY);
    if (fd < 0) fail("cannot open directory for fsync: " + directory.toStdString());
    try { syncFd(fd); } catch (...) { ::close(fd); throw; }
    ::close(fd);
#else
    (void)directory;
#endif
}
void makeDirectories(const QString& directory) {
    std::vector<QString> created;
    QString parent = directory;
    while (!QFileInfo::exists(parent)) {
        created.push_back(parent);
        const auto next = QFileInfo(parent).absolutePath();
        if (next == parent) fail("no existing ancestor");
        parent = next;
    }
    if (!QDir().mkpath(directory)) fail("cannot create " + directory.toStdString());
    for (const auto& path : created) {
        syncDirectory(path);
        syncDirectory(QFileInfo(path).absolutePath());
    }
}
std::string encodeIndex(const std::vector<BlockIndex>& index) {
    std::string bytes;
    put32(bytes, static_cast<uint32_t>(index.size()));
    for (const auto& entry : index) {
        put64(bytes, entry.offset);
        put64(bytes, entry.firstSystemNs);
        put64(bytes, entry.lastSystemNs);
        put64(bytes, entry.ordinal);
        put32(bytes, entry.records);
        put32(bytes, entry.compressedBytes);
        put32(bytes, entry.rawBytes);
    }
    return bytes;
}
// An unframed suffix is recoverable only if no valid block/index framing
// follows it. Scan in bounded chunks, so interior corruption cannot masquerade
// as a torn tail just because the damaged block's magic was overwritten.
bool hasFollowingFraming(QFile& file, qint64 start, qint64 end) {
    for (auto offset = start; offset < end; offset += 65533) {
        if (!file.seek(offset)) fail("tail scan seek failed");
        const auto bytes = read(file, std::min<qint64>(65536, end - offset));
        for (size_t i = 0; i + 4 <= bytes.size(); ++i) {
            const auto magic = std::string_view(bytes).substr(i, 4);
            const auto candidate = offset + qint64(i);
            if (magic == "BLK1" && end - candidate >= BlockHeaderBytes) {
                if (!file.seek(candidate)) fail("tail scan seek failed");
                const auto header = read(file, BlockHeaderBytes);
                size_t pos = 44;
                if (crc(std::string_view(header).substr(0, 44)) == get(header, pos, 4)) return true;
            } else if (magic == "IDX1" && end - candidate >= 12) {
                if (!file.seek(candidate + 4)) fail("tail scan seek failed");
                const auto length = read(file, 4);
                size_t pos = 0;
                const auto size = get(length, pos, 4);
                if (size < 4 || size > 4 + uint64_t(MaxIndexEntries) * IndexEntryBytes ||
                    size + 4 > uint64_t(end - file.pos())) continue;
                const auto body = read(file, size);
                const auto checksum = read(file, 4); pos = 0;
                if (crc(body) == get(checksum, pos, 4)) return true;
            }
        }
    }
    return false;
}
nlohmann::json headerFrom(QFile& file) {
    if (file.size() < 16) fail("incomplete file header");
    const auto prefix = read(file, 16);
    if (std::string_view(prefix).substr(0, 8) != Magic) fail("bad magic/version");
    size_t pos = 8;
    const auto size = get(prefix, pos, 4);
    const auto checksum = get(prefix, pos, 4);
    if (size == 0 || size > MaxHeaderBytes || size > uint64_t(file.size() - file.pos())) fail("bad header length");
    const auto data = read(file, size);
    if (crc(data) != checksum) fail("header CRC mismatch");
    auto header = nlohmann::json::parse(data);
    if (header.at("format_version") != 1) fail("unsupported version");
    for (const char* field : {"segment", "first_block_ordinal", "run_started_system_ns", "opened_system_ns", "opened_steady_ns"}) {
        const auto& value = header.at(field);
        if (!value.is_number_integer() || (!value.is_number_unsigned() && value.get<int64_t>() < 0))
            fail(std::string("invalid header field: ") + field);
        if (std::string_view(field).ends_with("_ns") && value.get<uint64_t>() > uint64_t(INT64_MAX))
            fail("header timestamp overflow");
    }
    const auto run = header.at("run_id").get<std::string>();
    if (run.empty() || run.size() > 128) fail("invalid run id");
    const auto& product = header.at("product_metadata");
    validateSymbol(product.at("product_id").get<std::string>());
    if (!product.at("quote_increment").is_string() || !product.at("base_increment").is_string())
        fail("increments must be strings");
    if (header.at("products") != nlohmann::json::array({product.at("product_id")})) fail("product metadata/subscription mismatch");
    return header;
}
} // namespace

struct Writer::CompressionState {
    std::unique_ptr<ZSTD_CCtx, decltype(&ZSTD_freeCCtx)> context{ZSTD_createCCtx(), &ZSTD_freeCCtx};
    std::string buffer;
    CompressionState() { if (!context) fail("cannot allocate zstd context"); }
};

Stamp Stamp::now() {
    return {std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count(),
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()};
}
void validateSymbol(const std::string& symbol) {
    if (symbol.empty() || symbol.size() > 40 || symbol.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-") != std::string::npos)
        fail("invalid symbol");
}
QString validateRoot(const QString& root) {
    if (!QDir::isAbsolutePath(root)) fail("root must be absolute");
    const auto absolute = QDir::cleanPath(root);
    QString existing = absolute;
    QStringList suffix;
    while (!QFileInfo::exists(existing)) {
        const QFileInfo info(existing);
        suffix.prepend(info.fileName());
        const auto parent = info.absolutePath();
        if (parent == existing) fail("root volume is not mounted");
        existing = parent;
    }
    if (!QFileInfo(existing).isDir()) fail("root ancestor is not a directory");
    const QStorageInfo storage(existing);
    // macOS APFS firmlinks make QStorageInfo report the sealed, read-only /
    // volume for writable /private/var. Exclusive file creation checks writes.
    if (!storage.isValid() || !storage.isReady()) fail("root volume is not mounted");
    const auto checkVolume = [&](const QString& path) {
        if (path.startsWith("/Volumes/")) {
            const auto volume = "/Volumes/" + path.mid(9).section('/', 0, 0);
            // QStorageInfo otherwise silently resolves a missing external volume to /.
            const QStorageInfo expected(volume);
            if (!QFileInfo(volume).isDir() || !expected.isReady() || QDir::cleanPath(expected.rootPath()) != volume)
                fail("required volume is not mounted: " + volume.toStdString());
        }
    };
    checkVolume(absolute);
    QString canonical = QFileInfo(existing).canonicalFilePath();
    if (canonical.isEmpty()) fail("cannot canonicalize root");
    for (const auto& part : suffix) canonical += '/' + part;
    checkVolume(canonical);
    const QString forbidden = "/Volumes/T7/sentinel-data/recording";
    const auto forbiddenCanonical = QFileInfo(forbidden).canonicalFilePath();
    for (const auto& path : {absolute, canonical}) {
        if (path == forbidden || path.startsWith(forbidden + '/') ||
            (!forbiddenCanonical.isEmpty() && (path == forbiddenCanonical || path.startsWith(forbiddenCanonical + '/'))))
            fail("refusing the server recording directory");
    }
    return canonical;
}

QString prepareDirectory(const QString& directory) {
    const auto canonical = validateRoot(directory);
    makeDirectories(canonical);
    return canonical;
}

Writer::Writer(WriterConfig config, nlohmann::json metadata)
    : m_config(std::move(config)), m_metadata(std::move(metadata)) {
    validateSymbol(m_config.symbol);
    m_config.root = validateRoot(m_config.root);
    if (m_config.blockBytes == 0 || m_config.blockBytes > MaxRecordBytes || m_config.blockInterval.count() < 1 ||
        m_config.blockInterval > std::chrono::seconds(60) || m_config.compressionLevel < 1 || m_config.compressionLevel > 19)
        fail("invalid block configuration");
    const auto now = Stamp::now();
    m_metadata["format_version"] = 1;
    m_metadata["run_id"] = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    m_metadata["run_started_system_ns"] = now.systemNs;
    m_metadata["products"] = nlohmann::json::array({m_config.symbol});
    m_metadata["channels"] = {"level2", "market_trades", "heartbeats"};
    m_metadata["config"] = {{"root", m_config.root.toStdString()}, {"block_bytes", m_config.blockBytes},
        {"block_ms", m_config.blockInterval.count()}, {"fsync_blocks", m_config.fsyncBlocks},
        {"zstd_level", m_config.compressionLevel}};
    m_block.reserve(m_config.blockBytes);
    m_compression = std::make_unique<CompressionState>();
}
Writer::~Writer() = default;
void Writer::write(const std::string& bytes) {
    if (m_file.write(bytes.data(), bytes.size()) != qint64(bytes.size()))
        fail("write failed: " + m_file.errorString().toStdString());
    m_stats.fileBytes += bytes.size();
}
void Writer::sync() {
    if (!m_file.flush()) fail("flush failed: " + m_file.errorString().toStdString());
    syncFd(m_file.handle());
    m_uncommittedRecord.reset(); m_uncommittedFrame.reset();
}
void Writer::open(Stamp time) {
    validateRoot(m_config.root);
    const auto date = QDateTime::fromMSecsSinceEpoch(time.systemNs / 1000000, QTimeZone::UTC);
    const auto directory = m_config.root + '/' + QString::fromStdString(m_config.symbol) + '/' + date.toString("yyyy/MM/dd");
    validateRoot(directory);
    makeDirectories(directory);
    const auto stem = directory + '/' + date.toString("HH");
    m_path = stem + ".rawl2";
    m_file.setFileName(m_path);
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        if (!QFileInfo::exists(m_path)) fail("cannot create " + m_path.toStdString() + ": " + m_file.errorString().toStdString());
        m_path = stem + '.' + QString::fromStdString(m_metadata.at("run_id").get<std::string>()) +
                 '.' + QString::number(m_segment) + ".rawl2";
        m_file.setFileName(m_path);
        if (!m_file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) fail("cannot create new segment: " + m_path.toStdString());
    }
    m_hour = time.systemNs / HourNs;
    m_index.clear();
    auto header = m_metadata;
    header["segment"] = m_segment++;
    header["first_block_ordinal"] = m_ordinal;
    header["opened_system_ns"] = time.systemNs;
    header["opened_steady_ns"] = time.steadyNs;
    const auto json = header.dump();
    if (json.size() > MaxHeaderBytes) fail("header too large");
    std::string prefix(Magic);
    put32(prefix, static_cast<uint32_t>(json.size()));
    put32(prefix, crc(json));
    write(prefix); write(json);
    sync();
    syncDirectory(directory);
    ++m_stats.files;
    sLog_App("Capture segment opened: path=" << m_path << " segment=" << m_segment - 1);
}
void Writer::append(const Record& record) {
    if (m_closed) fail("append after close");
    if (record.payload.size() > MaxRecordBytes || record.time.systemNs < 0 || record.time.steadyNs < 0)
        fail("record exceeds limits");
    if (record.kind < Kind::Frame || record.kind > Kind::EngineError) fail("unknown record kind");
    if (m_file.isOpen() && (record.time.systemNs / HourNs != m_hour || m_index.size() >= MaxIndexEntries)) seal();
    if (!m_file.isOpen()) open(record.time);
    const auto size = 32 + record.payload.size();
    if (m_count && (m_block.size() + size > m_config.blockBytes ||
                   record.time.steadyNs - m_first.steadyNs >= m_config.blockInterval.count() * 1000000)) flush();
    if (m_index.size() >= MaxIndexEntries) { seal(); open(record.time); }
    const RecordLocation location{record.time, record.connection, record.kind};
    if (!m_uncommittedRecord) m_uncommittedRecord = location;
    if (record.kind == Kind::Frame && !m_uncommittedFrame) m_uncommittedFrame = location;
    if (!m_count) m_first = record.time;
    m_last = record.time;
    put32(m_block, static_cast<uint32_t>(28 + record.payload.size()));
    put32(m_block, static_cast<uint32_t>(record.kind));
    put64(m_block, record.time.systemNs);
    put64(m_block, record.time.steadyNs);
    put64(m_block, record.connection);
    m_block += record.payload;
    ++m_count; ++m_stats.records;
    if (record.kind == Kind::Frame) { ++m_stats.frames; m_stats.frameBytes += record.payload.size(); }
    if (m_block.size() >= m_config.blockBytes) flush();
}
void Writer::flushDue(int64_t steadyNs) {
    if (m_count && steadyNs - m_first.steadyNs >= m_config.blockInterval.count() * 1000000) flush();
}
void Writer::flush() {
    if (!m_count) return;
    auto& compressed = m_compression->buffer;
    compressed.resize(ZSTD_compressBound(m_block.size()));
    const auto size = ZSTD_compressCCtx(m_compression->context.get(), compressed.data(), compressed.size(),
                                      m_block.data(), m_block.size(), m_config.compressionLevel);
    if (ZSTD_isError(size)) fail(std::string("zstd: ") + ZSTD_getErrorName(size));
    compressed.resize(size);
    const BlockIndex entry{uint64_t(m_file.pos()), m_first.systemNs, m_last.systemNs, m_ordinal++, m_count,
                           static_cast<uint32_t>(size), static_cast<uint32_t>(m_block.size())};
    std::string header("BLK1");
    put32(header, entry.compressedBytes); put32(header, entry.rawBytes); put32(header, entry.records);
    put64(header, entry.firstSystemNs); put64(header, entry.lastSystemNs); put64(header, entry.ordinal);
    put32(header, crc(m_block)); put32(header, crc(header));
    write(header); write(compressed);
    if (!m_file.flush()) fail("block flush failed");
    ++m_stats.blocks;
    if (m_config.fsyncBlocks && m_stats.blocks % m_config.fsyncBlocks == 0) sync();
    m_index.push_back(entry);
    m_block.clear(); m_count = 0;
}
void Writer::seal() {
    if (!m_file.isOpen()) return;
    flush();
    const auto index = encodeIndex(m_index);
    std::string footer("IDX1");
    put32(footer, static_cast<uint32_t>(index.size()));
    footer += index;
    put32(footer, crc(index));
    write(footer); sync(); m_file.close();
}
void Writer::close() {
    if (m_closed) return;
    seal(); m_closed = true;
}

std::optional<RecordLocation> Writer::firstUncommitted() const {
    return m_uncommittedFrame ? m_uncommittedFrame : m_uncommittedRecord;
}
void Writer::abandonSegment() {
    m_file.close();
    m_block.clear(); m_count = 0; m_index.clear();
    m_uncommittedRecord.reset(); m_uncommittedFrame.reset();
    m_closed = false;
}

nlohmann::json readHeader(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) fail("cannot read " + path.toStdString());
    return headerFrom(file);
}
ScanResult scan(const QString& path, const RecordVisitor& visitor) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) fail("cannot read " + path.toStdString());
    ScanResult result;
    result.header = headerFrom(file);
    // Snapshot length: a concurrent writer's new tail is outside this scan.
    const auto end = file.size();
    result.fileBytes = end;
    result.validBytes = file.pos();
    uint64_t nextOrdinal = result.header.at("first_block_ordinal").get<uint64_t>();
    while (file.pos() < end) {
        const auto offset = file.pos();
        if (end - offset < 4) { result.tornTail = true; break; }
        const auto magic = read(file, 4);
        if (magic == "IDX1") {
            if (end - file.pos() < 4) { result.tornTail = true; break; }
            const auto length = read(file, 4);
            size_t pos = 0;
            const auto size = get(length, pos, 4);
            if (size < 4 || size > 4 + uint64_t(MaxIndexEntries) * IndexEntryBytes) fail("bad index size");
            if (uint64_t(end - file.pos()) < size + 4) { result.tornTail = true; break; }
            const auto data = read(file, size);
            const auto checksum = read(file, 4); pos = 0;
            if (crc(data) != get(checksum, pos, 4)) fail("index CRC mismatch");
            if (data != encodeIndex(result.index)) fail("index differs from scanned blocks");
            if (file.pos() != end) fail("data after index");
            result.indexed = true;
            result.validBytes = file.pos();
            break;
        }
        if (magic != "BLK1") {
            if (hasFollowingFraming(file, offset + 1, end))
                fail("interior corruption at offset=" + std::to_string(offset));
            result.tornTail = true;
            break;
        }
        if (end - offset < BlockHeaderBytes) { result.tornTail = true; break; }
        const auto header = magic + read(file, BlockHeaderBytes - 4);
        size_t pos = 44;
        if (crc(std::string_view(header).substr(0, 44)) != get(header, pos, 4)) fail("block header CRC mismatch");
        pos = 4;
        BlockIndex entry;
        entry.offset = offset;
        entry.compressedBytes = get(header, pos, 4); entry.rawBytes = get(header, pos, 4); entry.records = get(header, pos, 4);
        entry.firstSystemNs = get(header, pos, 8); entry.lastSystemNs = get(header, pos, 8); entry.ordinal = get(header, pos, 8);
        const auto checksum = get(header, pos, 4);
        if (!entry.rawBytes || entry.rawBytes > MaxBlockBytes || !entry.compressedBytes ||
            entry.compressedBytes > ZSTD_compressBound(MaxBlockBytes) || !entry.records ||
            entry.records > entry.rawBytes / 32 || entry.ordinal != nextOrdinal || result.index.size() >= MaxIndexEntries)
            fail("invalid block limits/ordinal");
        if (end - file.pos() < entry.compressedBytes) { result.tornTail = true; break; }
        const auto compressed = read(file, entry.compressedBytes);
        if (ZSTD_findFrameCompressedSize(compressed.data(), compressed.size()) != compressed.size()) fail("invalid zstd frame boundary");
        std::string raw(entry.rawBytes, '\0');
        const auto size = ZSTD_decompress(raw.data(), raw.size(), compressed.data(), compressed.size());
        if (ZSTD_isError(size) || size != raw.size()) fail("zstd decompression failed");
        if (crc(raw) != checksum) fail("block CRC mismatch");
        // Validate the whole block's framing before exposing any record.
        pos = 0;
        for (uint32_t i = 0; i < entry.records; ++i) {
            const auto length = get(raw, pos, 4);
            if (length < 28 || length > MaxRecordBytes + 28 || length > raw.size() - pos) fail("bad record length");
            const auto start = pos;
            const auto kind = get(raw, pos, 4);
            if (kind < uint32_t(Kind::Frame) || kind > uint32_t(Kind::EngineError)) fail("bad record kind");
            const auto systemNs = get(raw, pos, 8);
            const auto steadyNs = get(raw, pos, 8);
            if (systemNs > uint64_t(INT64_MAX) || steadyNs > uint64_t(INT64_MAX)) fail("invalid timestamp");
            if ((i == 0 && int64_t(systemNs) != entry.firstSystemNs) ||
                (i + 1 == entry.records && int64_t(systemNs) != entry.lastSystemNs)) fail("block timestamp mismatch");
            pos = start + length;
        }
        if (pos != raw.size()) fail("record count mismatch");
        if (visitor) {
            pos = 0;
            for (uint32_t i = 0; i < entry.records; ++i) {
                const auto length = get(raw, pos, 4);
                Record record;
                record.kind = static_cast<Kind>(get(raw, pos, 4));
                record.time.systemNs = get(raw, pos, 8); record.time.steadyNs = get(raw, pos, 8);
                record.connection = get(raw, pos, 8);
                record.payload = raw.substr(pos, length - 28); pos += length - 28;
                visitor(record);
            }
        }
        result.index.push_back(entry); ++nextOrdinal;
        result.validBytes = file.pos();
    }
    return result;
}
} // namespace sentinel::capture
