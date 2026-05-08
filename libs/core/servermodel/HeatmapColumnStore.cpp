#include "HeatmapColumnStore.hpp"

#include "SentinelLogging.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <system_error>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

constexpr int64_t kMsPerDay = 86'400'000;

std::string formatDayDir(int64_t dayStartMs) {
    using namespace std::chrono;
    const auto tp = system_clock::time_point(milliseconds(dayStartMs));
    const auto day = floor<days>(tp);
    const std::chrono::year_month_day ymd{day};
    std::ostringstream oss;
    oss << static_cast<int>(ymd.year()) << '-'
        << std::setfill('0') << std::setw(2) << static_cast<unsigned>(ymd.month()) << '-'
        << std::setw(2) << static_cast<unsigned>(ymd.day());
    return oss.str();
}

void copySymbolField(const std::string& symbol, char (&dst)[24]) noexcept {
    std::memset(dst, 0, sizeof(dst));
    const auto n = std::min(symbol.size(), sizeof(dst) - 1);
    std::memcpy(dst, symbol.data(), n);
}

} // namespace

HeatmapColumnStore::HeatmapColumnStore(fs::path baseDir, Config cfg)
    : m_config(cfg), m_baseDir(std::move(baseDir)) {
    std::error_code ec;
    fs::create_directories(m_baseDir, ec);
    m_lockPath = m_baseDir / ".lock";
}

HeatmapColumnStore::~HeatmapColumnStore() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& [key, writer] : m_writers) {
            if (writer.stream.is_open()) {
                writer.stream.flush();
                writer.stream.close();
            }
        }
        m_writers.clear();
    }
    if (m_lockFd >= 0) {
        // Closing the fd releases the flock implicitly.
        ::close(m_lockFd);
        m_lockFd = -1;
    }
}

bool HeatmapColumnStore::acquireLock() {
    if (m_lockFd >= 0) return true;

    std::error_code ec;
    fs::create_directories(m_baseDir, ec);

    const int fd = ::open(m_lockPath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {
        sLog_Error("HeatmapColumnStore: cannot open lock file " << m_lockPath
                  << " errno=" << errno);
        return false;
    }
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        const int err = errno;
        ::close(fd);
        if (err == EWOULDBLOCK) {
            sLog_Warning("HeatmapColumnStore: lock " << m_lockPath
                         << " held by another process; refusing to start writer");
        } else {
            sLog_Error("HeatmapColumnStore: flock failed errno=" << err);
        }
        return false;
    }
    m_lockFd = fd;
    return true;
}

fs::path HeatmapColumnStore::filePathFor(const fs::path& baseDir,
                                         const std::string& symbol,
                                         int64_t timeframeMs,
                                         int64_t dayStartMs) {
    return baseDir / symbol / std::to_string(timeframeMs) / "v1"
                   / (formatDayDir(dayStartMs) + ".hmcol");
}

HeatmapColumnStore::DayWriter*
HeatmapColumnStore::openOrGet(const DayWriterKey& key,
                              int32_t gridHeight,
                              uint8_t liquidityFormat,
                              const std::string& symbol) {
    auto it = m_writers.find(key);
    if (it != m_writers.end()) {
        // Validate that gridHeight + liquidity layout match what the file was opened with.
        if (it->second.gridHeight != gridHeight || it->second.liquidityFormat != liquidityFormat) {
            sLog_Error("HeatmapColumnStore: layout mismatch for "
                       << symbol << " tf=" << key.timeframeMs
                       << " day=" << key.dayStartMs
                       << " expected gridHeight=" << it->second.gridHeight
                       << "/liq=" << static_cast<int>(it->second.liquidityFormat)
                       << " got gridHeight=" << gridHeight
                       << "/liq=" << static_cast<int>(liquidityFormat));
            return nullptr;
        }
        return &it->second;
    }

    DayWriter w;
    w.dayStartMs = key.dayStartMs;
    w.timeframeMs = key.timeframeMs;
    w.gridHeight = gridHeight;
    w.liquidityFormat = liquidityFormat;
    w.recordStride = hmcol::recordStride(gridHeight, liquidityFormat);
    w.slotsPerDay = (kMsPerDay / key.timeframeMs);
    w.path = filePathFor(m_baseDir, symbol, key.timeframeMs, key.dayStartMs);

    std::error_code ec;
    fs::create_directories(w.path.parent_path(), ec);
    if (ec) {
        sLog_Error("HeatmapColumnStore: mkdir " << w.path.parent_path()
                   << " failed: " << ec.message());
        return nullptr;
    }

    const bool isNew = !fs::exists(w.path);

    // Open read+write+binary. New files are created; existing files are not truncated.
    w.stream.open(w.path, std::ios::in | std::ios::out | std::ios::binary);
    if (!w.stream.is_open()) {
        // Create file then reopen.
        std::ofstream create(w.path, std::ios::binary);
        if (!create) {
            sLog_Error("HeatmapColumnStore: cannot create " << w.path);
            return nullptr;
        }
        create.close();
        w.stream.open(w.path, std::ios::in | std::ios::out | std::ios::binary);
        if (!w.stream.is_open()) {
            sLog_Error("HeatmapColumnStore: cannot open " << w.path);
            return nullptr;
        }
    }

    if (isNew) {
        // Write header.
        hmcol::FileHeader header{};
        header.magic = hmcol::kMagic;
        header.version = hmcol::kVersion;
        header.timeframeMs = key.timeframeMs;
        header.gridHeight = gridHeight;
        header.intensityFormat = hmcol::kIntensityFormatU16;
        header.liquidityFormat = liquidityFormat;
        header.dayStartMs = key.dayStartMs;
        copySymbolField(symbol, header.symbol);
        hmcol::finalizeFileHeaderCrc(header);

        w.stream.write(reinterpret_cast<const char*>(&header), sizeof(header));
        if (!w.stream) {
            sLog_Error("HeatmapColumnStore: header write failed for " << w.path);
            return nullptr;
        }
        w.stream.flush();

        // Sparse-extend the file to cover the full day. fs::resize_file pads with
        // zeros (sparse on macOS/Linux APFS/ext4), which gives empty slots
        // bucketStartMs == 0 == kEmptySlotSentinel for free.
        const std::size_t fullSize =
            sizeof(hmcol::FileHeader) +
            static_cast<std::size_t>(w.slotsPerDay) * w.recordStride;
        fs::resize_file(w.path, fullSize, ec);
        if (ec) {
            sLog_Error("HeatmapColumnStore: resize " << w.path
                       << " to " << fullSize << " failed: " << ec.message());
            return nullptr;
        }
        // Reopen so the stream's view of the file size is current.
        w.stream.close();
        w.stream.open(w.path, std::ios::in | std::ios::out | std::ios::binary);
        if (!w.stream.is_open()) {
            sLog_Error("HeatmapColumnStore: reopen " << w.path << " failed");
            return nullptr;
        }
    } else {
        // Validate existing header.
        hmcol::FileHeader header{};
        w.stream.seekg(0);
        w.stream.read(reinterpret_cast<char*>(&header), sizeof(header));
        if (!w.stream || !hmcol::verifyFileHeader(header)) {
            sLog_Error("HeatmapColumnStore: header invalid in " << w.path
                       << " — refusing to write");
            return nullptr;
        }
        if (header.timeframeMs != key.timeframeMs ||
            header.gridHeight != gridHeight ||
            header.liquidityFormat != liquidityFormat ||
            header.dayStartMs != key.dayStartMs) {
            sLog_Error("HeatmapColumnStore: header config mismatch in " << w.path);
            return nullptr;
        }
    }

    w.lastFlush = std::chrono::steady_clock::now();

    auto [insertedIt, inserted] = m_writers.emplace(key, std::move(w));
    return &insertedIt->second;
}

void HeatmapColumnStore::maybeFlush(DayWriter& w) {
    ++w.recordsSinceFlush;
    const auto now = std::chrono::steady_clock::now();
    const auto since = std::chrono::duration_cast<std::chrono::milliseconds>(now - w.lastFlush).count();
    const bool byCount = w.recordsSinceFlush >= m_config.fsyncEveryNRecords;
    const bool byTime = since >= m_config.fsyncEveryMs;
    if (byCount || byTime) {
        w.stream.flush();
        w.recordsSinceFlush = 0;
        w.lastFlush = now;
        ++m_stats.flushes;
    }
}

HeatmapColumnStore::AppendResult
HeatmapColumnStore::append(const std::string& symbol,
                           int64_t timeframeMs,
                           int32_t gridHeight,
                           int64_t bucketStartMs,
                           int64_t bucketEndMs,
                           double minPrice,
                           double maxPrice,
                           double tickSize,
                           const uint16_t* intensity,
                           const uint16_t* liquidity,
                           double liquidityScale) {
    if (m_lockFd < 0) {
        ++m_stats.badInputs;
        return AppendResult::IoError;
    }
    if (timeframeMs <= 0 || gridHeight <= 0 || bucketStartMs <= 0 ||
        bucketEndMs <= bucketStartMs || intensity == nullptr || symbol.empty()) {
        ++m_stats.badInputs;
        return AppendResult::BadInput;
    }
    if (kMsPerDay % timeframeMs != 0) {
        // Phase 1 v1 only supports timeframes that evenly divide a UTC day.
        ++m_stats.badInputs;
        return AppendResult::BadInput;
    }

    const int64_t dayStartMs = hmcol::dayStartForEpoch(bucketStartMs);
    const int64_t slot = hmcol::slotForBucket(bucketStartMs, dayStartMs, timeframeMs);
    const uint8_t liquidityFormat = (liquidity != nullptr)
        ? hmcol::kLiquidityFormatU16
        : hmcol::kLiquidityFormatNone;
    if (slot < 0 || slot >= (kMsPerDay / timeframeMs)) {
        ++m_stats.badInputs;
        return AppendResult::BadInput;
    }
    if (bucketStartMs == hmcol::kEmptySlotSentinel) {
        // The on-disk format reserves bucketStartMs == 0 to mean "empty slot",
        // so a real bucket at epoch 0 cannot be expressed. Reject.
        ++m_stats.badInputs;
        return AppendResult::BadInput;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    DayWriterKey key{symbol, timeframeMs, dayStartMs};
    DayWriter* w = openOrGet(key, gridHeight, liquidityFormat, symbol);
    if (w == nullptr) {
        ++m_stats.ioErrors;
        return AppendResult::IoError;
    }

    const std::size_t offset = hmcol::slotOffset(slot, gridHeight, liquidityFormat);

    // Read existing record header at slot.
    hmcol::RecordHeader existing{};
    w->stream.clear();
    w->stream.seekg(static_cast<std::streamoff>(offset));
    w->stream.read(reinterpret_cast<char*>(&existing), sizeof(existing));
    if (!w->stream) {
        sLog_Error("HeatmapColumnStore: read slot " << slot << " of " << w->path << " failed");
        w->stream.clear();
        ++m_stats.ioErrors;
        return AppendResult::IoError;
    }

    if (existing.bucketStartMs != hmcol::kEmptySlotSentinel) {
        if (existing.bucketStartMs == bucketStartMs) {
            // Re-presented same bucket. Treat as no-op success.
            ++m_stats.alreadyPresent;
            return AppendResult::AlreadyPresent;
        }
        sLog_Warning("HeatmapColumnStore: slot conflict in " << w->path
                     << " slot=" << slot
                     << " existing bucketStart=" << existing.bucketStartMs
                     << " new bucketStart=" << bucketStartMs
                     << " — refusing to overwrite");
        ++m_stats.slotConflicts;
        return AppendResult::SlotConflict;
    }

    // Build new record.
    hmcol::RecordHeader hdr{};
    hdr.bucketStartMs = bucketStartMs;
    hdr.bucketEndMs = bucketEndMs;
    hdr.minPrice = minPrice;
    hdr.maxPrice = maxPrice;
    hdr.tickSize = tickSize;
    hdr.liquidityScale = liquidityScale;
    hdr.flags = (liquidity != nullptr) ? hmcol::kFlagHasLiquidity : 0u;

    const std::size_t intensityBytes = static_cast<std::size_t>(gridHeight) * sizeof(uint16_t);
    const std::size_t liquidityBytes = (liquidity != nullptr) ? intensityBytes : 0;
    uint32_t crc = hmcol::crc32Update(0, intensity, intensityBytes);
    if (liquidity != nullptr) {
        crc = hmcol::crc32Update(crc, liquidity, liquidityBytes);
    }
    hdr.recordCrc32 = crc;

    // Write record header + payload at slot offset.
    w->stream.clear();
    w->stream.seekp(static_cast<std::streamoff>(offset));
    w->stream.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    w->stream.write(reinterpret_cast<const char*>(intensity), static_cast<std::streamsize>(intensityBytes));
    if (liquidity != nullptr) {
        w->stream.write(reinterpret_cast<const char*>(liquidity), static_cast<std::streamsize>(liquidityBytes));
    }
    if (!w->stream) {
        sLog_Error("HeatmapColumnStore: write slot " << slot << " of " << w->path << " failed");
        w->stream.clear();
        ++m_stats.ioErrors;
        return AppendResult::IoError;
    }

    maybeFlush(*w);
    ++m_stats.written;
    return AppendResult::Written;
}

fs::path HeatmapColumnStore::latestFileFor(const std::string& symbol, int64_t timeframeMs) const {
    const fs::path dir = m_baseDir / symbol / std::to_string(timeframeMs) / "v1";
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return {};

    fs::path best;
    for (auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".hmcol") continue;
        // Filenames are YYYY-MM-DD.hmcol — lexicographic order == chronological.
        if (best.empty() || entry.path().filename() > best.filename()) {
            best = entry.path();
        }
    }
    return best;
}

fs::path HeatmapColumnStore::earliestFileFor(const std::string& symbol, int64_t timeframeMs) const {
    const fs::path dir = m_baseDir / symbol / std::to_string(timeframeMs) / "v1";
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return {};

    fs::path best;
    for (auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".hmcol") continue;
        if (best.empty() || entry.path().filename() < best.filename()) {
            best = entry.path();
        }
    }
    return best;
}

int HeatmapColumnStore::scanFileBackwards(const fs::path& file,
                                          int64_t untilSlotInclusive,
                                          int maxCount,
                                          std::vector<LoadedColumn>& accum,
                                          int* crcFails) const {
    if (file.empty() || maxCount <= 0) return 0;

    std::ifstream in(file, std::ios::binary);
    if (!in) {
        sLog_Error("HeatmapColumnStore: cannot open " << file << " for reading");
        return 0;
    }

    hmcol::FileHeader header{};
    in.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!in || !hmcol::verifyFileHeader(header)) {
        sLog_Error("HeatmapColumnStore: header invalid in " << file
                   << " — skipping");
        return 0;
    }

    const int64_t slotsPerDay = kMsPerDay / header.timeframeMs;
    const int64_t startSlot = std::min<int64_t>(untilSlotInclusive, slotsPerDay - 1);
    const std::size_t intensityBytes = static_cast<std::size_t>(header.gridHeight) * sizeof(uint16_t);
    const std::size_t liquidityBytes = (header.liquidityFormat == hmcol::kLiquidityFormatU16)
        ? intensityBytes : 0;

    int pushed = 0;
    for (int64_t slot = startSlot; slot >= 0; --slot) {
        const std::size_t offset = hmcol::slotOffset(slot, header.gridHeight, header.liquidityFormat);
        in.clear();
        in.seekg(static_cast<std::streamoff>(offset));

        hmcol::RecordHeader rec{};
        in.read(reinterpret_cast<char*>(&rec), sizeof(rec));
        if (!in) break;
        if (rec.bucketStartMs == hmcol::kEmptySlotSentinel) continue;

        LoadedColumn col;
        col.intensity.resize(intensityBytes);
        in.read(reinterpret_cast<char*>(col.intensity.data()),
                static_cast<std::streamsize>(intensityBytes));
        if (!in) break;
        if (liquidityBytes > 0) {
            col.liquidity.resize(liquidityBytes);
            in.read(reinterpret_cast<char*>(col.liquidity.data()),
                    static_cast<std::streamsize>(liquidityBytes));
            if (!in) break;
        }

        uint32_t crc = hmcol::crc32Update(0, col.intensity.data(), intensityBytes);
        if (liquidityBytes > 0) {
            crc = hmcol::crc32Update(crc, col.liquidity.data(), liquidityBytes);
        }
        if (crc != rec.recordCrc32) {
            if (crcFails) ++(*crcFails);
            sLog_Warning("HeatmapColumnStore: record CRC mismatch in " << file
                         << " slot=" << slot << " — skipping");
            continue;
        }

        col.bucketStartMs  = rec.bucketStartMs;
        col.bucketEndMs    = rec.bucketEndMs;
        col.minPrice       = rec.minPrice;
        col.maxPrice       = rec.maxPrice;
        col.tickSize       = rec.tickSize;
        col.liquidityScale = rec.liquidityScale;
        col.gridHeight     = header.gridHeight;
        accum.push_back(std::move(col));
        ++pushed;

        if (static_cast<int>(accum.size()) >= maxCount) break;
    }
    return pushed;
}

bool HeatmapColumnStore::hasAnyFile(const std::string& symbol, int64_t timeframeMs) const {
    return !latestFileFor(symbol, timeframeMs).empty();
}

void HeatmapColumnStore::flushOpenWritersFor(const std::string& symbol,
                                             int64_t timeframeMs) const {
    for (auto& [key, w] : m_writers) {
        if (key.symbol == symbol && key.timeframeMs == timeframeMs && w.stream.is_open()) {
            w.stream.flush();
            w.recordsSinceFlush = 0;
            w.lastFlush = std::chrono::steady_clock::now();
        }
    }
}

bool HeatmapColumnStore::loadRecent(const std::string& symbol,
                                    int64_t timeframeMs,
                                    int maxCount,
                                    std::vector<LoadedColumn>& out) const {
    out.clear();
    if (timeframeMs <= 0 || maxCount <= 0 || symbol.empty()) return false;

    std::lock_guard<std::mutex> lock(m_mutex);
    flushOpenWritersFor(symbol, timeframeMs);

    const fs::path file = latestFileFor(symbol, timeframeMs);
    if (file.empty()) return false;

    std::vector<LoadedColumn> reverseAccum;
    reverseAccum.reserve(static_cast<std::size_t>(maxCount));

    int crcFails = 0;
    scanFileBackwards(file, /*untilSlotInclusive=*/std::numeric_limits<int64_t>::max(),
                      maxCount, reverseAccum, &crcFails);

    out.assign(reverseAccum.rbegin(), reverseAccum.rend());
    if (crcFails > 0) {
        sLog_Warning("HeatmapColumnStore: " << crcFails
                     << " record(s) skipped due to CRC failure in " << file);
    }
    return !out.empty();
}

bool HeatmapColumnStore::fetchRange(const std::string& symbol,
                                    int64_t timeframeMs,
                                    int64_t endMs,
                                    int maxCount,
                                    std::vector<LoadedColumn>& out) const {
    out.clear();
    if (timeframeMs <= 0 || maxCount <= 0 || symbol.empty() || endMs < 0) return false;

    std::lock_guard<std::mutex> lock(m_mutex);
    flushOpenWritersFor(symbol, timeframeMs);

    std::vector<LoadedColumn> reverseAccum;
    reverseAccum.reserve(static_cast<std::size_t>(std::min(maxCount, 8192)));

    int totalCrcFails = 0;
    int64_t dayMs = hmcol::dayStartForEpoch(endMs);

    for (int day = 0;
         day < kMaxDaysScanned && static_cast<int>(reverseAccum.size()) < maxCount;
         ++day, dayMs -= kMsPerDay) {
        const fs::path file = filePathFor(m_baseDir, symbol, timeframeMs, dayMs);
        std::error_code ec;
        if (!fs::exists(file, ec)) {
            // Stop early if we've gone past the earliest persisted file.
            const fs::path earliest = earliestFileFor(symbol, timeframeMs);
            if (earliest.empty() || file.filename() < earliest.filename()) break;
            continue; // gap in middle (server downtime): skip and keep walking
        }

        // Constrain start slot on the most recent day so we don't return
        // buckets newer than endMs.
        int64_t untilSlot = std::numeric_limits<int64_t>::max();
        if (day == 0) {
            untilSlot = hmcol::slotForBucket(endMs, dayMs, timeframeMs);
            if (untilSlot < 0) continue;
        }

        const int needed = maxCount - static_cast<int>(reverseAccum.size());
        scanFileBackwards(file, untilSlot, needed, reverseAccum, &totalCrcFails);
    }

    out.assign(reverseAccum.rbegin(), reverseAccum.rend());
    if (totalCrcFails > 0) {
        sLog_Warning("HeatmapColumnStore: " << totalCrcFails
                     << " record(s) skipped due to CRC failure across "
                     << kMaxDaysScanned << "-day fetchRange scan");
    }
    return !out.empty();
}

int64_t HeatmapColumnStore::oldestPersistedMs(const std::string& symbol,
                                              int64_t timeframeMs) const {
    if (timeframeMs <= 0 || symbol.empty()) return 0;

    std::lock_guard<std::mutex> lock(m_mutex);
    flushOpenWritersFor(symbol, timeframeMs);

    const fs::path earliest = earliestFileFor(symbol, timeframeMs);
    if (earliest.empty()) return 0;

    // Open and walk forward to find the lowest populated slot.
    std::ifstream in(earliest, std::ios::binary);
    if (!in) return 0;
    hmcol::FileHeader header{};
    in.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!in || !hmcol::verifyFileHeader(header)) return 0;

    const int64_t slotsPerDay = kMsPerDay / header.timeframeMs;
    const std::size_t stride = hmcol::recordStride(header.gridHeight, header.liquidityFormat);
    for (int64_t slot = 0; slot < slotsPerDay; ++slot) {
        const std::size_t offset = hmcol::slotOffset(slot, header.gridHeight, header.liquidityFormat);
        in.clear();
        in.seekg(static_cast<std::streamoff>(offset));
        hmcol::RecordHeader rec{};
        in.read(reinterpret_cast<char*>(&rec), sizeof(rec));
        if (!in) return 0;
        if (rec.bucketStartMs != hmcol::kEmptySlotSentinel) return rec.bucketStartMs;
    }
    return 0;
}

void HeatmapColumnStore::flush() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& [key, w] : m_writers) {
        if (w.stream.is_open()) {
            w.stream.flush();
            w.recordsSinceFlush = 0;
            w.lastFlush = std::chrono::steady_clock::now();
            ++m_stats.flushes;
        }
    }
}

HeatmapColumnStore::Stats HeatmapColumnStore::stats() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_stats;
}
