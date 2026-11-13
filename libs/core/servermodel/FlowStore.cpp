#include "FlowStore.hpp"
#include "RollupStore.hpp"
#include "PersistenceIo.hpp"
#include "SentinelLogging.hpp"
#include "flow/FlowBarBuilder.hpp"
#include "flow/FlowScales.hpp"
#include "Hmc2Store.hpp"
#include "roller/Grid.hpp"
#include "roller/JournalFeed.hpp"
#include <QCryptographicHash>
#include <QDateTime>
#include <QTimeZone>
#include <QFile>
#include <QSaveFile>
#include <algorithm>
#include <cmath>
#include <numeric>
#include <bit>
#include <cctype>
#include <set>
#include <unordered_set>

namespace recording {
namespace {
namespace flow = sentinel::flow;
namespace roller = sentinel::roller;
namespace fs = std::filesystem;
constexpr int64_t Minute = 60'000, Hour = 60 * Minute, Day = 24 * Hour;
// Same receive-time tail as JournalTradeEventSource. Buckets use exchange time.
constexpr int64_t TradeTail = Minute;
constexpr qint64 MaxDayBytes = 64 * 1024 * 1024;
constexpr uint8_t ScaleRuleVersion = 2;
using Fingerprint = std::array<uint8_t, 32>;
struct StoredDay {
    flow::DayFile day;
    uint32_t hours = 0, revision = 1;
    FlowStoreGridValues grid;
    nlohmann::json sources;
};
struct Cancelled : std::exception {
    const char *what() const noexcept override { return "flow replay cancelled"; }
};
void checkStop(std::stop_token stop) {
    if (stop.stop_requested())
        throw Cancelled{};
}
int64_t dayFloor(int64_t ms) { return ms / Day * Day; }
QByteArray bytesOf(const flow::Bytes &b) {
    return QByteArray::fromRawData(reinterpret_cast<const char *>(b.data()), qsizetype(b.size()));
}
std::span<const uint8_t> spanOf(const QByteArray &b) {
    return {reinterpret_cast<const uint8_t *>(b.constData()), size_t(b.size())};
}
QByteArray readFile(const fs::path &path, qint64 limit) {
    if (fs::is_symlink(fs::symlink_status(path)))
        throw std::runtime_error("flow file is a symlink");
    QFile f(QString::fromStdString(path.string()));
    if (!f.open(QIODevice::ReadOnly) || f.size() <= 0 || f.size() > limit)
        throw std::runtime_error("missing or oversized flow file");
    auto bytes = f.readAll();
    if (bytes.size() != f.size())
        throw std::runtime_error("short flow read");
    return bytes;
}
// A closed file uses the same name/size/mtime identity as the rollup store.
// The growing tail uses a content hash of complete blocks through the receive
// fence. Appending beyond that fence must not invalidate already sealed bars.
using Sources = nlohmann::json;
struct NotReady {};
int64_t sourceCutoff(int64_t day, uint32_t hours) {
    for (int h = 23; h >= 0; --h)
        if (hours & (1u << h))
            return day + (h + 1) * Hour + TradeTail;
    throw std::runtime_error("empty flow completion mask");
}
size_t sourceCount(const roller::JournalReader &catalog, int64_t cutoff) {
    size_t end = 0;
    for (size_t i = 0; i < catalog.files().size(); ++i)
        if (catalog.files()[i].header.at("opened_system_ns").get<int64_t>() / 1'000'000 <= cutoff)
            end = i + 1;
    return end;
}
std::string hashPrefix(const fs::path &path, uint64_t bytes, std::stop_token stop = {}) {
    QFile file(QString::fromStdString(path.string()));
    if (!file.open(QIODevice::ReadOnly) || uint64_t(file.size()) < bytes)
        throw std::runtime_error("flow journal prefix unavailable");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    std::array<char, 64 * 1024> buffer{};
    while (bytes) {
        checkStop(stop);
        const auto count = file.read(buffer.data(), qint64(std::min<uint64_t>(bytes, buffer.size())));
        if (count <= 0)
            throw std::runtime_error("short flow journal prefix read");
        hash.addData(QByteArrayView(buffer.data(), count));
        bytes -= uint64_t(count);
    }
    return hash.result().toHex().toStdString();
}
// Use the production RAWL2 reader's validated byte extent, never a second parser.
// Finish the fence's entire block; the next record exposes its end in validBytes.
uint64_t tailExtent(const roller::JournalFile &file, int64_t cutoff, std::stop_token stop) {
    sentinel::capture::RecordReader reader(QString::fromStdString(file.path.string()), true);
    sentinel::capture::Record record;
    std::optional<uint64_t> fenceBlock;
    while (reader.next(record)) {
        checkStop(stop);
        if (fenceBlock && reader.result().recordOrdinal != *fenceBlock)
            return reader.result().validBytes;
        if (!fenceBlock && record.time.systemNs / 1'000'000 > cutoff)
            fenceBlock = reader.result().recordOrdinal;
    }
    if (!fenceBlock && !reader.result().indexed)
        throw NotReady{};
    return reader.result().validBytes;
}
Sources snapshotSources(const roller::JournalReader &catalog, int64_t cutoff, const std::string &begin,
                        std::stop_token stop) {
    Sources sources = Sources::array();
    const auto end = sourceCount(catalog, cutoff);
    const auto first = std::find_if(catalog.files().begin(), catalog.files().end(),
                                    [&](const auto &file) { return file.path.generic_string() == begin; });
    if (first == catalog.files().end())
        throw std::runtime_error("flow source anchor disappeared");
    for (size_t i = size_t(first - catalog.files().begin()); i < end; ++i) {
        checkStop(stop);
        const auto &file = catalog.files()[i];
        Sources identity = {{"path", file.path.generic_string()},
                            {"size", fs::file_size(file.path)},
                            {"mtime", fs::last_write_time(file.path).time_since_epoch().count()}};
        if (!file.superseded) {
            const auto extent = tailExtent(file, cutoff, stop);
            identity["prefix"] = extent;
            identity["hash"] = hashPrefix(file.path, extent, stop);
        }
        sources.push_back(std::move(identity));
    }
    return sources;
}
bool sourcesMatch(const Sources &sources, const roller::JournalReader &catalog, int64_t cutoff,
                  std::stop_token stop = {}) {
    const auto end = sourceCount(catalog, cutoff);
    if (!sources.is_array() || sources.empty())
        return false;
    const auto first = std::find_if(catalog.files().begin(), catalog.files().end(), [&](const auto &file) {
        return sources.front().at("path") == file.path.generic_string();
    });
    if (first == catalog.files().end())
        return false;
    const auto begin = size_t(first - catalog.files().begin());
    if (begin >= end || sources.size() != end - begin)
        return false;
    for (size_t i = begin; i < end; ++i) {
        checkStop(stop);
        const auto &file = catalog.files()[i];
        const auto &saved = sources.at(i - begin);
        if (saved.at("path") != file.path.generic_string())
            return false;
        const auto size = fs::file_size(file.path);
        const auto mtime = fs::last_write_time(file.path).time_since_epoch().count();
        if (size == saved.at("size") && mtime == saved.at("mtime"))
            continue;
        // A changed fixed file or same-size rewrite invalidates the day. A tail
        // may only grow while retaining the exact prefix used for aggregation.
        if (!saved.contains("prefix") || size <= saved.at("size").get<uint64_t>() ||
            hashPrefix(file.path, saved.at("prefix").get<uint64_t>(), stop) != saved.at("hash").get<std::string>())
            return false;
    }
    return true;
}
Fingerprint fingerprint(const Sources &sources) {
    const auto digest = QCryptographicHash::hash(QByteArray::fromStdString(sources.dump()), QCryptographicHash::Sha256);
    Fingerprint out{};
    std::copy_n(reinterpret_cast<const uint8_t *>(digest.constData()), out.size(), out.begin());
    return out;
}
std::map<int64_t, fs::path> hmcDayPaths(const FlowStore::Options &options, const std::string &symbol,
                                        std::stop_token stop) {
    const auto dir = options.hmc2Root / symbol / "near-60000";
    std::map<int64_t, std::pair<uint32_t, fs::path>> selected;
    std::error_code ec;
    if (fs::is_directory(dir, ec))
        for (const auto &entry : fs::directory_iterator(dir)) {
            checkStop(stop);
            const auto filename = entry.path().filename().string();
            if (!entry.is_regular_file() || entry.path().extension() != ".hmc2" || filename.size() < 15)
                continue;
            const auto date = QDate::fromString(QString::fromStdString(filename.substr(0, 10)), "yyyy-MM-dd");
            if (!date.isValid())
                continue;
            uint32_t generation = 0;
            const auto suffix = filename.substr(10);
            if (suffix != ".hmc2") {
                if (!suffix.starts_with(".g") || !suffix.ends_with(".hmc2"))
                    continue;
                const auto number = suffix.substr(2, suffix.size() - 7);
                if (number.empty() || !std::all_of(number.begin(), number.end(), ::isdigit))
                    continue;
                try {
                    const auto parsed = std::stoull(number);
                    if (parsed > UINT32_MAX)
                        continue;
                    generation = uint32_t(parsed);
                } catch (const std::exception &) {
                    continue;
                }
            }
            const auto day = date.startOfDay(QTimeZone::UTC).toMSecsSinceEpoch();
            auto &best = selected[day];
            if (best.second.empty() || generation > best.first)
                best = {generation, entry.path()};
        }
    std::map<int64_t, fs::path> result;
    for (auto &[day, best] : selected)
        result.emplace(day, std::move(best.second));
    return result;
}
void appendWord(QByteArray &bytes, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i)
        bytes.append(char(value >> (i * 8)));
}
uint64_t readWord(const QByteArray &bytes, size_t at) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i)
        value |= uint64_t(uint8_t(bytes[qsizetype(at + i)])) << (i * 8);
    return value;
}
void appendGrid(QByteArray &bytes, const FlowStoreGridValues &grid) {
    appendWord(bytes, std::bit_cast<uint64_t>(grid.scales.nativeTick));
    appendWord(bytes, uint64_t(grid.scales.rowTickUnits));
    appendWord(bytes, uint64_t(grid.scales.priceScale));
    appendWord(bytes, uint64_t(grid.scales.baseScale));
    appendWord(bytes, uint64_t(grid.scales.quoteScale));
    appendWord(bytes, std::bit_cast<uint64_t>(grid.sizeFloor));
    appendWord(bytes, grid.configHash);
    appendWord(bytes, grid.hasHmc2 ? 1 : 0);
}
FlowStoreGridValues readGrid(const QByteArray &bytes) {
    FlowStoreGridValues grid;
    grid.scales.nativeTick = std::bit_cast<double>(readWord(bytes, 13));
    grid.scales.rowTickUnits = int64_t(readWord(bytes, 21));
    grid.scales.priceScale = int64_t(readWord(bytes, 29));
    grid.scales.baseScale = int64_t(readWord(bytes, 37));
    grid.scales.quoteScale = int64_t(readWord(bytes, 45));
    grid.sizeFloor = std::bit_cast<double>(readWord(bytes, 53));
    grid.configHash = readWord(bytes, 61);
    grid.hasHmc2 = readWord(bytes, 69) != 0;
    return grid;
}
QByteArray completion(const QByteArray &bytes, uint32_t hours, uint32_t revision, const FlowStoreGridValues &grid,
                      const std::string &sources) {
    QByteArray state("FLS1", 4);
    state.append(char(ScaleRuleVersion));
    for (unsigned i = 0; i < 4; ++i)
        state.append(char(hours >> (i * 8)));
    for (unsigned i = 0; i < 4; ++i)
        state.append(char(revision >> (i * 8)));
    appendGrid(state, grid);
    state.append(QByteArray::fromStdString(sources));
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(state);
    hash.addData(bytes);
    state.append(hash.result());
    return state;
}
std::optional<StoredDay> loadDay(const fs::path &path, const std::string &symbol, int64_t day) {
    try {
        const auto bytes = readFile(path, MaxDayBytes);
        const auto state = readFile(path.string() + ".state", MaxDayBytes);
        if (state.size() < 111 || uint8_t(state[4]) != ScaleRuleVersion)
            return {};
        uint32_t hours = 0, revision = 0;
        for (unsigned i = 0; i < 4; ++i) {
            hours |= uint32_t(uint8_t(state[5 + i])) << (i * 8);
            revision |= uint32_t(uint8_t(state[9 + i])) << (i * 8);
        }
        const auto grid = readGrid(state);
        const auto sourceText = state.mid(77, state.size() - 109).toStdString();
        if (!hours || hours >> 24 || !revision || state != completion(bytes, hours, revision, grid, sourceText))
            return {};
        auto sources = Sources::parse(sourceText);
        auto decoded = flow::decodeFlowDay(spanOf(bytes));
        if (decoded.product != symbol || decoded.dayStartMs != day || decoded.timeframeMs != Minute ||
            decoded.bars.size() > 1440 || decoded.sourceFingerprint != fingerprint(sources))
            return {};
        for (const auto &bar : decoded.bars)
            if (!(hours & (1u << ((bar.startMs - day) / Hour))))
                return {};
        return StoredDay{std::move(decoded), hours, revision, grid, std::move(sources)};
    } catch (const std::exception &) {
        return {};
    }
}
// A journal start is the first durable record in the last segment opened no
// later than the run's warmup fence. The reader applies every frame from there.
std::optional<roller::JournalPos> firstDurable(const roller::JournalReader &catalog, int64_t before,
                                               std::stop_token stop) {
    const auto &files = catalog.files();
    size_t candidate = 0;
    for (size_t i = 0; i < files.size(); ++i)
        if (files[i].header.at("opened_system_ns").get<int64_t>() / 1'000'000 <= before)
            candidate = i;
    for (size_t i = candidate; i < files.size(); ++i) {
        checkStop(stop);
        const auto &file = files[i];
        sentinel::capture::RecordReader probe(QString::fromStdString(file.path.string()), !file.superseded, {},
                                              [&](const sentinel::capture::BlockIndex &block, const char *reason) {
                                                  sLog_Warning("Flow start skipped block file="
                                                               << file.path.string() << " offset=" << block.offset
                                                               << " reason=" << reason);
                                              });
        sentinel::capture::Record record;
        if (!probe.next(record))
            continue;
        checkStop(stop);
        return roller::JournalPos{file.header.at("product_metadata").at("product_id").get<std::string>(),
                                  file.header.at("run_id"), probe.result().recordOrdinal, probe.result().recordIndex};
    }
    return {};
}
std::string sourcePath(const roller::JournalReader &catalog, const roller::JournalPos &start) {
    std::string path;
    for (const auto &file : catalog.files())
        if (file.header.at("run_id") == start.run &&
            file.header.at("first_block_ordinal").get<uint64_t>() <= start.block)
            path = file.path.generic_string();
    if (path.empty())
        throw std::runtime_error("flow replay cursor has no segment");
    return path;
}
uint32_t revisionHint(const fs::path &path) {
    QFile file(QString::fromStdString(path.string()));
    if (!file.open(QIODevice::ReadOnly))
        return 0;
    const auto header = file.read(13);
    if (header.size() != 13 || header.left(4) != "FLS1")
        return 0;
    uint32_t revision = 0;
    for (unsigned i = 0; i < 4; ++i)
        revision |= uint32_t(uint8_t(header[9 + i])) << (8 * i);
    return revision;
}
FlowStoreGridValues dayScales(const FlowStore::Options &options, const std::string &symbol, int64_t day,
                             const fs::path &hmcPath, const roller::JournalReader &catalog,
                             std::stop_token stop) {
    nlohmann::json metadata;
    for (const auto &file : catalog.files()) {
        if (file.header.at("opened_system_ns").get<int64_t>() / 1'000'000 <= day || metadata.is_null())
            metadata = file.header.at("product_metadata");
    }
    FlowStoreGridValues values;
    roller::Grid grid;
    if (!hmcPath.empty()) {
        Hmc2Reader reader(options.hmc2Root);
        ReadControl control;
        control.stop = stop;
        struct Found {};
        std::optional<Hmc2Header> header;
        try {
            reader.visit(
                symbol, "near", Minute, day, day + Day,
                [&](const Hmc2Record &record) {
                    header = record.header;
                    throw Found{};
                },
                control);
        } catch (const Found &) {
        }
        checkStop(stop);
        if (!header || control.status == ReadStatus::IoError)
            throw std::runtime_error("HMC2 day exists without a readable near grid");
        grid.priceScale = header->priceScale;
        grid.nearTick = double(header->rowTickUnits) / header->priceScale;
        grid.sizeFloor = header->sizeScale.floor;
        values.configHash = header->configHash;
        values.hasHmc2 = true;
    } else {
        sLog_Warning("Flow grid using journal fallback symbol=" << symbol << " day=" << day);
        const auto start = firstDurable(catalog, day, stop);
        if (!start)
            throw std::runtime_error("flow grid fallback has no journal record");
        roller::JournalReader reader(catalog, start);
        roller::JournalFeed feed(symbol);
        roller::JournalRecord input;
        bool ready = false;
        feed.onSnapshot = [&](int64_t, int64_t, std::vector<Level> levels) {
            double bid = 0, ask = INFINITY;
            for (const auto &level : levels)
                if (level.size > 0) {
                    if (level.isBid)
                        bid = std::max(bid, level.price);
                    else
                        ask = std::min(ask, level.price);
                }
            if (bid > 0 && std::isfinite(ask)) {
                grid = roller::deriveGrid(input.metadata, std::midpoint(bid, ask));
                ready = true;
            }
        };
        while (!ready && reader.next(input)) {
            checkStop(stop);
            if (input.record.time.systemNs / 1'000'000 >= day + Day)
                break;
            feed.apply(input);
            if (options.afterRecordForTest)
                options.afterRecordForTest(stop);
        }
        if (!ready)
            throw std::runtime_error("flow grid fallback found no two-sided snapshot");
    }
    values.scales = flow::flowScales(grid, metadata);
    values.sizeFloor = grid.sizeFloor;
    return values;
}
std::vector<flow::FlowBar> buildRun(const FlowStore::Options &options, const std::string &symbol,
                                    const flow::FlowScales &scales, int64_t first, int64_t end,
                                    const roller::JournalPos &start, std::stop_token stop,
                                    int64_t timeframe = Minute, bool *partial = nullptr,
                                    const roller::JournalReader *catalog = nullptr) {
    checkStop(stop);
    roller::JournalReader reader = catalog ? roller::JournalReader(*catalog, start) :
        roller::JournalReader(options.journalRoot, symbol, start);
    roller::JournalFeed feed(symbol);
    flow::FlowBarBuilder builder(timeframe, scales.nativeTick, scales.priceScale, scales.baseScale, scales.quoteScale);
    int64_t firstReceive = INT64_MAX, lastReceive = 0;
    bool coverageLost = false;
    if (partial) {
        feed.onInvalid = [&](int64_t ms, const std::string&) {
            if (ms > first && ms < end + TradeTail) coverageLost = true;
        };
    }
    flow::Trade trade;
    trade.id.reserve(UINT16_MAX);
    feed.onTrade = [&](const ::Trade &value) {
        const auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(value.timestamp.time_since_epoch()).count();
        if (ms < first || ms >= end)
            return;
        if (value.trade_id.size() > UINT16_MAX)
            throw std::runtime_error("flow trade id exceeds codec limit");
        trade.id.assign(value.trade_id);
        trade.exchangeMs = ms;
        trade.price = value.price;
        trade.size = value.size;
        trade.side = value.side == AggressorSide::Buy    ? flow::Side::Buy
                     : value.side == AggressorSide::Sell ? flow::Side::Sell
                                                         : flow::Side::Unknown;
        builder.add(trade);
    };
    roller::JournalRecord input;
    while (reader.next(input)) {
        checkStop(stop);
        const auto receiveMs = input.record.time.systemNs / 1'000'000;
        firstReceive = std::min(firstReceive, receiveMs);
        lastReceive = std::max(lastReceive, receiveMs);
        if (receiveMs > end + TradeTail)
            break;
        feed.apply(input);
        if (options.afterRecordForTest)
            options.afterRecordForTest(stop);
    }
    checkStop(stop);
    if (reader.damaged())
        throw std::runtime_error("damaged journal in flow run");
    if (builder.rejectedCount())
        sLog_Warning("Flow rejected trades symbol=" << symbol << " start=" << first
                                                    << " count=" << builder.rejectedCount());
    if (partial) *partial = builder.rejectedCount() != 0 || coverageLost ||
                            firstReceive > first || lastReceive < end + TradeTail;
    return builder.bars();
}
void atomicFile(const fs::path &path, const QByteArray &bytes) {
    QSaveFile f(QString::fromStdString(path.string()));
    f.setDirectWriteFallback(false);
    int error = 0;
    if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size() || !f.flush())
        throw std::runtime_error("cannot write flow temporary file");
#ifndef _WIN32
    if (!sentinel::persistence::syncFileDescriptor(f.handle(), error))
        throw std::runtime_error("cannot sync flow temporary file");
#endif
    if (!f.commit())
        throw std::runtime_error("cannot rename flow temporary file");
#ifdef _WIN32
    if (!sentinel::persistence::syncFilePath(path, error))
        throw std::runtime_error("cannot sync flow file");
#endif
    if (!sentinel::persistence::syncDirectory(path.parent_path(), error))
        throw std::runtime_error("cannot sync flow directory");
}
} // namespace

fs::path flowRootFor(const std::string &configured, const fs::path &hmc2Root, const fs::path &journalRoot,
                     const fs::path &rollupRoot, const std::vector<fs::path> &protectedRoots) {
    if (hmc2Root.empty() || journalRoot.empty())
        return {};
    try {
        auto tree = fs::weakly_canonical(fs::absolute(hmc2Root));
        if (!tree.has_filename())
            tree = tree.parent_path();
        const auto configuredRoot = configured.empty() ? tree.string() + "-flow" : configured;
        const auto root = rollupRootFor(configuredRoot, hmc2Root);
        if (root.empty() || rollupRootFor(root.string(), journalRoot).empty())
            return {};
        const auto rollup = rollupRoot.empty() ? rollupRootFor("", hmc2Root) : rollupRoot;
        if (rollup.empty() || rollupRootFor(root.string(), rollup).empty())
            return {};
        for (const auto &protectedRoot : protectedRoots)
            if (!protectedRoot.empty() && rollupRootFor(root.string(), protectedRoot).empty())
                return {};
        return root;
    } catch (const fs::filesystem_error &) {
        return {};
    }
}
FlowStore::FlowStore(Options options) : options_(std::move(options)) {
    options_.root = flowRootFor(options_.root.string(), options_.hmc2Root, options_.journalRoot, options_.rollupRoot,
                                options_.protectedRoots);
    if (options_.root.empty() || !options_.hourThrough || options_.rescan.count() <= 0)
        throw std::invalid_argument("flow store needs disjoint roots and an hour watermark");
    for (const auto &symbol : options_.symbols)
        sentinel::capture::validateSymbol(symbol);
}
FlowStore::~FlowStore() { stop(); }
void FlowStore::start() {
    if (thread_.joinable())
        return;
    stopSource_ = std::stop_source{};
    thread_ = std::thread([this, stop = stopSource_.get_token()] { builderLoop(stop); });
}
void FlowStore::stop() {
    stopSource_.request_stop();
    wake();
    if (thread_.joinable())
        thread_.join();
}
void FlowStore::wake() {
    if (options_.afterWakeForTest) options_.afterWakeForTest();
    {
        std::lock_guard lock(wakeMutex_);
        woken_ = true;
    }
    wakeCv_.notify_all();
}
void FlowStore::builderLoop(std::stop_token stop) {
    try {
        sentinel::logging::setCurrentThreadName("flow-builder");
        const bool lowered = !options_.lowerPriority || lowerThreadPriority();
        if (!lowered)
            sLog_Warning("Flow builder could not lower thread priority");
        sLog_Data("Flow builder started root=" << options_.root.string() << " symbols=" << options_.symbols.size());
        while (!stop.stop_requested()) {
            try {
                runPass(stop);
            } catch (const std::exception &e) {
                sLog_Error("Flow pass failed error=" << e.what());
            } catch (...) {
                sLog_Error("Flow pass failed unknown exception");
            }
            std::unique_lock lock(wakeMutex_);
            wakeCv_.wait_for(lock, options_.rescan, [&] { return woken_ || stop.stop_requested(); });
            woken_ = false;
        }
        sLog_Data("Flow builder stopped");
    } catch (...) {
        sLog_Error("Flow builder terminated unexpectedly");
    }
}
fs::path FlowStore::filePath(const std::string &symbol, int64_t day) const {
    sentinel::capture::validateSymbol(symbol);
    if (day < 0 || day % Day)
        throw std::invalid_argument("invalid flow day");
    return options_.root / symbol / "60000" /
           (QDateTime::fromMSecsSinceEpoch(day, QTimeZone::UTC).toString("yyyy-MM-dd").toStdString() + ".flb");
}
bool FlowStore::ownedPath(const fs::path &path) const {
    const auto relative = path.lexically_relative(options_.root);
    if (relative.empty() || *relative.begin() == "..")
        return false;
    // Recheck canonical roots as well as every descendant before reads/writes.
    if (flowRootFor(options_.root.string(), options_.hmc2Root, options_.journalRoot, options_.rollupRoot,
                    options_.protectedRoots) != options_.root)
        return false;
    fs::path at = options_.root;
    if (fs::is_symlink(fs::symlink_status(at)))
        return false;
    for (const auto &part : relative) {
        at /= part;
        if (fs::is_symlink(fs::symlink_status(at)))
            return false;
    }
    return true;
}
FlowStore::FileIdentity FlowStore::stamp(const fs::path &path) {
    std::error_code ec;
    const auto status = fs::symlink_status(path, ec);
    if (ec == std::errc::no_such_file_or_directory || status.type() == fs::file_type::not_found)
        return {};
    if (ec || !fs::is_regular_file(status))
        throw std::runtime_error("invalid flow file: " + path.string());
    FileIdentity result;
    result.present = true;
    result.size = fs::file_size(path);
    result.modified = fs::last_write_time(path);
    return result;
}
FlowStore::DayIndex FlowStore::loadIndex(const std::string &symbol, int64_t day) const {
    const auto path = filePath(symbol, day);
    if (!ownedPath(path) || !ownedPath(path.string() + ".state"))
        throw std::runtime_error("unsafe flow path");
    DayIndex result;
    result.file = stamp(path);
    result.state = stamp(path.string() + ".state");
    if (auto stored = loadDay(path, symbol, day)) {
        result.hours = stored->hours;
        result.revision = stored->revision;
        result.sources = stored->sources.dump();
        result.fingerprint = stored->day.sourceFingerprint;
        result.grid = stored->grid;
        result.valid = true;
        for (const auto &source : stored->sources)
            result.sourcePaths.emplace_back(source.at("path").get<std::string>());
    } else if (result.state.present) {
        result.revision = revisionHint(path.string() + ".state");
    }
    return result;
}
FlowStore::DayIndex FlowStore::indexFor(const std::string &symbol, int64_t day) {
    const auto key = std::pair{symbol, day};
    {
        std::lock_guard lock(indexMutex_);
        if (auto it = index_.find(key); it != index_.end())
            return it->second;
    }
    auto loaded = loadIndex(symbol, day);
    std::lock_guard lock(indexMutex_);
    return index_.try_emplace(key, std::move(loaded)).first->second;
}
void FlowStore::publishIndex(const std::string &symbol, int64_t day, const DayIndex &index) {
    {
        std::lock_guard lock(indexMutex_);
        index_[{symbol, day}] = index;
    }
    if (index.valid)
        for (int h = 0; h < 24; ++h)
            if (index.hours & (1u << h)) buildFailures_.erase({symbol, day + h * Hour});
    refreshBuiltThrough(symbol);
}
void FlowStore::setLiveRetentionFloor(const std::string &symbol, int64_t hourMs) {
    sentinel::capture::validateSymbol(symbol);
    if (hourMs < 0 || hourMs % Hour) throw std::invalid_argument("invalid live retention floor");
    std::lock_guard lock(indexMutex_);
    liveRetentionFloor_[symbol] = hourMs;
}
void FlowStore::refreshBuiltThrough(const std::string &symbol) {
    const auto first = firstHour_.find(symbol);
    if (first == firstHour_.end()) return;
    const auto through = options_.hourThrough(symbol) / Hour * Hour;
    int64_t floor = std::max<int64_t>(0, through - 6 * Hour);
    {
        std::lock_guard lock(indexMutex_);
        if (const auto it = liveRetentionFloor_.find(symbol); it != liveRetentionFloor_.end()) floor = it->second;
    }
    auto built = std::max(first->second, floor);
    for (auto hour = built; hour < through; hour += Hour) {
        const auto index = indexFor(symbol, dayFloor(hour));
        if (!index.valid || !(index.hours & (1u << ((hour - dayFloor(hour)) / Hour)))) break;
        built = hour + Hour;
    }
    std::lock_guard lock(indexMutex_);
    builtThrough_[symbol] = std::min(built, through);
}
void FlowStore::noteBuildFailure(const std::string &symbol, int64_t hour, const std::string &reason) {
    const auto count = ++buildFailures_[{symbol, hour}];
    if (count == 1 || count % 10 == 0)
        sLog_Warning("Flow hour build failed symbol=" << symbol << " hour=" << hour
                     << " count=" << count << " reason=" << reason);
}
fs::path FlowStore::journalDirectory(const std::string &symbol) {
    const auto dir = options_.journalRoot / symbol;
    const bool present = fs::is_directory(dir);
    std::lock_guard lock(indexMutex_);
    if (present)
        journalProductDirectories_.insert(symbol);
    else if (journalProductDirectories_.contains(symbol)) {
        sourceDays_.erase(symbol);
        sourceKnownBeforeDay_.erase(symbol);
        throw std::runtime_error("previously inventoried journal product directory unavailable: " + dir.string());
    }
    return present ? dir : options_.journalRoot;
}
std::map<fs::path, FlowStore::FileIdentity> FlowStore::inventory(const std::string &symbol,
                                                                 std::stop_token stop) {
    const auto dir = journalDirectory(symbol);
    std::map<fs::path, FileIdentity> result;
    for (fs::recursive_directory_iterator it(dir), end; it != end; ++it) {
        checkStop(stop);
        if (it->path().extension() == ".rawl2" && it->is_regular_file())
            result.emplace(it->path(), stamp(it->path()));
        if (options_.afterInventoryFileForTest)
            options_.afterInventoryFileForTest(stop);
    }
    return result;
}
void FlowStore::writeDay(const flow::DayFile &day, uint32_t hours, uint32_t revision,
                         const FlowStoreGridValues &grid,
                         const std::string &sources,
                         std::stop_token stop) {
    const auto path = filePath(day.product, day.dayStartMs);
    if (!ownedPath(path) || !ownedPath(path.string() + ".state"))
        throw std::runtime_error("unsafe flow path");
    std::vector<fs::path> missing;
    for (auto at = path.parent_path(); !fs::exists(at); at = at.parent_path())
        missing.push_back(at);
    int error = 0;
    for (auto it = missing.rbegin(); it != missing.rend(); ++it) {
        fs::create_directory(*it);
        if (!sentinel::persistence::syncDirectory(it->parent_path(), error))
            throw std::runtime_error("flow mkdir sync failed");
    }
    const auto encoded = flow::encodeFlowDay(day);
    if (encoded.size() > size_t(MaxDayBytes))
        throw std::runtime_error("flow day exceeds reader limit");
    const auto bytes = bytesOf(encoded);
    if (options_.beforeCommitForTest)
        options_.beforeCommitForTest(path);
    checkStop(stop);
    if (!ownedPath(path) || !ownedPath(path.string() + ".state"))
        throw std::runtime_error("unsafe flow path");
    atomicFile(path, bytes);
    // A crash between renames leaves a mismatched digest: readers refuse it and rebuild.
    atomicFile(path.string() + ".state", completion(bytes, hours, revision, grid, sources));
}
FlowStore::PassResult FlowStore::runPass(std::stop_token stop) {
    std::lock_guard lock(passMutex_);
    PassResult result;
    for (const auto &symbol : options_.symbols) {
        try {
            checkStop(stop);
            passSymbol(symbol, stop, result);
        } catch (const Cancelled &) {
            result.stopped = true;
            break;
        } catch (const roller::NoJournalFiles &) { /* A product may not have captured history yet. */
        } catch (const std::exception &e) {
            ++result.failed;
            sLog_Warning("Flow build failed symbol=" << symbol << " error=" << e.what());
        }
    }
    result.stopped = result.stopped || stop.stop_requested();
    return result;
}
std::shared_ptr<roller::JournalReader> FlowStore::catalogFor(const std::string &symbol, std::stop_token stop) {
    checkStop(stop);
    std::lock_guard lock(catalogMutex_);
    journalDirectory(symbol);
    auto &cached = catalogs_[symbol];
    if (cached) {
        std::lock_guard indexLock(indexMutex_);
        if (sourceDays_.contains(symbol)) return cached;
    }
    const auto knownBefore = dayFloor(options_.nowMs());
    std::map<int64_t, int64_t> days;
    try {
        auto catalog = std::make_shared<roller::JournalReader>(options_.journalRoot, symbol);
        for (const auto &file : catalog->files()) {
            const auto opened = file.header.at("opened_system_ns").get<int64_t>() / 1'000'000;
            auto [it, added] = days.try_emplace(dayFloor(opened), opened);
            if (!added) it->second = std::min(it->second, opened);
        }
        if (options_.afterCatalogConstructForTest) options_.afterCatalogConstructForTest();
        cached = std::move(catalog);
    } catch (const roller::NoJournalFiles &) {
        // JournalReader may fall back to the root if the product directory
        // disappears during construction. That is not an empty inventory.
        journalDirectory(symbol);
        std::lock_guard indexLock(indexMutex_);
        sourceDays_[symbol] = {};
        sourceKnownBeforeDay_[symbol] = knownBefore;
        throw;
    }
    std::lock_guard indexLock(indexMutex_);
    sourceDays_[symbol] = std::move(days);
    sourceKnownBeforeDay_[symbol] = knownBefore;
    return cached;
}
FlowStore::SourceState FlowStore::sourceState(const std::string &symbol, int64_t from, int64_t to) const {
    const auto day = dayFloor(from);
    // Never make a terminal claim for today's growing journal (or the future).
    if (day >= dayFloor(options_.nowMs())) return SourceState::Unknown;
    std::lock_guard lock(indexMutex_);
    const auto known = sourceDays_.find(symbol);
    if (known == sourceDays_.end() || day >= sourceKnownBeforeDay_.at(symbol)) return SourceState::Unknown;
    if (const auto first = known->second.find(day); first != known->second.end()) {
        // A leading hour cannot be filled by a later first segment. Require an
        // absent previous day too, so older replay/warmup sources stay possible.
        const auto hourEnd = ((to - 1) / Hour + 1) * Hour;
        if (!known->second.contains(day - Day) && hourEnd + TradeTail < first->second)
            return SourceState::Absent;
        return SourceState::Present;
    }
    // Trades use exchange time. The next day's first segment can hold late
    // trades for a window ending near midnight, within the replay's receive tail.
    const auto next = known->second.find(day + Day);
    return next == known->second.end() || next->second > to + TradeTail ?
        SourceState::Absent : SourceState::ReceiveTailOnly;
}
void FlowStore::passSymbol(const std::string &symbol, std::stop_token stop, PassResult &result) {
    const auto through = options_.hourThrough(symbol) / Hour * Hour;
    const auto knownBefore = dayFloor(options_.nowMs());
    std::map<fs::path, FileIdentity> currentFiles;
    try { currentFiles = inventory(symbol, stop); }
    catch (...) {
        // A failed inventory cannot renew a negative source claim.
        std::lock_guard catalogLock(catalogMutex_);
        catalogs_.erase(symbol);
        std::lock_guard indexLock(indexMutex_);
        sourceDays_.erase(symbol);
        sourceKnownBeforeDay_.erase(symbol);
        throw;
    }
    std::vector<fs::path> changed;
    std::set<fs::path> added;
    bool structuralChanged = false;
    auto &prior = journalInventory_[symbol];
    for (const auto &[path, identity] : currentFiles) {
        const auto it = prior.find(path);
        if (it == prior.end()) {
            changed.push_back(path);
            added.insert(path);
            structuralChanged = true;
        } else if (it->second != identity)
            changed.push_back(path);
    }
    for (const auto &[path, _] : prior)
        if (!currentFiles.contains(path)) {
            changed.push_back(path);
            structuralChanged = true;
        }
    prior = currentFiles;
    if (structuralChanged) {
        std::lock_guard catalogLock(catalogMutex_);
        catalogs_.erase(symbol);
        std::lock_guard indexLock(indexMutex_);
        sourceDays_.erase(symbol);
        sourceKnownBeforeDay_.erase(symbol);
    }
    std::shared_ptr<roller::JournalReader> catalog;
    try { catalog = catalogFor(symbol, stop); }
    catch (const roller::NoJournalFiles &) { return; }
    {
        std::lock_guard lock(indexMutex_);
        sourceKnownBeforeDay_[symbol] = knownBefore;
    }
    if (through <= 0) return;
    auto getCatalog = [&]() -> roller::JournalReader & { return *catalog; };
    if (!firstHour_.contains(symbol) || structuralChanged) {
        auto &files = getCatalog().files();
        int64_t first = through;
        for (const auto &file : files)
            first = std::min(first, file.header.at("opened_system_ns").get<int64_t>() / 1'000'000);
        firstHour_[symbol] = std::max<int64_t>(0, first / Hour * Hour);
    }
    const auto first = firstHour_.at(symbol);
    // Load the recent committed indexes before any journal replay can block this pass.
    refreshBuiltThrough(symbol);
    const auto nearDays = hmcDayPaths(options_, symbol, stop);
    {
        std::lock_guard catalogLock(catalogMutex_);
        hmcPaths_[symbol] = nearDays;
    }
    auto potentiallyChanged = [&](const DayIndex &index, int64_t day) {
        for (const auto &path : changed) {
            if (std::find(index.sourcePaths.begin(), index.sourcePaths.end(), path) != index.sourcePaths.end())
                return true;
            // A newly discovered segment on this UTC date can change a day's
            // journal window, including a previous empty hour.
            if (!added.contains(path))
                continue;
            const auto parentDay = path.parent_path().filename().string();
            if (parentDay.size() == 2 && std::all_of(parentDay.begin(), parentDay.end(), ::isdigit)) {
                auto date = path.parent_path();
                const auto dateText = date.parent_path().parent_path().filename().string() + "-" +
                                      date.parent_path().filename().string() + "-" + date.filename().string();
                const auto parsed = QDate::fromString(QString::fromStdString(dateText), "yyyy-MM-dd");
                if (parsed.isValid() && parsed.startOfDay(QTimeZone::UTC).toMSecsSinceEpoch() <= day)
                    return true;
            }
        }
        return false;
    };
    for (auto day = dayFloor(through - 1); day >= dayFloor(first); day -= Day) {
        checkStop(stop);
        const auto path = filePath(symbol, day);
        auto index = indexFor(symbol, day);
        // Do not synthesize empty complete days by replaying an older snapshot
        // across a date for which no source exists.
        const auto source = sourceState(symbol, day, day + Day);
        if (!index.file.present && !index.state.present &&
            (source == SourceState::Absent || source == SourceState::ReceiveTailOnly))
            continue;
        if (stamp(path) != index.file || stamp(path.string() + ".state") != index.state) {
            index = loadIndex(symbol, day);
            publishIndex(symbol, day, index);
        }
        uint32_t wanted = 0;
        for (int h = 0; h < 24; ++h)
            if (day + h * Hour >= first && day + (h + 1) * Hour <= through &&
                sourceState(symbol, day + h * Hour, day + (h + 1) * Hour) != SourceState::Absent)
                wanted |= 1u << h;
        const auto noteRunFailure = [&](int64_t from, int64_t to, const char *reason) {
            for (auto hour = from; hour < to; hour += Hour)
                noteBuildFailure(symbol, hour, reason);
        };
        try {
        const auto nearIt = nearDays.find(day);
        const fs::path hmcPath = nearIt == nearDays.end() ? fs::path{} : nearIt->second;
        bool sourceChanged = index.valid && potentiallyChanged(index, day);
        if (sourceChanged) {
            const auto knownPath = index.gridPath;
            const auto wasKnown = index.gridKnown;
            index = loadIndex(symbol, day); // only reload the FLD1 pair on an observed source change
            index.gridPath = knownPath;
            index.gridKnown = wasKnown;
            publishIndex(symbol, day, index);
            sourceChanged = !index.valid || !sourcesMatch(Sources::parse(index.sources), getCatalog(),
                                                          sourceCutoff(day, index.hours), stop);
            if (sourceChanged) {
                index.valid = false;
                index.hours = 0;
                publishIndex(symbol, day, index);
            }
        }
        const bool complete = (index.hours & wanted) == wanted;
        std::optional<FlowStoreGridValues> currentGrid;
        if (wanted && (!index.valid || !complete || sourceChanged || !index.gridKnown || index.gridPath != hmcPath)) {
            currentGrid = dayScales(options_, symbol, day, hmcPath, getCatalog(), stop);
            if (index.valid && index.grid != *currentGrid) {
                sourceChanged = true;
                index.valid = false;
                index.hours = 0;
            }
            index.gridKnown = true;
            index.gridPath = hmcPath;
            publishIndex(symbol, day, index);
        }
        if (!sourceChanged && complete) {
            if (day == 0)
                break;
            continue;
        }
        auto &files = getCatalog();
        uint32_t hours = sourceChanged ? 0 : index.hours;
        uint32_t revision = index.revision ? index.revision : 1;
        if (sourceChanged || (!index.valid && index.revision)) {
            if (revision == UINT32_MAX)
                throw std::runtime_error("flow revision exhausted");
            ++revision;
        }
        flow::DayFile data;
        if (hours) {
            auto existing = loadDay(path, symbol, day);
            if (!existing) {
                hours = 0;
                if (index.revision == UINT32_MAX)
                    throw std::runtime_error("flow revision exhausted");
                revision = std::max<uint32_t>(revision, index.revision + 1);
            } else
                data = std::move(existing->day);
        }
        const auto grid = currentGrid ? *currentGrid : dayScales(options_, symbol, day, hmcPath, files, stop);
        const auto &scales = grid.scales;
        if (hours && (data.nativeTick != scales.nativeTick || data.priceScale != scales.priceScale ||
                      data.baseScale != scales.baseScale || data.quoteScale != scales.quoteScale)) {
            hours = 0;
            if (revision == UINT32_MAX)
                throw std::runtime_error("flow revision exhausted");
            ++revision;
        }
        if (!hours) {
            data = {};
            data.product = symbol;
            data.dayStartMs = day;
            data.timeframeMs = Minute;
            data.nativeTick = scales.nativeTick;
            data.priceScale = scales.priceScale;
            data.baseScale = scales.baseScale;
            data.quoteScale = scales.quoteScale;
        }
        for (int high = 23; high >= 0;) {
            if (!(wanted & (1u << high)) || (hours & (1u << high))) {
                --high;
                continue;
            }
            int low = high;
            while (low > 0 && (wanted & (1u << (low - 1))) && !(hours & (1u << (low - 1))))
                --low;
            bool built = false;
            for (int endHour = high; endHour >= low && !built; --endHour) {
                const auto runFirst = day + low * Hour, runEnd = day + (endHour + 1) * Hour;
                const auto start = firstDurable(files, runFirst - TradeTail, stop);
                if (!start)
                    continue;
                const auto startPath = sourcePath(files, *start);
                std::string sourceBegin = startPath;
                if (hours) {
                    const auto oldSources = Sources::parse(index.sources);
                    const auto oldPath = oldSources.front().at("path").get<std::string>();
                    const auto &listed = files.files();
                    auto position = [&](const std::string &p) {
                        return std::find_if(listed.begin(), listed.end(),
                                            [&](const auto &f) { return f.path.generic_string() == p; }) -
                               listed.begin();
                    };
                    if (position(oldPath) < position(startPath))
                        sourceBegin = oldPath;
                }
                const auto nextHours = hours | (((1u << (endHour - low + 1)) - 1) << low);
                Sources sources;
                try {
                    sources = snapshotSources(files, sourceCutoff(day, nextHours), sourceBegin, stop);
                } catch (const NotReady &) {
                    continue;
                }
                const auto started = std::chrono::steady_clock::now();
                auto bars = buildRun(options_, symbol, scales, runFirst, runEnd, *start, stop);
                const auto afterFiles = inventory(symbol, stop);
                if (afterFiles.size() != currentFiles.size() ||
                    std::any_of(afterFiles.begin(), afterFiles.end(),
                                [&](const auto &entry) { return !currentFiles.contains(entry.first); })) {
                    noteRunFailure(runFirst, runEnd, "journal inventory changed");
                    return;
                }
                if (!sourcesMatch(sources, files, sourceCutoff(day, nextHours), stop)) {
                    noteRunFailure(runFirst, runEnd, "journal source changed");
                    return;
                }
                if (options_.hourThrough(symbol) < runEnd) {
                    noteRunFailure(runFirst, runEnd, "recorder hourThrough regressed");
                    return;
                }
                data.bars.insert(data.bars.end(), std::make_move_iterator(bars.begin()),
                                 std::make_move_iterator(bars.end()));
                data.revisions.insert(data.revisions.end(), bars.size(), revision);
                std::vector<size_t> order(data.bars.size());
                std::iota(order.begin(), order.end(), 0);
                std::sort(order.begin(), order.end(),
                          [&](size_t a, size_t b) { return data.bars[a].startMs < data.bars[b].startMs; });
                auto unsorted = std::move(data.bars);
                auto revisions = std::move(data.revisions);
                data.bars.clear();
                data.revisions.clear();
                for (auto i : order) {
                    data.bars.push_back(std::move(unsorted[i]));
                    data.revisions.push_back(revisions[i]);
                }
                hours = nextHours;
                data.sourceFingerprint = fingerprint(sources);
                writeDay(data, hours, revision, grid, sources.dump(), stop);
                auto fresh = loadIndex(symbol, day);
                fresh.gridKnown = true;
                fresh.gridPath = hmcPath;
                publishIndex(symbol, day, fresh);
                index = std::move(fresh);
                result.built += endHour - low + 1;
                const auto ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started)
                        .count();
                sLog_Data("Flow run built symbol=" << symbol << " hour=" << runFirst << " hours=" << (endHour - low + 1)
                                                   << " bars=" << bars.size() << " ms=" << ms);
                sLog_Probe("flow.store",
                           "symbol=" << symbol << " hour=" << runFirst << " bars=" << bars.size() << " ms=" << ms);
                high = low - 1;
                built = true;
            }
            if (!built) {
                noteRunFailure(day + low * Hour, day + (high + 1) * Hour, "journal tail unavailable");
                high = low - 1;
            }
        }
        } catch (const Cancelled &) {
            throw;
        } catch (const std::exception &e) {
            const auto current = indexFor(symbol, day);
            for (int h = 0; h < 24; ++h)
                if ((wanted & (1u << h)) && (!current.valid || !(current.hours & (1u << h))))
                    noteBuildFailure(symbol, day + h * Hour, e.what());
            ++result.failed;
        }
        if (day == 0)
            break;
    }
}
int64_t FlowStore::sealedThroughMs(const std::string &symbol) const {
    sentinel::capture::validateSymbol(symbol);
    std::lock_guard lock(indexMutex_);
    const auto it = builtThrough_.find(symbol);
    return it == builtThrough_.end() ? 0 : it->second;
}

FlowStore::Reply FlowStore::buildWindow(const std::string &symbol, int64_t from, int64_t to,
                                            std::stop_token stop) {
    std::stop_source combined;
    std::stop_callback callerStop(stop, [&] { combined.request_stop(); });
    std::stop_callback storeStop(stopSource_.get_token(), [&] { combined.request_stop(); });
    stop = combined.get_token();
    sentinel::capture::validateSymbol(symbol);
    if (from < 0 || to <= from || from % 1000 || to % 1000 || to > INT64_MAX - Day - TradeTail)
        throw std::invalid_argument("invalid flow second window");
    if (to - from > kMaxReplayWindowMs)
        throw std::invalid_argument("1s journal replay cap is 3600000 ms (1 h)");
    Reply reply;
    try {
        const auto catalog = catalogFor(symbol, stop);
        std::map<int64_t, fs::path> grids;
        bool cataloguedGrids = false;
        {
            std::lock_guard lock(catalogMutex_);
            if (const auto it = hmcPaths_.find(symbol); it != hmcPaths_.end()) {
                grids = it->second;
                cataloguedGrids = true;
            }
        }
        if (!cataloguedGrids) {
            grids = hmcDayPaths(options_, symbol, stop);
            std::lock_guard lock(catalogMutex_);
            hmcPaths_[symbol] = grids;
        }
        for (auto first = from; first < to;) {
            const auto day = dayFloor(first), end = std::min(to, day + Day);
            const auto index = indexFor(symbol, day);
            if (!index.file.present && !index.state.present && sourceState(symbol, first, end) == SourceState::Absent) {
                reply.ranges.push_back({first, end, "unrecorded", {}});
                first = end;
                continue;
            }
            const auto firstJournalMs = catalog->files().front().header.at("opened_system_ns").get<int64_t>() / 1'000'000;
            if (firstJournalMs >= end) {
                reply.ranges.push_back({first, end, "partial", {}});
                first = end;
                continue;
            }
            const auto start = firstDurable(*catalog, first - TradeTail, stop);
            if (!start) {
                reply.ranges.push_back({first, end, "partial", {}});
                first = end;
                continue;
            }
            const auto grid = dayScales(options_, symbol, day,
                grids.contains(day) ? grids.at(day) : fs::path{}, *catalog, stop);
            bool partial = firstJournalMs > first;
            Sources sources;
            try {
                sources = snapshotSources(*catalog, end + TradeTail, sourcePath(*catalog, *start), stop);
            } catch (const NotReady &) { partial = true; }
            bool rejected = false;
            auto bars = buildRun(options_, symbol, grid.scales, first, end, *start, stop, 1000, &rejected, catalog.get());
            if (!sources.empty() && !sourcesMatch(sources, *catalog, end + TradeTail, stop))
                throw std::runtime_error("journal changed during flow replay");
            reply.ranges.push_back({first, end, partial || rejected ? "partial" : "complete", grid.scales});
            reply.revisions.insert(reply.revisions.end(), bars.size(), 1);
            reply.bars.insert(reply.bars.end(), std::make_move_iterator(bars.begin()),
                              std::make_move_iterator(bars.end()));
            first = end;
        }
    } catch (const roller::NoJournalFiles &) {
        for (auto first = from; first < to;) {
            const auto end = std::min(to, dayFloor(first) + Day);
            const auto index = indexFor(symbol, dayFloor(first));
            const bool absent = !index.file.present && !index.state.present && sourceState(symbol, first, end) == SourceState::Absent;
            reply.ranges.push_back({first, end, absent ? "unrecorded" : "partial", {}});
            first = end;
        }
    }
    return reply;
}
FlowStore::TradeWindow FlowStore::readTrades(const std::string &symbol, int64_t from, int64_t to,
                                             size_t limit, std::stop_token stop) {
    std::stop_source combined;
    std::stop_callback callerStop(stop, [&] { combined.request_stop(); });
    std::stop_callback storeStop(stopSource_.get_token(), [&] { combined.request_stop(); });
    stop = combined.get_token();
    sentinel::capture::validateSymbol(symbol);
    if (from < 0 || to <= from || to - from > protocol::SentinelProtocol::kTradeHistoryMaxWindowMs ||
        to > INT64_MAX - TradeTail || limit > protocol::SentinelProtocol::kTradeHistoryMaxTrades)
        throw std::invalid_argument("invalid trade journal window");
    TradeWindow result;
    result.coveredFromMs = from;
    result.coveredToMs = from;
    try {
        const auto catalog = catalogFor(symbol, stop);
        const auto firstJournalMs = catalog->files().front().header.at("opened_system_ns").get<int64_t>() / 1'000'000;
        result.coveredFromMs = std::max(from, firstJournalMs);
        if (result.coveredFromMs >= to) { result.partial = true; return result; }
        const auto start = firstDurable(*catalog, std::max<int64_t>(0, from - TradeTail), stop);
        if (!start) { result.partial = true; return result; }
        Sources sources;
        try {
            sources = snapshotSources(*catalog, to + TradeTail, sourcePath(*catalog, *start), stop);
        } catch (const NotReady&) { result.partial = true; }
        roller::JournalReader reader(*catalog, *start);
        roller::JournalFeed feed(symbol);
        std::optional<int64_t> firstGapMs;
        feed.onInvalid = [&](int64_t ms, const std::string&) {
            if (ms > from && ms < to + TradeTail) {
                result.partial = true;
                if (ms < to) firstGapMs = std::min(firstGapMs.value_or(ms), ms);
            }
        };
        feed.onTrade = [&](const Trade& trade) {
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                trade.timestamp.time_since_epoch()).count();
            if (ms < from || ms >= to) return;
            if (result.rows.size() > limit) return;
            if (trade.trade_id.size() > protocol::SentinelProtocol::kTradeHistoryMaxIdBytes ||
                !std::isfinite(trade.price) || trade.price <= 0 ||
                !std::isfinite(trade.size) || trade.size <= 0) {
                result.partial = true;
                return;
            }
            if (result.rows.size() <= limit)
                result.rows.push_back({trade.trade_id, ms, trade.price, trade.size, trade.side, 1});
        };
        roller::JournalRecord input;
        int64_t lastReceive = 0;
        while (reader.next(input)) {
            checkStop(stop);
            const auto receiveMs = input.record.time.systemNs / 1'000'000;
            lastReceive = std::max(lastReceive, receiveMs);
            if (receiveMs > to + TradeTail) break;
            feed.apply(input);
            if (options_.afterRecordForTest) options_.afterRecordForTest(stop);
            if (result.rows.size() > limit) break;
        }
        checkStop(stop);
        bool sourceChanged = reader.damaged();
        if (!sourceChanged && !sources.empty()) {
            try { sourceChanged = !sourcesMatch(sources, *catalog, to + TradeTail, stop); }
            catch (const fs::filesystem_error&) { sourceChanged = true; }
        }
        if (sourceChanged) { result.partial = true; result.rows.clear(); }
        result.coveredToMs = std::min(to, lastReceive);
        if (firstGapMs) result.coveredToMs = std::min(result.coveredToMs, *firstGapMs);
        if (sourceChanged) result.coveredToMs = result.coveredFromMs;
        result.coveredToMs = std::max(result.coveredFromMs, result.coveredToMs);
        result.partial |= result.coveredFromMs > from || lastReceive < to + TradeTail;
        std::stable_sort(result.rows.begin(), result.rows.end(), [](const auto& a, const auto& b) {
            return a.exchangeMs < b.exchangeMs;
        });
    } catch (const roller::NoJournalFiles&) { result.partial = true; }
    return result;
}
FlowStore::Reply FlowStore::window(const std::string &symbol, int64_t from, int64_t to) {
    sentinel::capture::validateSymbol(symbol);
    if (from < 0 || to <= from || from % Minute || to % Minute || to > INT64_MAX - Day - TradeTail)
        throw std::invalid_argument("invalid flow minute window");
    Reply reply;
    bool missingSealed = false;
    bool sealedRequested = false;
    try {
        const auto through = sealedThroughMs(symbol);
        reply.sealedThroughMs = through;
        for (auto day = dayFloor(from); day < to; day += Day) {
            const auto path = filePath(symbol, day);
            const auto index = indexFor(symbol, day);
            const bool readable = index.valid && stamp(path) == index.file &&
                                  stamp(path.string() + ".state") == index.state;
            uint32_t required = 0;
            for (auto hour = std::max(day, from / Hour * Hour); hour < std::min(day + Day, to); hour += Hour) {
                const bool noStoredHour = (!index.file.present && !index.state.present) ||
                    (readable && !(index.hours & (1u << ((hour - day) / Hour))));
                const auto source = sourceState(symbol, std::max(hour, from), std::min(hour + Hour, to));
                if (noStoredHour && (source == SourceState::Absent || source == SourceState::ReceiveTailOnly)) {
                    reply.ranges.push_back({std::max(hour, from), std::min(hour + Hour, to),
                        source == SourceState::Absent ? "unrecorded" : "partial", {}});
                    continue;
                }
                const bool ready = readable && hour + Hour <= through &&
                    (index.hours & (1u << ((hour - day) / Hour)));
                reply.ranges.push_back({std::max(hour, from), std::min(hour + Hour, to),
                                        ready ? "complete" : "building", index.grid.scales, index.revision});
                if (hour + Hour > through)
                    reply.building = true;
                else
                    required |= 1u << ((hour - day) / Hour);
            }
            sealedRequested |= required != 0;
            if (!readable) {
                reply.building |= required != 0;
                missingSealed |= required != 0;
                continue;
            }
            const auto absent = required & ~index.hours;
            if (absent) {
                reply.building = true;
                missingSealed = true;
            }
            const auto bytes = readFile(path, MaxDayBytes);
            auto data = flow::decodeFlowDay(spanOf(bytes));
            if (data.product != symbol || data.dayStartMs != day || data.sourceFingerprint != index.fingerprint)
                throw std::runtime_error("flow indexed file changed");
            for (size_t i = 0; i < data.bars.size(); ++i) {
                const auto &bar = data.bars[i];
                if (bar.startMs >= from && bar.startMs < to && bar.startMs / Hour * Hour + Hour <= through &&
                    (index.hours & (1u << ((bar.startMs - day) / Hour)))) {
                    reply.bars.push_back(bar);
                    reply.revisions.push_back(data.revisions[i]);
                }
            }
        }
    } catch (const std::exception &e) {
        reply.ranges = {{from, to, "building", {}}};
        reply.bars.clear();
        reply.revisions.clear();
        reply.building = true;
        missingSealed = sealedRequested;
        sLog_Warning("Flow read failed symbol=" << symbol << " error=" << e.what());
    }
    if (missingSealed) {
        const auto now = std::chrono::steady_clock::now();
        bool send = false;
        {
            std::lock_guard lock(wakeMutex_);
            if (lastWindowWake_ == std::chrono::steady_clock::time_point{} ||
                now - lastWindowWake_ >= std::chrono::seconds(10)) {
                lastWindowWake_ = now;
                send = true;
            }
        }
        if (send)
            wake();
    }
    return reply;
}
} // namespace recording
