#include "HeatmapColumnStore.hpp"

#include "SentinelLogging.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <iomanip>
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
