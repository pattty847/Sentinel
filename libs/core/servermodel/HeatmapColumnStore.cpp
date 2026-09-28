#include "HeatmapColumnStore.hpp"
#include "PersistenceIo.hpp"

#include "SentinelLogging.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

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

using sentinel::persistence::syncFilePath;

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
                syncWriter(writer);
                writer.stream.close();
            }
        }
        m_writers.clear();
    }
#ifdef _WIN32
    if (m_lockHandle != nullptr) {
        OVERLAPPED overlapped{};
        UnlockFileEx(static_cast<HANDLE>(m_lockHandle), 0, MAXDWORD, MAXDWORD, &overlapped);
        CloseHandle(static_cast<HANDLE>(m_lockHandle));
        m_lockHandle = nullptr;
    }
#else
    if (m_lockFd >= 0) {
        // Closing the fd releases the flock implicitly.
        ::close(m_lockFd);
        m_lockFd = -1;
    }
#endif
}

bool HeatmapColumnStore::acquireLock() {
#ifdef _WIN32
    if (m_lockHandle != nullptr) return true;
#else
    if (m_lockFd >= 0) return true;
#endif

    std::error_code ec;
    fs::create_directories(m_baseDir, ec);

    int error = 0;
    const auto handle = sentinel::persistence::acquireFileLock(m_lockPath, error);
    if (handle == sentinel::persistence::noLock) {
        if (sentinel::persistence::lockIsContended(error)) {
            sLog_Warning("HeatmapColumnStore: lock " << m_lockPath << " held by another process, error=" << error);
        } else {
            sLog_Error("HeatmapColumnStore: cannot acquire lock " << m_lockPath << " error=" << error);
        }
        return false;
    }
#ifdef _WIN32
    m_lockHandle = handle;
#else
    m_lockFd = handle;
#endif
    sLog_Data("HeatmapColumnStore: writer lock acquired: path=" << m_lockPath);
    return true;
}

bool HeatmapColumnStore::isLocked() const noexcept {
#ifdef _WIN32
    return m_lockHandle != nullptr;
#else
    return m_lockFd >= 0;
#endif
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

    // A long-running process needs at most one open UTC day per symbol and
    // timeframe. Sync and close the previous day before rotating forward (or
    // reopening an older day during a repair/backfill).
    for (auto writerIt = m_writers.begin(); writerIt != m_writers.end();) {
        const auto& openKey = writerIt->first;
        if (openKey.symbol == key.symbol &&
            openKey.timeframeMs == key.timeframeMs &&
            openKey.dayStartMs != key.dayStartMs) {
            if (writerIt->second.stream.is_open()) {
                if (!syncWriter(writerIt->second)) {
                    ++m_stats.ioErrors;
                    return nullptr;
                }
                writerIt->second.stream.close();
            }
            sLog_Data("HeatmapColumnStore: closed day file for rotation: path=" << writerIt->second.path
                      << " nextDay=" << formatDayDir(key.dayStartMs));
            writerIt = m_writers.erase(writerIt);
        } else {
            ++writerIt;
        }
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
            sLog_Error("HeatmapColumnStore: header config mismatch in " << w.path
                       << " — refusing to write: file tfMs=" << header.timeframeMs
                       << " gridHeight=" << header.gridHeight
                       << " liq=" << static_cast<int>(header.liquidityFormat)
                       << " dayStart=" << header.dayStartMs
                       << " wanted tfMs=" << key.timeframeMs
                       << " gridHeight=" << gridHeight
                       << " liq=" << static_cast<int>(liquidityFormat)
                       << " dayStart=" << key.dayStartMs);
            return nullptr;
        }
    }

    sLog_Data("HeatmapColumnStore: opened day file: path=" << w.path
              << " new=" << isNew
              << " tfMs=" << key.timeframeMs
              << " gridHeight=" << gridHeight
              << " liq=" << static_cast<int>(liquidityFormat)
              << " slots=" << w.slotsPerDay);

    w.lastFlush = std::chrono::steady_clock::now();

    auto [insertedIt, inserted] = m_writers.emplace(key, std::move(w));
    return &insertedIt->second;
}

bool HeatmapColumnStore::syncWriter(DayWriter& w) {
    w.stream.flush();
    if (!w.stream) {
        sLog_Error("HeatmapColumnStore: stream flush failed for " << w.path);
        return false;
    }

    int errorCode = 0;
    if (!syncFilePath(w.path, errorCode)) {
        sLog_Error("HeatmapColumnStore: durable sync failed for " << w.path
                   << " error=" << errorCode);
        return false;
    }
    return true;
}

bool HeatmapColumnStore::maybeFlush(DayWriter& w) {
    ++w.recordsSinceFlush;
    const auto now = std::chrono::steady_clock::now();
    const auto since = std::chrono::duration_cast<std::chrono::milliseconds>(now - w.lastFlush).count();
    const bool byCount = w.recordsSinceFlush >= m_config.fsyncEveryNRecords;
    const bool byTime = since >= m_config.fsyncEveryMs;
    if (byCount || byTime) {
        if (!syncWriter(w)) {
            return false;
        }
        w.recordsSinceFlush = 0;
        w.lastFlush = now;
        ++m_stats.flushes;
    }
    return true;
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
    if (!isLocked()) {
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

    if (!maybeFlush(*w)) {
        ++m_stats.ioErrors;
        return AppendResult::IoError;
    }
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

HeatmapColumnStore::ScanResult
HeatmapColumnStore::scanFileBackwards(const fs::path& file,
                                      int64_t untilSlotInclusive,
                                      int maxCount,
                                      std::vector<LoadedColumn>& accum,
                                      int* crcFails,
                                      int64_t startMs) const {
    ScanResult result;
    if (file.empty() || maxCount <= 0) return result;

    std::ifstream in(file, std::ios::binary);
    if (!in) {
        sLog_Error("HeatmapColumnStore: cannot open " << file << " for reading");
        return result;
    }

    hmcol::FileHeader header{};
    in.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!in || !hmcol::verifyFileHeader(header)) {
        sLog_Error("HeatmapColumnStore: header invalid in " << file
                   << " — skipping");
        return result;
    }

    const int64_t slotsPerDay = kMsPerDay / header.timeframeMs;
    const int64_t startSlot = std::min<int64_t>(untilSlotInclusive, slotsPerDay - 1);
    const std::size_t intensityBytes = static_cast<std::size_t>(header.gridHeight) * sizeof(uint16_t);
    const std::size_t liquidityBytes = (header.liquidityFormat == hmcol::kLiquidityFormatU16)
        ? intensityBytes : 0;

    for (int64_t slot = startSlot; slot >= 0; --slot) {
        const std::size_t offset = hmcol::slotOffset(slot, header.gridHeight, header.liquidityFormat);
        in.clear();
        in.seekg(static_cast<std::streamoff>(offset));

        hmcol::RecordHeader rec{};
        in.read(reinterpret_cast<char*>(&rec), sizeof(rec));
        if (!in) break;
        if (rec.bucketStartMs == hmcol::kEmptySlotSentinel) continue;

        // Phase 4 floor: stop early once we cross below startMs.
        if (startMs > 0 && rec.bucketStartMs < startMs) {
            result.hitFloor = true;
            break;
        }

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
            // Callers log one summary warning per scan; per-record detail is a probe.
            sLog_Probe("persist.crc", "file=" << file.string() << " slot=" << slot
                       << " bucketStart=" << rec.bucketStartMs);
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
        ++result.pushed;

        // maxCount is this file's budget; accum may already hold newer days.
        if (result.pushed >= maxCount) break;
    }
    return result;
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
                     << " record(s) skipped due to CRC failure in " << file
                     << " (enable probe persist.crc for slots)");
    }
    return !out.empty();
}

bool HeatmapColumnStore::fetchRange(const std::string& symbol,
                                    int64_t timeframeMs,
                                    int64_t endMs,
                                    int maxCount,
                                    std::vector<LoadedColumn>& out,
                                    int64_t startMs) const {
    out.clear();
    if (timeframeMs <= 0 || maxCount <= 0 || symbol.empty() || endMs < 0) return false;

    std::lock_guard<std::mutex> lock(m_mutex);
    flushOpenWritersFor(symbol, timeframeMs);

    if (endMs == 0) {
        // No upper bound: start from the newest day file.
        const fs::path latest = latestFileFor(symbol, timeframeMs);
        if (latest.empty()) return false;
        std::ifstream in(latest, std::ios::binary);
        hmcol::FileHeader header{};
        in.read(reinterpret_cast<char*>(&header), sizeof(header));
        if (!in || !hmcol::verifyFileHeader(header)) return false;
        endMs = header.dayStartMs + kMsPerDay - 1;
    }

    std::vector<LoadedColumn> reverseAccum;
    reverseAccum.reserve(static_cast<std::size_t>(std::min(maxCount, 8192)));

    int totalCrcFails = 0;
    int64_t dayMs = hmcol::dayStartForEpoch(endMs);

    for (int day = 0;
         day < kMaxDaysScanned && static_cast<int>(reverseAccum.size()) < maxCount;
         ++day, dayMs -= kMsPerDay) {
        // Day-boundary floor optimization: if startMs is set and even the
        // newest possible bucket on this day is older than startMs, stop.
        if (startMs > 0) {
            const int64_t latestOnDay = dayMs + (kMsPerDay - timeframeMs);
            if (latestOnDay < startMs) break;
        }

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
        const ScanResult sr = scanFileBackwards(file, untilSlot, needed,
                                                reverseAccum, &totalCrcFails, startMs);
        if (sr.hitFloor) break; // no point in scanning older days
    }

    out.assign(reverseAccum.rbegin(), reverseAccum.rend());
    if (totalCrcFails > 0) {
        sLog_Warning("HeatmapColumnStore: " << totalCrcFails
                     << " record(s) skipped due to CRC failure in fetchRange: symbol=" << symbol
                     << " tfMs=" << timeframeMs << " end=" << endMs << " start=" << startMs
                     << " (enable probe persist.crc for slots)");
    }
    sLog_Probe("persist.fetch",
               "symbol=" << symbol << " tfMs=" << timeframeMs
               << " start=" << startMs << " end=" << endMs << " maxCount=" << maxCount
               << " returned=" << out.size()
               << " first=" << (out.empty() ? 0 : out.front().bucketStartMs)
               << " last=" << (out.empty() ? 0 : out.back().bucketStartMs));
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
            if (!syncWriter(w)) {
                ++m_stats.ioErrors;
                continue;
            }
            w.recordsSinceFlush = 0;
            w.lastFlush = std::chrono::steady_clock::now();
            ++m_stats.flushes;
        }
    }
}

int HeatmapColumnStore::enforceRetention(int retentionDays) {
    using namespace std::chrono;
    const int64_t nowMs = duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()).count();
    return enforceRetentionAt(nowMs, retentionDays);
}

int HeatmapColumnStore::enforceRetentionAt(int64_t nowMs, int retentionDays) {
    if (retentionDays <= 0) return 0;

    const int64_t cutoff = hmcol::dayStartForEpoch(nowMs)
                           - static_cast<int64_t>(retentionDays) * kMsPerDay;

    std::lock_guard<std::mutex> lock(m_mutex);

    int removed = 0;
    std::error_code ec;
    if (!fs::is_directory(m_baseDir, ec)) return 0;

    // Walk <baseDir>/<symbol>/<tf>/v1/*.hmcol.
    for (auto& symDir : fs::directory_iterator(m_baseDir, ec)) {
        if (ec) break;
        if (!symDir.is_directory()) continue;
        for (auto& tfDir : fs::directory_iterator(symDir.path(), ec)) {
            if (ec) break;
            if (!tfDir.is_directory()) continue;
            const fs::path versionDir = tfDir.path() / "v1";
            if (!fs::is_directory(versionDir, ec)) continue;
            for (auto& fileEntry : fs::directory_iterator(versionDir, ec)) {
                if (ec) break;
                if (!fileEntry.is_regular_file()) continue;
                const fs::path& file = fileEntry.path();
                if (file.extension() != ".hmcol") continue;

                // Skip files matching an open writer — should not happen at
                // startup but we never want to yank one out from under one.
                bool hasOpenWriter = false;
                for (auto& [key, w] : m_writers) {
                    if (w.path == file) { hasOpenWriter = true; break; }
                }
                if (hasOpenWriter) continue;

                // Parse YYYY-MM-DD from filename stem.
                const std::string stem = file.stem().string();
                if (stem.size() < 10 || stem[4] != '-' || stem[7] != '-') {
                    continue;
                }
                int year = 0, month = 0, day = 0;
                try {
                    year  = std::stoi(stem.substr(0, 4));
                    month = std::stoi(stem.substr(5, 2));
                    day   = std::stoi(stem.substr(8, 2));
                } catch (...) {
                    continue;
                }
                if (year < 1970 || year > 9999 || month < 1 || month > 12 ||
                    day < 1 || day > 31) continue;

                std::chrono::year_month_day ymd{
                    std::chrono::year{year},
                    std::chrono::month{static_cast<unsigned>(month)},
                    std::chrono::day{static_cast<unsigned>(day)}};
                if (!ymd.ok()) continue;
                std::chrono::sys_days sd{ymd};
                const int64_t fileDayMs =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        sd.time_since_epoch()).count();

                if (fileDayMs < cutoff) {
                    std::error_code rmEc;
                    fs::remove(file, rmEc);
                    if (!rmEc) {
                        ++removed;
                        sLog_Data("HeatmapColumnStore: retention removed " << file);
                    } else {
                        sLog_Warning("HeatmapColumnStore: retention failed to "
                                     "remove " << file << ": " << rmEc.message());
                    }
                }
            }
        }
    }
    return removed;
}

HeatmapColumnStore::Stats HeatmapColumnStore::stats() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_stats;
}
