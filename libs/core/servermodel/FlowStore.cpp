#include "FlowStore.hpp"
#include "RollupStore.hpp"
#include "PersistenceIo.hpp"
#include "SentinelLogging.hpp"
#include "flow/FlowBarBuilder.hpp"
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
#if defined(__APPLE__)
#include <pthread.h>
#include <sys/qos.h>
#elif !defined(_WIN32)
#include <sys/resource.h>
#endif

namespace recording {
namespace {
namespace flow = sentinel::flow;
namespace roller = sentinel::roller;
namespace fs = std::filesystem;
constexpr int64_t Minute = 60'000, Hour = 60 * Minute, Day = 24 * Hour;
// Same receive-time tail as JournalTradeEventSource. Buckets use exchange time.
constexpr int64_t TradeTail = Minute;
constexpr qint64 MaxDayBytes = 64 * 1024 * 1024;
using Fingerprint = std::array<uint8_t, 32>;
struct StoredDay {
    flow::DayFile day;
    uint32_t hours = 0, revision = 1;
    nlohmann::json sources;
};
struct Cancelled {};
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
QByteArray completion(const QByteArray &bytes, uint32_t hours, uint32_t revision, const std::string &sources) {
    QByteArray state("FLS1", 4);
    for (unsigned i = 0; i < 4; ++i)
        state.append(char(hours >> (i * 8)));
    for (unsigned i = 0; i < 4; ++i)
        state.append(char(revision >> (i * 8)));
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
        if (state.size() < 46)
            return {};
        uint32_t hours = 0, revision = 0;
        for (unsigned i = 0; i < 4; ++i) {
            hours |= uint32_t(uint8_t(state[4 + i])) << (i * 8);
            revision |= uint32_t(uint8_t(state[8 + i])) << (i * 8);
        }
        const auto sourceText = state.mid(12, state.size() - 44).toStdString();
        if (!hours || hours >> 24 || !revision || state != completion(bytes, hours, revision, sourceText))
            return {};
        auto sources = Sources::parse(sourceText);
        auto decoded = flow::decodeFlowDay(spanOf(bytes));
        if (decoded.product != symbol || decoded.dayStartMs != day || decoded.timeframeMs != Minute ||
            decoded.bars.size() > 1440 || decoded.sourceFingerprint != fingerprint(sources))
            return {};
        for (const auto &bar : decoded.bars)
            if (!(hours & (1u << ((bar.startMs - day) / Hour))))
                return {};
        return StoredDay{std::move(decoded), hours, revision, std::move(sources)};
    } catch (const std::exception &) {
        return {};
    }
}
flow::DayFile dayGrid(const FlowStore::Options &options, const std::string &symbol, int64_t day,
                      roller::JournalReader &catalog, std::string &sourceBegin, std::stop_token stop) {
    checkStop(stop);
    const auto anchor = catalog.anchor(day);
    sourceBegin = catalog.files().front().path.generic_string();
    if (anchor)
        for (const auto &file : catalog.files())
            if (file.header.at("run_id") == anchor->run &&
                file.header.at("first_block_ordinal").get<uint64_t>() <= anchor->block)
                sourceBegin = file.path.generic_string();
    checkStop(stop);
    roller::JournalReader reader(options.journalRoot, symbol, anchor);
    roller::JournalFeed feed(symbol);
    roller::JournalRecord input;
    std::optional<roller::Grid> grid;
    feed.onSnapshot = [&](int64_t, int64_t, std::vector<Level> levels) {
        double bid = 0, ask = INFINITY;
        for (const auto &l : levels)
            if (l.size > 0) {
                if (l.isBid)
                    bid = std::max(bid, l.price);
                else
                    ask = std::min(ask, l.price);
            }
        if (bid > 0 && std::isfinite(ask))
            grid = roller::deriveGrid(input.metadata, std::midpoint(bid, ask));
    };
    while (!grid && reader.next(input)) {
        checkStop(stop);
        if (input.record.time.systemNs / 1'000'000 >= day + Day)
            break;
        feed.apply(input);
        if (options.afterRecordForTest)
            options.afterRecordForTest(stop);
    }
    checkStop(stop);
    if (!grid)
        throw std::runtime_error("no daily grid snapshot");
    flow::DayFile out;
    out.product = symbol;
    out.dayStartMs = day;
    out.timeframeMs = Minute;
    out.nativeTick = grid->nearTick;
    out.priceScale = std::llround(grid->priceScale);
    out.baseScale = options.baseScale;
    out.quoteScale = options.quoteScale;
    return out;
}
std::vector<flow::FlowBar> buildHour(const FlowStore::Options &options, const flow::DayFile &day, int64_t hour,
                                     roller::JournalReader &catalog, std::stop_token stop) {
    checkStop(stop);
    const auto anchor = catalog.anchor(hour);
    checkStop(stop);
    roller::JournalReader reader(options.journalRoot, day.product, anchor);
    roller::JournalFeed feed(day.product);
    flow::FlowBarBuilder builder(Minute, day.nativeTick, day.priceScale, day.baseScale, day.quoteScale);
    flow::Trade trade; // reuse the adapter's string storage across callbacks
    // FLB1 strings are u16-sized. Reserve once, so adapting a print never allocates.
    trade.id.reserve(UINT16_MAX);
    feed.onTrade = [&](const ::Trade &t) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t.timestamp.time_since_epoch()).count();
        if (ms < hour || ms >= hour + Hour)
            return;
        if (t.trade_id.size() > UINT16_MAX)
            throw std::runtime_error("flow trade id exceeds codec limit");
        trade.id.assign(t.trade_id);
        trade.exchangeMs = ms;
        trade.price = t.price;
        trade.size = t.size;
        trade.side = t.side == AggressorSide::Buy    ? flow::Side::Buy
                     : t.side == AggressorSide::Sell ? flow::Side::Sell
                                                     : flow::Side::Unknown;
        builder.add(trade);
    };
    roller::JournalRecord input;
    while (reader.next(input)) {
        checkStop(stop);
        if (input.record.time.systemNs / 1'000'000 > hour + Hour + TradeTail)
            break;
        feed.apply(input); // All channels: sequence identity and reconnect dedupe need every frame.
        if (options.afterRecordForTest)
            options.afterRecordForTest(stop);
    }
    checkStop(stop);
    if (reader.damaged())
        throw std::runtime_error("damaged journal in flow hour");
    if (builder.rejectedCount())
        sLog_Warning("Flow rejected trades symbol=" << day.product << " hour=" << hour
                                                    << " count=" << builder.rejectedCount());
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

fs::path flowRootFor(const std::string &configured, const fs::path &hmc2Root, const fs::path &journalRoot) {
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
        return root;
    } catch (const fs::filesystem_error &) {
        return {};
    }
}
FlowStore::FlowStore(Options options) : options_(std::move(options)) {
    options_.root = flowRootFor(options_.root.string(), options_.hmc2Root, options_.journalRoot);
    if (options_.root.empty() || !options_.hourThrough || options_.rescan.count() <= 0 || options_.baseScale <= 0 ||
        options_.quoteScale <= 0)
        throw std::invalid_argument("flow store needs disjoint roots, scales and an hour watermark");
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
    {
        std::lock_guard lock(wakeMutex_);
        woken_ = true;
    }
    wakeCv_.notify_all();
}
void FlowStore::builderLoop(std::stop_token stop) {
    try {
        sentinel::logging::setCurrentThreadName("flow-builder");
#if defined(__APPLE__)
        const bool lowered = pthread_set_qos_class_self_np(QOS_CLASS_BACKGROUND, 0) == 0;
#elif defined(_WIN32)
        const bool lowered = SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN) != 0;
#else
        const bool lowered = setpriority(PRIO_PROCESS, 0, 19) == 0;
#endif
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
    if (flowRootFor(options_.root.string(), options_.hmc2Root, options_.journalRoot) != options_.root)
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
void FlowStore::writeDay(const flow::DayFile &day, uint32_t hours, uint32_t revision, const std::string &sources,
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
    atomicFile(path.string() + ".state", completion(bytes, hours, revision, sources));
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
void FlowStore::passSymbol(const std::string &symbol, std::stop_token stop, PassResult &result) {
    const auto through = options_.hourThrough(symbol) / Hour * Hour;
    if (through <= 0)
        return;
    roller::JournalReader catalog(options_.journalRoot, symbol);
    int64_t first = through;
    for (const auto &f : catalog.files())
        first = std::min(first, f.header.at("opened_system_ns").get<int64_t>() / 1'000'000);
    first = std::max<int64_t>(0, first / Hour * Hour);
    for (auto day = dayFloor(through - 1); day >= dayFloor(first); day -= Day) {
        checkStop(stop);
        const auto path = filePath(symbol, day);
        if (!ownedPath(path) || !ownedPath(path.string() + ".state"))
            throw std::runtime_error("unsafe flow path");
        auto old = loadDay(path, symbol, day);
        const auto wanted = [&](int64_t hour) { return hour >= first && hour + Hour <= through; };
        uint32_t hours = 0, revision = 1;
        flow::DayFile data;
        std::string sourceBegin;
        if (old && sourcesMatch(old->sources, catalog, sourceCutoff(day, old->hours), stop) &&
            old->day.baseScale == options_.baseScale && old->day.quoteScale == options_.quoteScale) {
            hours = old->hours;
            revision = old->revision;
            data = std::move(old->day);
            sourceBegin = old->sources.front().at("path").get<std::string>();
        } else {
            if (old) {
                if (old->revision == UINT32_MAX)
                    throw std::runtime_error("flow revision exhausted");
                revision = old->revision + 1;
            }
            data = dayGrid(options_, symbol, day, catalog, sourceBegin, stop);
        }
        for (int h = 23; h >= 0; --h) {
            const auto hour = day + h * Hour;
            if (!wanted(hour) || (hours & (1u << h)))
                continue;
            checkStop(stop);
            const auto started = std::chrono::steady_clock::now();
            const auto nextHours = hours | (1u << h);
            Sources sources;
            try {
                sources = snapshotSources(catalog, sourceCutoff(day, nextHours), sourceBegin, stop);
            } catch (const NotReady &) {
                continue;
            } // Wait for the durable receive tail; still backfill older hours.
            auto bars = buildHour(options_, data, hour, catalog, stop);
            // Re-discover too: a segment added during replay must invalidate the snapshot.
            roller::JournalReader current(options_.journalRoot, symbol);
            if (!sourcesMatch(sources, current, sourceCutoff(day, nextHours), stop))
                return;
            data.sourceFingerprint = fingerprint(sources);
            if (options_.hourThrough(symbol) < hour + Hour)
                return;
            const auto count = bars.size();
            data.bars.insert(data.bars.begin(), std::make_move_iterator(bars.begin()),
                             std::make_move_iterator(bars.end()));
            data.revisions.insert(data.revisions.begin(), count, revision);
            hours |= 1u << h;
            // Existing completed hours may be earlier than newly sealed ones.
            std::vector<size_t> order(data.bars.size());
            std::iota(order.begin(), order.end(), 0);
            std::sort(order.begin(), order.end(),
                      [&](size_t a, size_t b) { return data.bars[a].startMs < data.bars[b].startMs; });
            auto unordered = std::move(data.bars);
            auto revisions = std::move(data.revisions);
            data.bars.clear();
            data.revisions.clear();
            for (auto i : order) {
                data.bars.push_back(std::move(unordered[i]));
                data.revisions.push_back(revisions[i]);
            }
            writeDay(data, hours, revision, sources.dump(), stop);
            ++result.built;
            const auto ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started)
                    .count();
            sLog_Probe("flow.store", "symbol=" << symbol << " hour=" << hour << " bars=" << count << " ms=" << ms);
            sLog_Data("Flow hour built symbol=" << symbol << " hour=" << hour << " bars=" << count << " ms=" << ms);
        }
        if (day == 0)
            break;
    }
}
FlowStore::Reply FlowStore::window(const std::string &symbol, int64_t from, int64_t to) {
    sentinel::capture::validateSymbol(symbol);
    if (from < 0 || to <= from || from % Minute || to % Minute || to > INT64_MAX - Day - TradeTail)
        throw std::invalid_argument("invalid flow minute window");
    Reply reply;
    try {
        const auto through = options_.hourThrough(symbol);
        roller::JournalReader catalog(options_.journalRoot, symbol);
        for (auto day = dayFloor(from); day < to; day += Day) {
            const auto path = filePath(symbol, day);
            auto stored =
                ownedPath(path) && ownedPath(path.string() + ".state") ? loadDay(path, symbol, day) : std::nullopt;
            if (!stored || !sourcesMatch(stored->sources, catalog, sourceCutoff(day, stored->hours)) ||
                stored->day.baseScale != options_.baseScale || stored->day.quoteScale != options_.quoteScale) {
                reply.building = true;
                continue;
            }
            const auto &data = stored->day;
            for (auto hour = std::max(day, from / Hour * Hour); hour < std::min(day + Day, to); hour += Hour)
                if (hour + Hour > through || !(stored->hours & (1u << ((hour - day) / Hour))))
                    reply.building = true;
            for (size_t i = 0; i < data.bars.size(); ++i) {
                const auto &bar = data.bars[i];
                if (bar.startMs >= from && bar.startMs < to && bar.startMs / Hour * Hour + Hour <= through) {
                    reply.bars.push_back(bar);
                    reply.revisions.push_back(data.revisions[i]);
                }
            }
        }
    } catch (const roller::NoJournalFiles &) {
        reply.building = true;
    } catch (const std::exception &e) {
        reply.bars.clear();
        reply.revisions.clear();
        reply.building = true;
        sLog_Warning("Flow read failed symbol=" << symbol << " error=" << e.what());
    }
    if (reply.building)
        wake();
    return reply;
}
} // namespace recording
