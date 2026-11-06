#include "RollupStore.hpp"
#include "SessionManager.hpp"
#include "../heatmap/TimeComposer.hpp"
#include "SentinelLogging.hpp"
#include <algorithm>
#include <cstdio>
#include <stdexcept>
#if defined(__APPLE__)
#include <pthread.h>
#include <sys/qos.h>
#include <unistd.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <io.h>
#include <process.h>
#include <windows.h>
#else
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace recording {
namespace {
using heatmap::kDayMs;
using heatmap::kHourMs;
using heatmap::kMinuteMs;
constexpr uint16_t kRollupFileVersion = 1;
constexpr uint64_t kMaxRollupFrameBytes = 64u * 1024u * 1024u;

uint64_t fnv(uint64_t h, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        h ^= (v >> (8 * i)) & 0xff;
        h *= 1099511628211ull;
    }
    return h;
}
uint64_t fnv(uint64_t h, std::string_view s) {
    for (const unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return fnv(h, uint64_t(s.size()));
}
constexpr uint64_t kFnvSeed = 14695981039346656037ull;
int64_t dayFloor(int64_t ms) { return floorDiv(ms, kDayMs) * kDayMs; }

std::string dayString(int64_t dayMs) {
    int64_t y = 0;
    int m = 0, d = 0;
    SessionManager::detail::civilFromDays(floorDiv(dayMs, kDayMs), y, m, d);
    char out[16];
    std::snprintf(out, sizeof out, "%04lld-%02d-%02d", static_cast<long long>(y), m, d);
    return out;
}
std::optional<int64_t> parseDay(std::string_view name) {
    if (name.size() < 10 || name[4] != '-' || name[7] != '-') return std::nullopt;
    auto number = [&](size_t at, size_t n) -> int {
        int v = 0;
        for (size_t i = at; i < at + n; ++i) {
            if (name[i] < '0' || name[i] > '9') return -1;
            v = v * 10 + (name[i] - '0');
        }
        return v;
    };
    const int y = number(0, 4), m = number(5, 2), d = number(8, 2);
    if (y < 2000 || m < 1 || m > 12 || d < 1 || d > 31) return std::nullopt;
    return SessionManager::detail::daysFromCivil(y, m, d) * kDayMs;
}
// Size and modification time of a file; 0 when it does not exist.
uint64_t fileIdentity(const std::filesystem::path& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) return 0;
    const auto time = std::filesystem::last_write_time(path, ec);
    if (ec) return 0;
    return fnv(fnv(kFnvSeed, uint64_t(size)), uint64_t(time.time_since_epoch().count())) | 1;
}
std::vector<uint8_t> readFile(const std::filesystem::path& path, uint64_t maxBytes) {
    std::FILE* f = std::fopen(path.string().c_str(), "rb");
    if (!f) return {};
    std::vector<uint8_t> out;
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (!ec && size <= maxBytes) {
        out.resize(size_t(size));
        if (std::fread(out.data(), 1, out.size(), f) != out.size()) out.clear();
    }
    std::fclose(f);
    return out;
}
std::optional<RollupFileHeader> parseHeader(std::span<const uint8_t> bytes, uint64_t fileBytes) {
    if (bytes.size() < kRollupHeaderBytes) return std::nullopt;
    auto get = [&](size_t at, size_t n) {
        uint64_t v = 0;
        for (size_t i = 0; i < n; ++i) v |= uint64_t(bytes[at + i]) << (8 * i);
        return v;
    };
    if (bytes[0] != 'S' || bytes[1] != 'H' || bytes[2] != 'R' || bytes[3] != '1' ||
        get(4, 2) != kRollupFileVersion || get(6, 2) != 0) return std::nullopt;
    RollupFileHeader h{get(8, 8), get(16, 8)};
    if (!h.frameBytes || h.frameBytes > kMaxRollupFrameBytes || fileBytes != kRollupHeaderBytes + h.frameBytes)
        return std::nullopt;
    return h;
}
void lowerThreadPriority() {
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_BACKGROUND, 0); // CPU and I/O throttled
#elif defined(_WIN32)
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
#else
    (void)setpriority(PRIO_PROCESS, 0, 19); // the calling thread on Linux
#endif
}
uint64_t processId() {
#if defined(_WIN32)
    return uint64_t(_getpid());
#else
    return uint64_t(getpid());
#endif
}
bool isTemp(const std::filesystem::path& path) { return path.filename().string().find(".tmp-") != std::string::npos; }
std::string sourceFor(const std::string& layer) {
    const auto* source = heatmap::chunkSourceForHmc2Layer(layer);
    if (!source) throw std::invalid_argument("unknown rollup layer");
    return std::string(source->id);
}
} // namespace

DayRollup composeDay(Hmc2Reader& reader, const std::string& symbol, const std::string& layer, int64_t dayMs,
                     ReadControl& control) {
    if (dayMs % kDayMs) throw std::invalid_argument("rollup day must be a UTC day start");
    const auto source = sourceFor(layer);
    std::vector<heatmap::SparseColumns> minutes;
    minutes.reserve(24);
    // The day is closed: every minute chunk of it is committed.
    const BookRecorder::Watermarks closed{dayMs + kDayMs, 0, 0};
    for (int64_t hour = dayMs; hour < dayMs + kDayMs; hour += kHourMs)
        minutes.push_back(buildChunk(reader, {symbol, source, kMinuteMs, hour}, closed, control));
    std::vector<const heatmap::SparseColumns*> levels;
    levels.reserve(minutes.size());
    for (const auto& m : minutes) levels.push_back(&m);
    // The production compose, exactly as a view composes minute chunks
    // (tiles::composeChunks): clip to the day, inputs validated by buildChunk.
    heatmap::ComposeOptions options;
    options.startMs = dayMs;
    options.endMs = dayMs + kDayMs;
    options.trustedInputs = true;
    const std::span<const heatmap::SparseColumns* const> span(levels);
    return {heatmap::compose(span, kHourMs, options), heatmap::compose(span, kDayMs, options)};
}

std::vector<uint8_t> encodeHourFrame(const std::string& symbol, const DayRollup& rollup, int64_t dayMs) {
    heatmap::ChunkFrame frame;
    frame.key = {symbol, sourceFor(rollup.hour.layer), kHourMs, dayMs};
    frame.state = {true, dayMs + kDayMs, 0};
    frame.columns = rollup.hour;
    return heatmap::encodeChunk(frame);
}
std::vector<uint8_t> encodeDayPiece(const std::string& symbol, const DayRollup& rollup, int64_t dayMs) {
    const int64_t span = heatmap::chunkSpanMs(kDayMs);
    const int64_t chunk = heatmap::chunkStartOf(kDayMs, dayMs);
    heatmap::ChunkFrame frame;
    frame.key = {symbol, sourceFor(rollup.day.layer), kDayMs, chunk};
    frame.state = {false, dayMs + kDayMs, 1};
    frame.columns = rollup.day;
    frame.columns.startMs = chunk;
    frame.columns.endMs = chunk + span;
    return heatmap::encodeChunk(frame);
}

std::vector<uint8_t> encodeRollupFile(uint64_t sourceFingerprint, std::span<const uint8_t> frame) {
    if (frame.empty() || frame.size() > kMaxRollupFrameBytes) throw std::invalid_argument("invalid rollup frame");
    std::vector<uint8_t> out{'S', 'H', 'R', '1'};
    auto put = [&](uint64_t v, size_t n) { for (size_t i = 0; i < n; ++i) out.push_back(uint8_t(v >> (8 * i))); };
    put(kRollupFileVersion, 2);
    put(0, 2);
    put(sourceFingerprint, 8);
    put(frame.size(), 8);
    out.insert(out.end(), frame.begin(), frame.end());
    return out;
}
std::optional<RollupFileHeader> readRollupHeader(const std::filesystem::path& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size < kRollupHeaderBytes) return std::nullopt;
    std::FILE* f = std::fopen(path.string().c_str(), "rb");
    if (!f) return std::nullopt;
    uint8_t bytes[kRollupHeaderBytes];
    const bool ok = std::fread(bytes, 1, sizeof bytes, f) == sizeof bytes;
    std::fclose(f);
    return ok ? parseHeader(bytes, size) : std::nullopt;
}
std::optional<std::vector<uint8_t>> readRollupFrame(const std::filesystem::path& path, const ChunkKey& expected) {
    auto bytes = readFile(path, kRollupHeaderBytes + kMaxRollupFrameBytes);
    if (!parseHeader(bytes, bytes.size())) return std::nullopt;
    std::vector<uint8_t> frame(bytes.begin() + kRollupHeaderBytes, bytes.end());
    try {
        const auto decoded = heatmap::decodeChunk(frame);
        if (decoded.kind != heatmap::ChunkKind::Chunk || !(decoded.key == expected)) return std::nullopt;
    } catch (const std::invalid_argument&) {
        return std::nullopt;
    }
    return frame;
}

std::map<int64_t, uint64_t> minuteFingerprints(const std::filesystem::path& hmc2Root, const std::string& symbol,
                                               const std::string& layer) {
    std::map<int64_t, std::vector<std::tuple<std::string, uint64_t, int64_t>>> files;
    std::error_code ec;
    const auto dir = hmc2Root / symbol / (layer + "-60000");
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const auto name = it->path().filename().string();
        if (name.size() < 15 || name.substr(name.size() - 5) != ".hmc2" || (name[10] != '.')) continue;
        const auto day = parseDay(name);
        if (!day) continue;
        std::error_code fileError;
        const auto size = it->file_size(fileError);
        const auto time = it->last_write_time(fileError);
        if (fileError) continue;
        files[*day].emplace_back(name, uint64_t(size), int64_t(time.time_since_epoch().count()));
    }
    std::map<int64_t, uint64_t> out;
    for (auto& [day, list] : files) {
        std::sort(list.begin(), list.end());
        uint64_t h = kFnvSeed;
        for (const auto& [name, size, time] : list) h = fnv(fnv(fnv(h, name), size), uint64_t(time));
        out[day] = h | 1; // nonzero: 0 means "no minute files"
    }
    return out;
}

std::filesystem::path rollupRootFor(const std::string& configured, const std::filesystem::path& hmc2Root) {
    auto tree = hmc2Root.lexically_normal();
    if (!tree.has_filename()) tree = tree.parent_path(); // "hmc2/" -> "hmc2"
    std::filesystem::path root = configured.empty() ? std::filesystem::path(tree.string() + "-rollups")
                                                    : std::filesystem::path(configured).lexically_normal();
    const auto absolute = [](const std::filesystem::path& p) {
        std::error_code ec;
        const auto a = std::filesystem::weakly_canonical(p, ec);
        return ec ? p : a;
    };
    const auto rel = absolute(root).lexically_relative(absolute(tree));
    if (!rel.empty() && *rel.begin() != "..") return {}; // the tree itself or inside it
    return root;
}

RollupStore::RollupStore(Options options) : options_(std::move(options)) {
    if (!options_.minuteThrough) throw std::invalid_argument("rollup store needs a minute watermark");
    if (options_.root.empty() || options_.hmc2Root.empty()) throw std::invalid_argument("rollup store needs roots");
}
RollupStore::~RollupStore() { stop(); }

std::filesystem::path RollupStore::filePath(const std::string& symbol, const std::string& layer, int64_t levelMs,
                                            int64_t dayMs) const {
    return options_.root / symbol / layer / std::to_string(levelMs) / (dayString(dayMs) + ".shr");
}

void RollupStore::start() {
    if (thread_.joinable()) return;
    stopSource_ = std::stop_source{};
    thread_ = std::thread([this, stop = stopSource_.get_token()] { builderLoop(stop); });
}
void RollupStore::stop() {
    if (!thread_.joinable()) return;
    stopSource_.request_stop();
    {
        std::lock_guard lock(wakeMutex_);
        woken_ = true;
    }
    wakeCv_.notify_all();
    thread_.join();
}
void RollupStore::wake() {
    {
        std::lock_guard lock(wakeMutex_);
        woken_ = true;
    }
    wakeCv_.notify_all();
}

void RollupStore::builderLoop(std::stop_token stop) {
    sentinel::logging::setCurrentThreadName("rollup-builder");
    lowerThreadPriority();
    // A build killed between write and rename leaves a temp file; never served.
    std::error_code ec;
    uint64_t removed = 0;
    for (std::filesystem::recursive_directory_iterator it(options_.root, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file() && isTemp(it->path())) {
            std::error_code removeError;
            if (std::filesystem::remove(it->path(), removeError)) ++removed;
        }
    sLog_Data("Rollup builder started root=" << options_.root.string() << " symbols=" << options_.symbols.size()
              << " staleTempsRemoved=" << removed);
    while (!stop.stop_requested()) {
        try {
            const auto result = runPass(stop);
            if (result.built || result.failed)
                sLog_Data("Rollup pass built=" << result.built << " failed=" << result.failed
                          << " pending=" << result.pending << " stopped=" << result.stopped);
        } catch (const std::exception& e) {
            sLog_Error("Rollup pass failed error=" << e.what());
        }
        std::unique_lock lock(wakeMutex_);
        wakeCv_.wait_for(lock, options_.rescan, [&] { return woken_ || stop.stop_requested(); });
        woken_ = false;
    }
}

void RollupStore::setSymbols(std::vector<std::string> symbols) {
    std::lock_guard pass(passMutex_);
    options_.symbols = std::move(symbols);
}

RollupStore::PassResult RollupStore::runPass(std::stop_token stop) {
    std::lock_guard pass(passMutex_);
    PassResult result;
    Hmc2Reader reader(options_.hmc2Root);
    for (const auto& symbol : options_.symbols)
        for (const auto* layer : {"near", "deep"}) {
            if (stop.stop_requested()) { result.stopped = true; break; }
            try {
                passSeries(reader, symbol, layer, stop, result);
            } catch (const std::exception& e) {
                ++result.failed;
                counters_.buildFailures.fetch_add(1, std::memory_order_relaxed);
                sLog_Warning("Rollup series failed symbol=" << symbol << " layer=" << layer << " error=" << e.what());
            }
        }
    counters_.pendingDays.store(int64_t(result.pending), std::memory_order_relaxed);
    return result;
}

void RollupStore::passSeries(Hmc2Reader& reader, const std::string& symbol, const std::string& layer,
                             std::stop_token stop, PassResult& result) {
    ReadControl control;
    control.stop = stop;
    const auto available = reader.availability(symbol, layer, kMinuteMs, control);
    if (control.status != ReadStatus::Complete) {
        if (stop.stop_requested()) { result.stopped = true; return; }
        throw std::runtime_error("minute availability scan incomplete");
    }
    if (!available.oldestMs) { publish(symbol, layer, {}); return; }
    Series s{*available.oldestMs, 0, 0};
    const int64_t firstDay = dayFloor(*available.oldestMs);
    const int64_t closedEnd = dayFloor(options_.minuteThrough(symbol, layer));
    std::set<int64_t> suspects;
    {
        std::lock_guard lock(indexMutex_);
        for (auto it = suspect_.begin(); it != suspect_.end();) {
            if (std::get<0>(*it) == symbol && std::get<1>(*it) == layer) {
                suspects.insert(std::get<2>(*it));
                it = suspect_.erase(it);
            } else ++it;
        }
    }
    const auto fingerprints = minuteFingerprints(options_.hmc2Root, symbol, layer);
    auto fingerprint = [&](const std::map<int64_t, uint64_t>& fps, int64_t day) {
        const auto it = fps.find(day);
        return it == fps.end() ? uint64_t(0) : it->second;
    };
    auto current = [&](int64_t day, uint64_t fp) {
        for (const int64_t level : {kHourMs, kDayMs}) {
            const auto header = readRollupHeader(filePath(symbol, layer, level, day));
            if (!header || header->sourceFingerprint != fp) return false;
        }
        return true;
    };
    // Mid-pass, publish only growth of the advertised run (a backfill becomes
    // visible day by day); the pass's final run, which may be shorter, is
    // published when the pass completes. Re-checking current days never
    // briefly shrinks what clients see.
    const auto published = series(symbol, layer);
    auto publishGrowth = [&] {
        if (!published.runEnd || (s.runStart <= published.runStart && s.runEnd >= published.runEnd))
            publish(symbol, layer, s);
    };
    uint64_t failures = 0;
    std::string firstError;
    bool contiguous = true;
    for (int64_t day = closedEnd - kDayMs; day >= firstDay; day -= kDayMs) {
        if (stop.stop_requested()) { result.stopped = true; break; }
        const auto fp = fingerprint(fingerprints, day);
        // A day without minute files is a recorder gap: it needs no files (a
        // request composes it, all gaps, from the absent minutes at no cost).
        bool built = !fp || (!suspects.contains(day) && current(day, fp));
        if (!built && options_.buildMissing) {
            const auto started = std::chrono::steady_clock::now();
            try {
                ReadControl read;
                read.stop = stop;
                const auto rollup = composeDay(reader, symbol, layer, day, read);
                const auto hour = encodeHourFrame(symbol, rollup, day);
                const auto piece = encodeDayPiece(symbol, rollup, day);
                // A minute file that changed while it was read is retried next pass.
                if (fingerprint(minuteFingerprints(options_.hmc2Root, symbol, layer), day) != fp)
                    throw std::runtime_error("minute files changed during the build");
                writeAtomically(filePath(symbol, layer, kHourMs, day), encodeRollupFile(fp, hour));
                writeAtomically(filePath(symbol, layer, kDayMs, day), encodeRollupFile(fp, piece));
                const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - started).count();
                counters_.daysBuilt.fetch_add(1, std::memory_order_relaxed);
                counters_.buildMicros.fetch_add(uint64_t(micros), std::memory_order_relaxed);
                ++result.built;
                built = true;
                sLog_Probe("rollup.build", "symbol=" << symbol << " layer=" << layer << " day=" << dayString(day)
                           << " ms=" << micros / 1000 << " hourBytes=" << hour.size() << " dayBytes=" << piece.size());
            } catch (const std::exception& e) {
                if (stop.stop_requested()) { result.stopped = true; break; }
                ++result.failed;
                counters_.buildFailures.fetch_add(1, std::memory_order_relaxed);
                if (!failures++) firstError = dayString(day) + ": " + e.what();
            }
        }
        if (built) {
            if (!s.runEnd) s = {s.firstMinuteMs, day, day + kDayMs};
            else if (contiguous && day + kDayMs == s.runStart) s.runStart = day;
            publishGrowth();
        } else {
            ++result.pending;
            if (s.runEnd) contiguous = false;
        }
    }
    if (failures) // one line per series and pass; retried next pass
        sLog_Warning("Rollup day build failed symbol=" << symbol << " layer=" << layer << " days=" << failures
                     << " first=" << firstError);
    if (!result.stopped) publish(symbol, layer, s);
}

void RollupStore::writeAtomically(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) const {
    static std::atomic<uint64_t> counter{0};
    std::filesystem::create_directories(path.parent_path());
    auto temp = path;
    temp += ".tmp-" + std::to_string(processId()) + "-" + std::to_string(counter.fetch_add(1));
    std::FILE* f = std::fopen(temp.string().c_str(), "wb");
    if (!f) throw std::runtime_error("cannot create " + temp.string());
    bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size() && std::fflush(f) == 0;
#if defined(_WIN32)
    ok = ok && _commit(_fileno(f)) == 0;
#else
    ok = ok && fsync(fileno(f)) == 0;
#endif
    ok = std::fclose(f) == 0 && ok;
    std::error_code ec;
    try {
        if (!ok) throw std::runtime_error("cannot write " + temp.string());
        if (options_.beforeRename) options_.beforeRename(temp, path);
        std::filesystem::rename(temp, path);
    } catch (...) {
        std::filesystem::remove(temp, ec);
        throw;
    }
}

void RollupStore::publish(const std::string& symbol, const std::string& layer, const Series& s) {
    std::lock_guard lock(indexMutex_);
    auto& current = index_[{symbol, layer}];
    if (current.firstMinuteMs == s.firstMinuteMs && current.runStart == s.runStart && current.runEnd == s.runEnd)
        return;
    current = s;
    generation_.fetch_add(1, std::memory_order_acq_rel);
}
RollupStore::Series RollupStore::series(const std::string& symbol, const std::string& layer) const {
    std::lock_guard lock(indexMutex_);
    const auto it = index_.find({symbol, layer});
    return it == index_.end() ? Series{} : it->second;
}
void RollupStore::markSuspect(const std::string& symbol, const std::string& layer, int64_t dayMs) {
    {
        std::lock_guard lock(indexMutex_);
        suspect_.insert({symbol, layer, dayMs});
    }
    wake();
}

std::vector<RollupStore::LevelRange> RollupStore::levels(const std::string& symbol, const std::string& layer) const {
    const auto s = series(symbol, layer);
    if (!s.runEnd || !s.firstMinuteMs) return {};
    // Complete: the built run reaches the first recorded day, whose earlier
    // time has no minutes (a 1d chunk scans it as gaps). Otherwise a backfill
    // is in progress: 1d chunks start on a chunk boundary inside the run, so a
    // cached 1d chunk never lacks days that the run later gains.
    const bool complete = s.runStart <= dayFloor(s.firstMinuteMs);
    std::vector<LevelRange> out;
    out.push_back({kHourMs, complete ? floorDiv(s.firstMinuteMs, kHourMs) * kHourMs : s.runStart, s.runEnd});
    const int64_t span = heatmap::chunkSpanMs(kDayMs);
    const int64_t dayOldest = complete ? dayFloor(s.firstMinuteMs) : heatmap::chunkStartOf(kDayMs, s.runStart + span - 1);
    if (dayOldest < s.runEnd) out.push_back({kDayMs, dayOldest, s.runEnd});
    return out;
}

RollupStore::Reply RollupStore::serve(const ChunkKey& key, Hmc2Reader& reader) {
    try {
        (void)chunkEndMs(key);
        const auto layer = hmc2Layer(key);
        if (key.levelMs == kHourMs) return serveHour(key, layer, reader);
        if (key.levelMs == kDayMs) return serveDay(key, layer, reader);
        return {nullptr, heatmap::ChunkError::InvalidRequest, "not a rollup level"};
    } catch (const std::invalid_argument& e) {
        return {nullptr, heatmap::ChunkError::InvalidRequest, e.what()};
    } catch (const std::exception& e) {
        sLog_Error("Rollup serve failed symbol=" << key.symbol << " source=" << key.source << " level=" << key.levelMs
                   << " start=" << key.startMs << " error=" << e.what());
        return {nullptr, heatmap::ChunkError::BuildFailed, e.what()};
    }
}

RollupStore::Reply RollupStore::serveHour(const ChunkKey& key, const std::string& layer, Hmc2Reader& reader) {
    const int64_t day = key.startMs;
    if (day + kDayMs > dayFloor(options_.minuteThrough(key.symbol, layer)))
        return {nullptr, heatmap::ChunkError::Unavailable, "rollup day is not closed"};
    const auto path = filePath(key.symbol, layer, kHourMs, day);
    if (const auto identity = fileIdentity(path)) {
        if (auto hit = cached(key, identity)) return {std::move(hit)};
        if (auto frame = readRollupFrame(path, key)) {
            auto wire = std::make_shared<const std::vector<uint8_t>>(std::move(*frame));
            remember(key, identity, wire);
            return {std::move(wire)};
        }
    }
    // Missing or corrupt: the day's minutes give the same picture.
    const bool fallback = expectFile(key, layer, day);
    ReadControl control;
    const auto rollup = composeDay(reader, key.symbol, layer, day, control);
    return {std::make_shared<const std::vector<uint8_t>>(encodeHourFrame(key.symbol, rollup, day)),
            heatmap::ChunkError::BuildFailed, {}, fallback};
}

RollupStore::Reply RollupStore::serveDay(const ChunkKey& key, const std::string& layer, Hmc2Reader& reader) {
    const int64_t start = key.startMs, end = chunkEndMs(key);
    const auto s = series(key.symbol, layer);
    const int64_t firstDay = dayFloor(s.firstMinuteMs);
    const bool complete = s.runEnd && s.firstMinuteMs && s.runStart <= firstDay;
    int64_t scanFrom = s.runEnd ? (complete ? start : std::max(start, s.runStart)) : start;
    const int64_t scanTo = s.runEnd ? std::min(end, s.runEnd) : start;
    if (scanTo <= scanFrom) scanFrom = scanTo;
    // Days of the scanned range with data; earlier days of a complete series
    // precede its first minute and are scanned gaps.
    std::vector<std::pair<int64_t, uint64_t>> days;
    uint64_t identity = fnv(fnv(fnv(kFnvSeed, uint64_t(scanFrom)), uint64_t(scanTo)), uint64_t(firstDay));
    bool allPresent = true;
    for (int64_t day = std::max(scanFrom, firstDay); day < scanTo; day += kDayMs) {
        const auto id = fileIdentity(filePath(key.symbol, layer, kDayMs, day));
        allPresent = allPresent && id;
        identity = fnv(identity, id);
        days.emplace_back(day, id);
    }
    if (allPresent)
        if (auto hit = cached(key, identity)) return {std::move(hit)};
    heatmap::ChunkFrame frame;
    frame.key = key;
    frame.columns = {key.symbol, layer, kDayMs, start, end};
    if (scanTo > scanFrom) frame.columns.scannedRanges.push_back({scanFrom, scanTo});
    bool fallback = false;
    for (const auto& [day, id] : days) {
        std::optional<heatmap::SparseColumns> piece;
        if (id)
            if (const auto bytes = readRollupFrame(filePath(key.symbol, layer, kDayMs, day), key))
                piece = heatmap::decodeChunk(*bytes).columns;
        if (!piece) {
            fallback = expectFile(key, layer, day) || fallback;
            ReadControl control;
            piece = std::move(composeDay(reader, key.symbol, layer, day, control).day);
        }
        for (auto& column : piece->columns)
            if (column.bucketStartMs >= day && column.bucketStartMs < day + kDayMs)
                frame.columns.columns.push_back(std::move(column));
    }
    const bool sealed = scanFrom == start && scanTo == end;
    frame.state = {sealed, sealed ? end : s.runEnd, sealed ? 0 : 1 + uint64_t((scanTo - scanFrom) / kDayMs)};
    auto wire = std::make_shared<const std::vector<uint8_t>>(heatmap::encodeChunk(frame));
    if (allPresent && !fallback) remember(key, identity, wire);
    return {std::move(wire), heatmap::ChunkError::BuildFailed, {}, fallback};
}

bool RollupStore::expectFile(const ChunkKey& key, const std::string& layer, int64_t dayMs) {
    // A recorder-gap day (no minute files) has no rollup files by design.
    if (!minuteFingerprints(options_.hmc2Root, key.symbol, layer).contains(dayMs)) return false;
    markSuspect(key.symbol, layer, dayMs);
    counters_.fallbackServes.fetch_add(1, std::memory_order_relaxed);
    sLog_Warning("Rollup file missing or corrupt; composing minutes symbol=" << key.symbol << " layer=" << layer
                 << " level=" << key.levelMs << " day=" << dayString(dayMs));
    return true;
}

std::shared_ptr<const std::vector<uint8_t>> RollupStore::cached(const ChunkKey& key, uint64_t identity) {
    std::lock_guard lock(cacheMutex_);
    for (auto it = cache_.begin(); it != cache_.end(); ++it) {
        if (!(it->key == key)) continue;
        if (it->identity != identity) {
            cacheBytes_ -= it->wire->size();
            cache_.erase(it);
            return {};
        }
        cache_.splice(cache_.begin(), cache_, it);
        return cache_.front().wire;
    }
    return {};
}
void RollupStore::remember(const ChunkKey& key, uint64_t identity, std::shared_ptr<const std::vector<uint8_t>> wire) {
    std::lock_guard lock(cacheMutex_);
    if (wire->size() > options_.cacheBytes) return;
    for (auto it = cache_.begin(); it != cache_.end(); ++it)
        if (it->key == key) {
            cacheBytes_ -= it->wire->size();
            cache_.erase(it);
            break;
        }
    cacheBytes_ += wire->size();
    cache_.push_front({key, identity, std::move(wire)});
    while (cacheBytes_ > options_.cacheBytes) {
        cacheBytes_ -= cache_.back().wire->size();
        cache_.pop_back();
    }
}
} // namespace recording
