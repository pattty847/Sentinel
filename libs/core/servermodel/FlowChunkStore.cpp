// Canonical scans use a fresh feed per hour and update M before delivering the
// seal record's trades. Published bytes are immutable within an epoch. Fence
// first, persist the epoch, then rebuild and unfence. The worker uses utility
// priority and gives the live-retention days of every product a priority lane.
// No lock spans replay or disk I/O; readers recheck the epoch/fence after I/O.
#include "FlowChunkStore.hpp"
#include "RollupStore.hpp"
#include "PersistenceIo.hpp"
#include "SentinelLogging.hpp"
#include "flow/FlowBarBuilder.hpp"
#include "flow/FlowCompose.hpp"
#include "roller/JournalFeed.hpp"
#include "heatmap/ChunkCodec.hpp"
#include "Crc32.hpp"
#include <QFile>
#include <QDateTime>
#include <QTimeZone>
#include <algorithm>
#include <bit>
#include <cctype>
#include <nlohmann/json.hpp>
#include <unordered_set>
#if defined(__APPLE__)
#include <pthread.h>
#include <sys/qos.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/resource.h>
#endif

namespace recording {
namespace {
namespace fs = std::filesystem;
namespace flow = sentinel::flow;
namespace chunks = flow::chunks;
namespace roller = sentinel::roller;
constexpr int64_t Minute = 60'000, Hour = 60 * Minute, DayMs = 24 * Hour;
struct Cancelled {};
struct HourUnavailable : std::runtime_error { using std::runtime_error::runtime_error; };
bool setChunkWorkerPriority() {
#if defined(__APPLE__)
    return pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0) == 0;
#elif defined(_WIN32)
    return SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL) != 0;
#else
    return setpriority(PRIO_PROCESS, 0, 10) == 0;
#endif
}
void check(std::stop_token stop) { if (stop.stop_requested()) throw Cancelled{}; }
std::string symbolOf(const std::string &id) {
    if (!id.starts_with("coinbase:")) throw std::invalid_argument("flow chunk instrument");
    const auto symbol = id.substr(9);
    sentinel::capture::validateSymbol(symbol);
    return symbol;
}
std::vector<uint8_t> bytes(const fs::path &path) {
    QFile f(QString::fromStdString(path.string()));
    if (!f.open(QIODevice::ReadOnly) || f.size() <= 0 || f.size() > 64 * 1024 * 1024)
        throw std::runtime_error("flow chunk file unavailable or oversized");
    const auto b = f.readAll();
    if (b.size() != f.size()) throw std::runtime_error("flow chunk short read");
    return {reinterpret_cast<const uint8_t *>(b.data()), reinterpret_cast<const uint8_t *>(b.data()) + b.size()};
}
std::optional<roller::JournalPos> scanCursor(const std::string &symbol, int64_t warmup,
    const roller::JournalReader &catalog, std::stop_token stop, const std::function<void()> &afterRecord = {}) {
    // RAWL2 rotates on every receive-hour change, including backward changes.
    // Skip only segments whose entire receive hour is below the warmup fence;
    // never binary-search wall times or choose the last earlier run.
    std::optional<roller::JournalPos> cursor;
    for (const auto &file : catalog.files()) {
        check(stop);
        if (file.openedNs / 1'000'000 / Hour < warmup / Hour) continue;
        sentinel::capture::RecordReader probe(QString::fromStdString(file.path.string()), !file.superseded);
        sentinel::capture::Record record;
        if (probe.next(record)) {
            if (afterRecord) afterRecord();
            cursor = roller::JournalPos{symbol, file.run, probe.result().recordOrdinal, probe.result().recordIndex};
            break;
        }
    }
    return cursor;
}
bool validatePosition(const roller::JournalReader &catalog, const roller::JournalPos &pos,
                      std::stop_token stop) {
    // The footer's ordinal/offset index can seek to the saved boundary block.
    // Its first/last timestamps are not extrema: a clock step inside a block
    // may cross a threshold and then step back, so timestamp binary search is
    // not a valid replacement for the saved exact record position.
    for (auto it = catalog.files().rbegin(); it != catalog.files().rend(); ++it) {
        if (it->run != pos.run || it->firstBlock > pos.block) continue;
        sentinel::capture::RecordReader reader(QString::fromStdString(it->path.string()), !it->superseded, pos.block);
        sentinel::capture::Record record;
        while (reader.next(record)) {
            check(stop);
            const auto &result = reader.result();
            if (result.recordOrdinal > pos.block) break;
            if (result.recordOrdinal == pos.block && result.recordIndex == pos.record) return true;
        }
        return false;
    }
    return false;
}
bool validateCachedBoundaries(const roller::JournalReader &catalog,
                              const FlowChunkStore::SourceCheck &source, std::stop_token stop) {
    return validatePosition(catalog, source.first, stop) && validatePosition(catalog, source.last, stop);
}
size_t fileOf(const roller::JournalReader &catalog, const roller::JournalPos &position) {
    size_t found = catalog.files().size();
    for (size_t n = 0; n < catalog.files().size(); ++n) {
        const auto &file = catalog.files()[n];
        if (file.run == position.run && file.firstBlock <= position.block) found = n;
    }
    if (found == catalog.files().size()) throw std::runtime_error("RAWL2 boundary file missing");
    return found;
}
std::optional<roller::JournalPos> firstAtOrBeyond(const std::string &symbol,
    const roller::JournalReader &catalog, const roller::JournalPos &start, int64_t threshold,
    std::stop_token stop) {
    // Capture rotates whenever the receive *hour* changes, including clock
    // steps back. That excludes whole files outside this threshold's hour.
    // BLK1/IDX1 record only first/last times, not extrema, so blocks in the
    // threshold hour cannot safely be binary-searched. Decode those blocks
    // until the first crossing; seek to the start block through IDX1.
    const auto from = fileOf(catalog, start);
    for (size_t n = from; n < catalog.files().size(); ++n) {
        check(stop);
        const auto &file = catalog.files()[n];
        if (file.openedNs / 1'000'000 / Hour < threshold / Hour) continue;
        sentinel::capture::RecordReader reader(QString::fromStdString(file.path.string()), !file.superseded,
            n == from ? std::optional<uint64_t>(start.block) : std::nullopt);
        sentinel::capture::Record record;
        while (reader.next(record)) {
            check(stop);
            const roller::JournalPos pos{symbol, file.run, reader.result().recordOrdinal, reader.result().recordIndex};
            if (n == from && std::pair(pos.block, pos.record) < std::pair(start.block, start.record)) continue;
            if (record.time.systemNs / 1'000'000 >= threshold) return pos;
        }
    }
    return {};
}
bool sealReached(int64_t sealMs, const roller::JournalReader &catalog, std::stop_token stop) {
    // RAWL2 rotates on every receive-hour change. Only files opened in or
    // after the seal hour can contain its first durable crossing record.
    // This polls one hour's files without feeding or decoding the preceding
    // hour's trades on every worker wake, including when W exceeds 60 s.
    for (const auto &file : catalog.files()) {
        check(stop);
        if (file.openedNs / 1'000'000 / Hour < sealMs / Hour) continue;
        try {
            sentinel::capture::RecordReader probe(QString::fromStdString(file.path.string()), !file.superseded);
            sentinel::capture::Record record;
            while (probe.next(record)) {
                check(stop);
                if (record.time.systemNs / 1'000'000 >= sealMs) return true;
            }
        } catch (const std::exception &) {
            // A damaged seal-hour file cannot suppress healthy later hours.
            // buildHour will mark the affected hour unavailable.
        }
    }
    return false;
}
chunks::FlowGrid gridOf(const FlowStoreGridValues &g) {
    return {g.scales.rowTickUnits, g.scales.priceScale, g.scales.baseScale, g.scales.quoteScale};
}
void appendRun(std::vector<chunks::CoverageRun> &runs, uint16_t slot, chunks::Status status) {
    if (!runs.empty() && runs.back().status == status) runs.back().toSlot = slot + 1;
    else runs.push_back({slot, uint16_t(slot + 1), status});
}
chunks::DaySection section(std::span<const flow::FlowBar> bars, int64_t level, int64_t start,
                           uint32_t count, chunks::FlowGrid grid, std::span<const chunks::CoverageRun> coverage) {
    std::vector<chunks::FlowGrid> grids(bars.size(), grid);
    auto data = chunks::fromBars(bars, level, start, count, nullptr, grids, coverage);
    chunks::DaySection result;
    result.sealed = true;
    result.stepExponent = uint8_t(std::countr_zero(data.priceStep));
    result.payload = chunks::encode(data, false, &result.summaryLength);
    return result;
}
chunks::Status composedStatus(std::span<const chunks::CoverageRun> runs) {
    const auto status = runs.front().status;
    return std::all_of(runs.begin(), runs.end(), [&](const auto &r) { return r.status == status; }) ?
        status : chunks::Status::Partial;
}
void compose(chunks::DayFile &day) {
    std::vector<flow::FlowBar> minutes, hours;
    std::vector<chunks::CoverageRun> coverage;
    for (int h = 0; h < 24; ++h) {
        const auto &s = day.sections[h];
        if (!s.sealed) return;
        const auto decoded = chunks::decode(s.payload, Minute, day.dayStartMs + h * Hour, 60, false,
                                           uint64_t(1) << s.stepExponent);
        auto bars = chunks::toBars(decoded);
        appendRun(coverage, uint16_t(h), composedStatus(decoded.coverage));
        if (!bars.empty()) hours.push_back(flow::composeFlowBars(bars, Hour));
        minutes.insert(minutes.end(), bars.begin(), bars.end());
    }
    day.sections[24] = section(hours, Hour, day.dayStartMs, 24, day.grid, coverage);
    std::vector<flow::FlowBar> daily;
    if (!minutes.empty()) daily.push_back(flow::composeFlowBars(minutes, DayMs));
    const std::array<chunks::CoverageRun, 1> dailyCoverage{{{0, 1, composedStatus(coverage)}}};
    day.sections[25] = section(daily, DayMs, day.dayStartMs, 1, day.grid, dailyCoverage);
}
}
FlowChunkStore::FlowChunkStore(FlowStore &sources, Options options) : sources_(sources), options_(std::move(options)) {
    const auto &o = sources.options();
    if (!options_.root.empty() && fs::is_symlink(fs::symlink_status(options_.root)))
        throw std::invalid_argument("flow chunk root is a symlink");
    auto protectedRoots = o.protectedRoots;
    protectedRoots.push_back(sources.root());
    auto hmcRoot = fs::weakly_canonical(o.hmc2Root);
    if (!hmcRoot.has_filename()) hmcRoot = hmcRoot.parent_path();
    const auto configured = options_.root.empty() ? hmcRoot.string() + "-flc" : options_.root.string();
    options_.root = flowRootFor(configured, o.hmc2Root, o.journalRoot, o.rollupRoot, protectedRoots);
    if (options_.root.empty() || options_.rescan.count() <= 0 || options_.rules.sealWindowMs > Hour)
        throw std::invalid_argument("flow chunk store needs disjoint roots and W <= one hour");
    for (const auto &symbol : o.symbols) {
        auto &i = instruments_["coinbase:" + symbol];
        if (!options_.metrics) continue;
        auto &m = *options_.metrics;
        const sentinel::metrics::Labels labels{{"instrument", "coinbase:" + symbol}};
        i.late = &m.counter("sentinel_flow_late_trades_total", "Trades excluded when an hour first seals in this process.", labels);
        i.regressions = &m.counter("sentinel_flow_receive_regressions_total", "Receive regressions when an hour first seals in this process.", labels);
        i.failures = &m.counter("sentinel_flow_chunk_build_failures_total", "Failed chunk day builds, retried next pass.", labels);
        i.hours = &m.counter("sentinel_flow_chunk_hours_sealed_total", "Hour sections first sealed in this process.", labels);
        i.pending = &m.gauge("sentinel_flow_chunk_pending_days", "Days waiting for build or epoch validation.", labels);
    }
}
FlowChunkStore::~FlowChunkStore() { stop(); }
FlowStore::Archive FlowChunkStore::sharedArchive(const std::string &symbol, std::stop_token stop) {
    if (options_.afterArchiveRefreshForTest) options_.afterArchiveRefreshForTest();
    return sources_.archive(symbol, stop);
}
fs::path FlowChunkStore::checked(const fs::path &path) const {
    const auto &o = sources_.options();
    auto roots = o.protectedRoots; roots.push_back(sources_.root());
    if (flowRootFor(options_.root.string(), o.hmc2Root, o.journalRoot, o.rollupRoot, roots) != options_.root)
        throw std::runtime_error("unsafe chunk root");
    const auto relative = path.lexically_relative(options_.root);
    if (relative.empty() || *relative.begin() == "..") throw std::runtime_error("unsafe chunk path");
    auto at = options_.root;
    if (fs::is_symlink(fs::symlink_status(at))) throw std::runtime_error("chunk root symlink");
    for (const auto &part : relative) {
        at /= part;
        if (fs::is_symlink(fs::symlink_status(at))) throw std::runtime_error("chunk path symlink");
    }
    return path;
}
fs::path FlowChunkStore::filePath(const std::string &id, int64_t day) const {
    const auto symbol = symbolOf(id);
    if (day < 0 || day % DayMs) throw std::invalid_argument("flow chunk day");
    return options_.root / "coinbase" / symbol /
        (QDateTime::fromMSecsSinceEpoch(day, QTimeZone::UTC).toString("yyyy-MM-dd").toStdString() + ".flc");
}
void FlowChunkStore::write(const fs::path &path, const std::vector<uint8_t> &data) {
    checked(path);
    std::vector<fs::path> missing;
    for (auto at = path.parent_path(); !fs::exists(at); at = at.parent_path()) missing.push_back(at);
    int error = 0;
    for (auto it = missing.rbegin(); it != missing.rend(); ++it) {
        fs::create_directory(*it);
        if (!sentinel::persistence::syncDirectory(it->parent_path(), error)) throw std::runtime_error("chunk mkdir sync");
    }
    checked(path);
    atomicFlowFile(path, QByteArray::fromRawData(reinterpret_cast<const char *>(data.data()), qsizetype(data.size())));
}
std::optional<chunks::DayFile> FlowChunkStore::load(const std::string &id, int64_t day,
                                                    std::optional<uint64_t> expectedRevision) {
    if (options_.beforeDayLoadForTest) options_.beforeDayLoadForTest(day);
    try {
        auto result = chunks::parseDayFile(bytes(checked(filePath(id, day))));
        if (result.instrument != id || result.dayStartMs != day) throw std::runtime_error("chunk file identity");
        return result;
    } catch (const std::exception &e) {
        bool log = false;
        { std::lock_guard lock(mutex_); auto &d = instruments_.at(id).days[day];
          if (!expectedRevision || d.indexRevision == *expectedRevision) {
              log = d.hours != 0; const auto next = d.indexRevision + 1; d = {0, false, true}; d.indexRevision = next; changed(id, day);
          } }
        if (log) sLog_Warning("Flow chunk withdrawn instrument=" << id << " day=" << day << " error=" << e.what());
        if (log) wake();
        return {};
    }
}
namespace {
bool chunkTempName(std::string_view name) {
    // QSaveFile uses <target>.<six random characters> in the target directory.
    const auto dot = name.rfind('.');
    if (dot == std::string_view::npos || name.size() - dot != 7) return false;
    const auto target = name.substr(0, dot);
        if (target != "epoch" &&
        !(target.size() == 14 && target.substr(10) == ".flc" &&
          QDate::fromString(QString::fromStdString(std::string(target.substr(0, 10))), "yyyy-MM-dd").isValid()) &&
        !(target.size() == 18 && target.substr(10) == ".flc.loc" &&
          QDate::fromString(QString::fromStdString(std::string(target.substr(0, 10))), "yyyy-MM-dd").isValid()))
        return false;
    return std::all_of(name.begin() + ptrdiff_t(dot + 1), name.end(), [](unsigned char ch) {
        return std::isalnum(ch);
    });
}
}
void FlowChunkStore::initializeInstrument(const std::string &id, const FlowStore::Archive &archive) {
    { std::lock_guard lock(mutex_); if (instruments_.at(id).initialized) return; }
    auto days = archive.days;
    const auto directory = checked(filePath(id, 0)).parent_path();
    if (fs::is_directory(directory)) {
        uint64_t removed = 0;
        for (const auto &entry : fs::directory_iterator(directory)) {
            const auto status = entry.symlink_status();
            if (!fs::is_regular_file(status)) continue;
            if (chunkTempName(entry.path().filename().string())) {
                fs::remove(checked(entry.path())); ++removed; continue;
            }
            if (entry.path().extension() != ".flc") continue;
            const auto date = QDate::fromString(QString::fromStdString(entry.path().stem().string()), "yyyy-MM-dd");
            if (date.isValid()) days.push_back(date.startOfDay(QTimeZone::UTC).toMSecsSinceEpoch());
        }
        if (removed) {
            int error = 0;
            if (!sentinel::persistence::syncDirectory(directory, error)) throw std::runtime_error("chunk temp cleanup sync");
            sLog_Data("Flow chunk stale temps removed instrument=" << id << " count=" << removed);
        }
    }
    std::sort(days.begin(), days.end()); days.erase(std::unique(days.begin(), days.end()), days.end());
    std::optional<chunks::EpochState> stored;
    try { stored = chunks::parseEpochFile(bytes(checked(directory / "epoch"))); }
    catch (const std::exception &e) { sLog_Data("Flow epoch recovery instrument=" << id << " reason=" << e.what()); }
    auto rules = options_.rules;
    if (options_.bigFloors.contains(id)) rules.bigFloorQuote = options_.bigFloors.at(id);
    if (stored) {
        days.insert(days.end(), stored->pending.begin(), stored->pending.end());
        std::sort(days.begin(), days.end()); days.erase(std::unique(days.begin(), days.end()), days.end());
    }
    auto next = chunks::startupTransition(stored, chunks::ruleIdentity(rules), days, uint64_t(sources_.options().nowMs()));
    // Nothing can be served until the new epoch is durable.
    write(directory / "epoch", chunks::encodeEpochFile(next));
    { std::lock_guard lock(mutex_); auto &i = instruments_.at(id); i.state = next; i.initialized = true;
      i.oldestSourceDay = archive.days.empty() ? INT64_MAX : archive.days.front(); changed(id, {});
      for (auto day : days) i.days.try_emplace(day); }
    for (auto day : days) if (auto file = load(id, day)) {
        publish(id, *file);
        auto locators = loadLocators(id, *file);
        std::lock_guard lock(mutex_);
        instruments_.at(id).days.at(day).sources = std::move(locators);
    }
}
void FlowChunkStore::initialize(std::stop_token stop) {
    std::lock_guard pass(passMutex_);
    for (const auto &symbol : sources_.options().symbols) {
        check(stop); initializeInstrument("coinbase:" + symbol, sharedArchive(symbol, stop));
    }
}
void FlowChunkStore::publish(const std::string &id, const chunks::DayFile &file,
                             std::optional<uint64_t> expectedRevision) {
    const auto path = checked(filePath(id, file.dayStartMs));
    const auto size = fs::file_size(path);
    const auto modified = fs::last_write_time(path);
    uint64_t serial = 0;
    bool servable = false, notifyLive = false;
    {
        std::lock_guard lock(mutex_);
        auto &i = instruments_.at(id);
        auto &d = i.days[file.dayStartMs];
        if (expectedRevision && d.indexRevision != *expectedRevision) return;
        const auto revision = d.indexRevision + 1;
        d = {}; d.indexRevision = revision; changed(id, file.dayStartMs);
        serial = i.serial;
        if (chunks::servable(i.state, file)) {
            servable = true;
            // Only the newest source day and its predecessor can intersect a
            // six-hour live window. Avoid decoding historical backfill days.
            notifyLive = !i.days.empty() &&
                file.dayStartMs >= i.days.rbegin()->first - DayMs;
            for (int h = 0; h < 24; ++h) if (file.sections[h].sealed) d.hours |= 1u << h;
            d.coarse = file.sections[24].sealed && file.sections[25].sealed;
            d.fileSize = size; d.fileModified = modified;
            uint64_t offset = 12 + 1 + id.size() + 8 + 32 + 1 + 4 + 12 + 8 + 4 +
                              chunks::DaySections * chunks::TableEntryBytes;
            for (size_t n = 0; n < d.entries.size(); ++n) {
                const auto &section = file.sections[n];
                d.entries[n] = {uint32_t(offset), uint32_t(section.payload.size()), section.summaryLength,
                                section.payload.empty() ? 0u : sentinel::crc32(section.payload.data(), section.payload.size()),
                                section.stepExponent};
                offset += section.payload.size();
            }
        }
    }
    if (!servable || !notifyLive || !options_.onDayPublished) return;
    std::vector<flow::FlowBar> bars;
    std::vector<int64_t> hours;
    for (int h = 0; h < 24; ++h) {
        const auto &section = file.sections[h];
        if (!section.sealed) continue;
        const auto hour = file.dayStartMs + h * Hour;
        hours.push_back(hour);
        auto decoded = chunks::toBars(chunks::decode(section.payload, Minute, hour, 60, false,
                                                     uint64_t(1) << section.stepExponent));
        bars.insert(bars.end(), std::make_move_iterator(decoded.begin()), std::make_move_iterator(decoded.end()));
    }
    options_.onDayPublished(id, file.dayStartMs, serial, std::move(bars), std::move(hours));
}
fs::path FlowChunkStore::locatorPath(const std::string &id, int64_t day) const {
    return filePath(id, day).string() + ".loc";
}
void FlowChunkStore::saveLocators(const std::string &id, const chunks::DayFile &file,
                                  const std::array<std::optional<SourceCheck>, 24> &sources) {
    const auto encoded = chunks::encodeDayFile(file);
    nlohmann::json loc = {{"version", 1}, {"instrument", id}, {"day", file.dayStartMs},
        {"file_crc", sentinel::crc32(encoded.data(), encoded.size())}, {"hours", nlohmann::json::array()}};
    for (size_t h = 0; h < sources.size(); ++h) {
        if (!file.sections[h].sealed || !sources[h]) { loc["hours"].push_back(nullptr); continue; }
        const auto &s = *sources[h];
        loc["hours"].push_back({{"cursor", s.cursor}, {"first", s.first}, {"last", s.last},
            {"stamp", s.stamp}, {"prefix", s.prefix}, {"source", int(s.source)}});
    }
    const auto body = loc.dump();
    loc["crc"] = sentinel::crc32(body.data(), body.size());
    const auto text = loc.dump();
    write(locatorPath(id, file.dayStartMs), {text.begin(), text.end()});
}
std::array<std::optional<FlowChunkStore::SourceCheck>, 24> FlowChunkStore::loadLocators(
    const std::string &id, const chunks::DayFile &file) {
    std::array<std::optional<SourceCheck>, 24> out;
    try {
        const auto raw = bytes(checked(locatorPath(id, file.dayStartMs)));
        auto loc = nlohmann::json::parse(raw);
        const auto crc = loc.at("crc").get<uint32_t>();
        loc.erase("crc");
        const auto body = loc.dump();
        if (crc != sentinel::crc32(body.data(), body.size())) return {};
        const auto encoded = chunks::encodeDayFile(file);
        if (loc.at("version") != 1 || loc.at("instrument") != id || loc.at("day") != file.dayStartMs ||
            loc.at("file_crc") != sentinel::crc32(encoded.data(), encoded.size()) ||
            !loc.at("hours").is_array() || loc.at("hours").size() != out.size()) return {};
        for (size_t h = 0; h < out.size(); ++h) {
            if (!file.sections[h].sealed) continue;
            const auto &item = loc.at("hours").at(h);
            const int source = item.at("source").get<int>();
            if (source < int(FlowStore::SourceState::Unknown) || source > int(FlowStore::SourceState::Absent)) return {};
            SourceCheck value{item.at("cursor").get<roller::JournalPos>(),
                item.at("first").get<roller::JournalPos>(), item.at("last").get<roller::JournalPos>(),
                item.at("stamp").get<uint64_t>(), item.at("prefix").get<uint64_t>(),
                FlowStore::SourceState(source)};
            if (value.prefix != file.sections[h].sourcePrefix) return {};
            value.boundaryVerified = false;
            out[h] = std::move(value);
        }
    } catch (const std::exception &) { return {}; }
    return out;
}
void FlowChunkStore::transition(const std::string &id, int64_t day) {
    chunks::EpochState next;
    { std::lock_guard lock(mutex_); auto &i = instruments_.at(id);
      // Startup rule changes and crash recovery already fenced this day and
      // advanced the counter. Its new seal point may change the prefix too;
      // finish that transition instead of starting another one per rebuilt day.
      if (std::binary_search(i.state.pending.begin(), i.state.pending.end(), day)) return;
      // Fence readers before disk I/O, but do not enter pending or advance
      // the counter until the epoch write succeeds. A failed write retries the
      // transition on the next pass and cannot publish new bytes under old epoch.
      i.days[day].invalid = true; ++i.days[day].indexRevision; changed(id, day);
      next = i.state; }
    next.pending.push_back(day);
    std::sort(next.pending.begin(), next.pending.end());
    if (next.counter == UINT64_MAX) throw std::overflow_error("flow epoch exhausted");
    ++next.counter;
    if (options_.beforeEpochWriteForTest) options_.beforeEpochWriteForTest();
    write(filePath(id, day).parent_path() / "epoch", chunks::encodeEpochFile(next));
    { std::lock_guard lock(mutex_); auto &i = instruments_.at(id); i.state = next;
      auto revision = i.days[day].indexRevision + 1; i.days[day] = {0, false, true};
      i.days[day].indexRevision = revision; changed(id, {}); }
    sLog_Data("Flow epoch changed instrument=" << id << " day=" << day << " counter=" << next.counter);
}
FlowChunkStore::HourResult FlowChunkStore::buildHour(const std::string &symbol, int64_t hour,
    const FlowStoreGridValues &grid, const roller::JournalReader &catalog, std::stop_token stop, bool evidence) {
    try {
    HourResult out;
    const auto warmup = hour - Minute;
    const auto cursor = scanCursor(symbol, warmup, catalog, stop);
    if (!cursor) return out;
    roller::JournalReader reader(catalog, cursor);
    roller::JournalFeed feed(symbol);
    const auto &s = grid.scales;
    flow::FlowBarBuilder builder(Minute, s.nativeTick, s.priceScale, s.baseScale, s.quoteScale);
    std::unordered_set<std::string> acceptedIds;
    std::optional<roller::JournalPos> first, last;
    int64_t watermark = INT64_MIN, firstReceive = INT64_MAX;
    bool partial = false;
    feed.onInvalid = [&](int64_t ms, const std::string &) { if (ms >= hour && ms < hour + Hour) partial = true; };
    feed.onTrade = [&](const ::Trade &value) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(value.timestamp.time_since_epoch()).count();
        if (ms < hour || ms >= hour + Hour) return;
        flow::Trade trade{value.trade_id, ms, value.price, value.size,
            value.side == AggressorSide::Buy ? flow::Side::Buy : value.side == AggressorSide::Sell ? flow::Side::Sell : flow::Side::Unknown};
        if (!trade.id.empty() && acceptedIds.contains(trade.id)) return;
        if (watermark >= ms / Minute * Minute + Minute + options_.rules.sealWindowMs) {
            ++out.late; if (evidence) out.excluded.push_back(trade); return;
        }
        if (builder.add(trade) == flow::FlowBarBuilder::AddResult::Accepted) {
            if (!trade.id.empty()) acceptedIds.insert(trade.id);
            if (evidence) out.accepted.push_back(std::move(trade));
        }
    };
    roller::JournalRecord input;
    while (reader.next(input)) {
        check(stop);
        const auto receive = input.record.time.systemNs / 1'000'000;
        if (!first && receive < warmup) continue;
        if (!first) { first = input.pos; firstReceive = receive; }
        if (receive < watermark) ++out.regressions;
        watermark = std::max(watermark, receive);
        feed.apply(input);
        if (options_.afterRecordForTest) options_.afterRecordForTest(stop);
        if (watermark >= hour + Hour + options_.rules.sealWindowMs) { last = input.pos; break; }
    }
    check(stop);
    if (!last) return {};
    if (reader.damaged()) throw std::runtime_error("damaged RAWL2 hour scan");
    out.rejected = uint32_t(builder.rejectedCount());
    partial |= out.rejected != 0 || firstReceive > hour;
    std::vector<chunks::CoverageRun> coverage;
    for (int m = 0; m < 60; ++m) {
        const auto source = sources_.sourceState(symbol, hour + m * Minute, hour + (m + 1) * Minute, true,
                                                 std::max<int64_t>(Minute, options_.rules.sealWindowMs));
        appendRun(coverage, uint16_t(m), source == FlowStore::SourceState::Absent ? chunks::Status::Unrecorded :
            partial || source == FlowStore::SourceState::ReceiveTailOnly ? chunks::Status::Partial : chunks::Status::Complete);
    }
    auto bars = builder.bars();
    out.section = section(bars, Minute, hour, 60, gridOf(grid), coverage);
    out.section.sourcePrefix = FlowStore::prefixIdentity(catalog, *first, *last, stop);
    out.source = SourceCheck{*cursor, *first, *last, sources_.archiveStamp(symbol, catalog, *cursor, *last, stop),
                              out.section.sourcePrefix,
                              sources_.sourceState(symbol, hour, hour + Hour, true,
                                  std::max<int64_t>(Minute, options_.rules.sealWindowMs))};
    return out;
    } catch (const Cancelled &) { throw; }
      catch (const std::exception &e) { throw HourUnavailable(e.what()); }
}
std::optional<FlowChunkStore::SourceCheck> FlowChunkStore::locateHour(const std::string &symbol, int64_t hour,
    const roller::JournalReader &catalog, std::stop_token stop, bool requireClosed) {
    if (options_.afterLocateForTest) options_.afterLocateForTest(hour);
    const auto cursor = scanCursor(symbol, hour - Minute, catalog, stop);
    if (!cursor) return {};
    const auto first = firstAtOrBeyond(symbol, catalog, *cursor, hour - Minute, stop);
    if (!first) return {};
    const auto last = firstAtOrBeyond(symbol, catalog, *first, hour + Hour + options_.rules.sealWindowMs, stop);
    if (!last) return {};
    if (requireClosed) {
        const auto &file = catalog.files().at(fileOf(catalog, *last));
        if (!file.superseded) {
            sentinel::capture::RecordReader probe(QString::fromStdString(file.path.string()), true, last->block);
            sentinel::capture::Record ignored;
            while (probe.next(ignored)) check(stop);
            if (!probe.result().indexed) return {}; // open tail: replay the hour after restart
        }
    }
    SourceCheck result{*cursor, *first, *last, sources_.archiveStamp(symbol, catalog, *cursor, *last, stop),
        FlowStore::prefixIdentity(catalog, *first, *last, stop),
        sources_.sourceState(symbol, hour, hour + Hour, true,
            std::max<int64_t>(Minute, options_.rules.sealWindowMs))};
    if (options_.afterLocatedForTest) options_.afterLocatedForTest(hour, result);
    return result;
}
void FlowChunkStore::finishPending(const std::string &id, int64_t day) {
    chunks::EpochState next;
    { std::lock_guard lock(mutex_); next = instruments_.at(id).state; }
    if (!std::binary_search(next.pending.begin(), next.pending.end(), day)) return;
    chunks::finishDay(next, day);
    write(filePath(id, day).parent_path() / "epoch", chunks::encodeEpochFile(next));
    { std::lock_guard lock(mutex_); instruments_.at(id).state = next; changed(id, day); }
}
void FlowChunkStore::buildDay(const std::string &id, int64_t day, const FlowStore::Archive &archive,
                               std::stop_token stop, FlowStore::PassResult &result) {
    const auto symbol = symbolOf(id);
    bool fileSuspect = false;
    uint32_t previousDamageFailures = 0;
    { std::lock_guard lock(mutex_);
      const auto &state = instruments_.at(id).days[day];
      fileSuspect = state.invalid; previousDamageFailures = state.damageFailures; }
    auto old = load(id, day);
    auto unavailableSource = [&] {
        if (old) {
            transition(id, day);
            throw std::runtime_error("RAWL2 no longer reaches a previously sealed day");
        }
        finishPending(id, day);
    };
    if (!archive.catalog) { unavailableSource(); return; }
    const bool sealable = sealReached(day + Hour + options_.rules.sealWindowMs, *archive.catalog, stop);
    // A source-only forming day has no sealed bytes to revalidate. It remains
    // unavailable, but does not keep an archive recovery epoch pending forever.
    if (!sealable) { unavailableSource(); return; }
    const auto grid = sources_.gridFor(symbol, day, *archive.catalog, stop);
    chunks::DayFile file;
    file.instrument = id; file.dayStartMs = day; file.grid = gridOf(grid);
    file.sealWindowMs = options_.rules.sealWindowMs; file.ruleVersion = options_.rules.builderVersion;
    { std::lock_guard lock(mutex_); file.ruleIdentity = instruments_.at(id).state.ruleIdentity; }
    // Recompute header counters along with every canonical hour; there are no
    // per-hour counters on disk to recover incrementally after a restart.
    std::array<std::optional<SourceCheck>, 24> dependencies;
    { std::lock_guard lock(mutex_); dependencies = instruments_.at(id).days[day].sources; }
    uint64_t built = 0, newlySealed = 0, newlyLate = 0, newlyRegressions = 0;
    uint32_t badHours = 0;
    bool changed = old && old->grid != file.grid;
    bool reuse = old && !fileSuspect && !changed && old->ruleIdentity == file.ruleIdentity;
    if (reuse) for (int h = 0; h < 24; ++h) if (old->sections[h].sealed) {
        try {
        const auto hour = day + h * Hour;
        const auto cursor = scanCursor(symbol, hour - Minute, *archive.catalog, stop);
        const auto state = sources_.sourceState(symbol, hour, hour + Hour, true,
            std::max<int64_t>(Minute, options_.rules.sealWindowMs));
        if (dependencies[h] && !dependencies[h]->boundaryVerified) {
            if (!validateCachedBoundaries(*archive.catalog, *dependencies[h], stop)) dependencies[h].reset();
            else dependencies[h]->boundaryVerified = true;
        }
        if (!dependencies[h] || !cursor || dependencies[h]->cursor != *cursor ||
            dependencies[h]->source != state ||
            sources_.archiveStamp(symbol, *archive.catalog, *cursor, dependencies[h]->last, stop) != dependencies[h]->stamp) {
            dependencies[h] = locateHour(symbol, hour, *archive.catalog, stop, !dependencies[h]);
        }
        if (!dependencies[h] || dependencies[h]->prefix != old->sections[h].sourcePrefix) { reuse = false; break; }
        } catch (const Cancelled &) { throw; }
          catch (const std::exception &) { reuse = false; break; }
    }
    if (reuse) {
        file.lateTrades = old->lateTrades;
        file.rejectedTrades = old->rejectedTrades;
        file.regressions = old->regressions;
    } else dependencies = {};
    for (int h = 0; h < 24; ++h) {
        check(stop);
        const auto hour = day + h * Hour;
        if (reuse && old->sections[h].sealed) { file.sections[h] = old->sections[h]; ++built; continue; }
        HourResult scan;
        try { scan = buildHour(symbol, hour, grid, *archive.catalog, stop); }
        catch (const HourUnavailable &e) {
            ++result.failed;
            if (auto *counter = instruments_.at(id).failures) counter->inc();
            sLog_Warning("Flow chunk hour unavailable instrument=" << id << " hour=" << hour << " error=" << e.what());
            badHours |= 1u << h;
            if (old && old->sections[h].sealed) changed = true;
            continue;
        }
        if (!scan.section.sealed) {
            if (old && old->sections[h].sealed) changed = true;
            continue;
        }
        if (old && old->sections[h].sealed && (old->sections[h].sourcePrefix != scan.section.sourcePrefix ||
            (old->ruleIdentity == file.ruleIdentity && old->sections[h] != scan.section))) changed = true;
        dependencies[h] = scan.source;
        file.sections[h] = std::move(scan.section);
        file.lateTrades += scan.late; file.rejectedTrades += scan.rejected; file.regressions += scan.regressions;
        ++built;
        if (!old || !old->sections[h].sealed) {
            ++newlySealed; newlyLate += scan.late; newlyRegressions += scan.regressions;
        }
    }
    check(stop);
    if (changed) transition(id, day);
    auto rememberDamage = [&] {
        std::lock_guard lock(mutex_);
        auto &d = instruments_.at(id).days[day];
        d.badHours = badHours;
        if (badHours) {
            d.damageFailures = std::min<uint32_t>(previousDamageFailures + 1, 6);
            d.retryAfter = options_.schedulerNow() + options_.damageRetry * (1u << (d.damageFailures - 1));
        } else { d.damageFailures = 0; d.retryAfter = {}; }
    };
    if (!built) { rememberDamage(); return; }
    compose(file);
    // Refresh the inventory after replay. A new hour file beyond this day's
    // seal range is harmless; a rewrite or insertion within it is not.
    const auto after = sharedArchive(symbol, stop);
    if (!after.catalog) throw std::runtime_error("RAWL2 disappeared during chunk build");
    for (int h = 0; after.integrity != archive.integrity && h < 24; ++h) if (file.sections[h].sealed) {
        const auto hour = day + h * Hour;
        const auto cursor = scanCursor(symbol, hour - Minute, *after.catalog, stop);
        if (!cursor || !dependencies[h]) throw std::runtime_error("RAWL2 changed during chunk build");
        const auto stamp = sources_.archiveStamp(symbol, *after.catalog, *cursor, dependencies[h]->last, stop);
        if (*cursor == dependencies[h]->cursor && stamp == dependencies[h]->stamp) continue;
        auto located = locateHour(symbol, hour, *after.catalog, stop);
        if (!located || located->prefix != file.sections[h].sourcePrefix)
            throw std::runtime_error("RAWL2 changed during chunk build");
        if (located->stamp != dependencies[h]->stamp)
            throw std::runtime_error("RAWL2 source identity changed during chunk build");
    }
    if (!old || *old != file) write(filePath(id, day), chunks::encodeDayFile(file));
    try { saveLocators(id, file, dependencies); }
    catch (const std::exception &e) {
        sLog_Warning("Flow chunk locator cache unavailable instrument=" << id << " day=" << day << " error=" << e.what());
    }
    finishPending(id, day);
    publish(id, file);
    { std::lock_guard lock(mutex_); instruments_.at(id).days[day].sources = std::move(dependencies); }
    rememberDamage();
    auto &i = instruments_.at(id); // registry references are immutable after construction
    if (i.late) i.late->inc(newlyLate);
    if (i.regressions) i.regressions->inc(newlyRegressions);
    if (i.hours) i.hours->inc(newlySealed);
    result.built += newlySealed;
    sLog_Data("Flow chunks built instrument=" << id << " day=" << day << " hours=" << built
              << " late=" << file.lateTrades << " regressions=" << file.regressions);
    if (options_.afterDayForTest) options_.afterDayForTest(day);
}
bool FlowChunkStore::unchanged(const std::string &id, int64_t day, const FlowStore::Archive &archive,
                                std::stop_token stop) {
    if (!archive.catalog) return false;
    try {
    Day saved;
    { std::lock_guard lock(mutex_); saved = instruments_.at(id).days.at(day); }
    if (!saved.hours || saved.invalid) return false;
    const auto symbol = symbolOf(id);
    auto file = load(id, day);
    if (!file) return false;
    bool upgraded = false;
    int firstMissing = -1;
    for (int h = 0; h < 24; ++h) {
        check(stop);
        if (!(saved.hours & (1u << h))) {
            if (firstMissing < 0) firstMissing = h;
            continue;
        }
        const auto hour = day + h * Hour;
        const auto cursor = scanCursor(symbol, hour - Minute, *archive.catalog, stop, options_.afterCursorRecordForTest);
        if (!cursor) return false;
        const auto state = sources_.sourceState(symbol, hour, hour + Hour, true,
            std::max<int64_t>(Minute, options_.rules.sealWindowMs));
        if (saved.sources[h] && !saved.sources[h]->boundaryVerified) {
            if (!validateCachedBoundaries(*archive.catalog, *saved.sources[h], stop)) saved.sources[h].reset();
            else saved.sources[h]->boundaryVerified = true;
        }
        if (!saved.sources[h] || saved.sources[h]->cursor != *cursor || saved.sources[h]->source != state ||
            sources_.archiveStamp(symbol, *archive.catalog, *cursor, saved.sources[h]->last, stop) != saved.sources[h]->stamp) {
            // A restart has no in-memory positions. Locate the first and seal
            // records without feeding trades, then compare the canonical
            // prefix with the durable table entry.
            auto located = locateHour(symbol, hour, *archive.catalog, stop, !saved.sources[h]);
            if (!located || located->prefix != file->sections[h].sourcePrefix) return false;
            saved.sources[h] = std::move(located);
            upgraded = true;
        }
    }
    if (firstMissing >= 0) {
        // Poll the forming hour without rebuilding all of today's sealed
        // hours and their counters on every wake. A new seal resumes replay.
        const auto hour = day + firstMissing * Hour;
        if (sealReached(hour + Hour + options_.rules.sealWindowMs, *archive.catalog, stop)) return false;
    }
    { std::lock_guard lock(mutex_);
      auto &sources = instruments_.at(id).days.at(day).sources;
      sources = saved.sources; }
    if (upgraded) {
        try { saveLocators(id, *file, saved.sources); }
        catch (const std::exception &e) {
            sLog_Warning("Flow chunk locator refresh unavailable instrument=" << id << " day=" << day << " error=" << e.what());
        }
    }
    return file->grid == gridOf(sources_.gridFor(symbol, day, *archive.catalog, stop));
    } catch (const std::exception &) { check(stop); return false; }
}
FlowStore::PassResult FlowChunkStore::runPass(std::stop_token stop) {
    std::lock_guard pass(passMutex_);
    FlowStore::PassResult result;
    std::map<std::string, FlowStore::Archive> archives;
    // Fences for all products become durable before any long replay.
    for (const auto &symbol : sources_.options().symbols) {
        const auto id = "coinbase:" + symbol;
        try {
            check(stop);
            auto archive = sharedArchive(symbol, stop);
            initializeInstrument(id, archive);
            archives.emplace(symbol, std::move(archive));
        } catch (const Cancelled &) { result.stopped = true; return result; }
          catch (const std::exception &e) {
            if (stop.stop_requested()) { result.stopped = true; return result; }
            ++result.failed;
            if (auto *counter = instruments_.at(id).failures) counter->inc();
            sLog_Warning("Flow chunk discovery failed instrument=" << id << " error=" << e.what());
        }
    }
    std::set<std::string> incomplete;
    { std::lock_guard lock(mutex_);
      for (const auto& [symbol, archive] : archives) {
          auto& i = instruments_.at("coinbase:" + symbol);
          const auto oldest = archive.days.empty() ? INT64_MAX : archive.days.front();
          if (i.oldestSourceDay != oldest) { i.oldestSourceDay = oldest; changed("coinbase:" + symbol, {}); }
      }
    }
    const auto checkedArchives = archives; // later priority refreshes cannot certify earlier day checks
    using Attempt = std::tuple<std::string, int64_t, uint64_t>;
    std::set<Attempt> attempted;
    std::set<std::pair<std::string, int64_t>> failedThisPass;
    auto liveDays = [&](const std::string &symbol, const FlowStore::Archive &archive) {
        std::vector<int64_t> days;
        if (archive.days.empty()) return days;
        int64_t watermark = archive.days.back();
        if (archive.catalog) for (const auto &file : archive.catalog->files())
            watermark = std::max(watermark, file.openedNs / 1'000'000);
        // The model's retention floor is derived from its receive watermark.
        // Before the model publishes one, the newest file's receive hour gives
        // a conservative floor (up to one extra hour of priority work).
        const auto fallback = std::max<int64_t>(0, (watermark - Minute) / Hour * Hour - 6 * Hour);
        const auto liveFloor = sources_.liveRetentionFloor(symbol);
        const auto floor = liveFloor.value_or(fallback);
        const auto now = liveFloor ? sources_.options().nowMs() : watermark;
        for (auto day : archive.days)
            if (day <= now && day + DayMs > floor) days.push_back(day);
        return days;
    };
    auto needsWork = [&](const std::string &id, int64_t day, const FlowStore::Archive &archive) {
        uintmax_t size;
        fs::file_time_type modified;
        bool needs;
        { std::lock_guard lock(mutex_);
          const auto &i = instruments_.at(id);
          const auto &d = i.days.at(day);
          needs = i.integrity != archive.integrity || !d.coarse || d.invalid ||
              std::binary_search(i.state.pending.begin(), i.state.pending.end(), day);
          size = d.fileSize; modified = d.fileModified; }
        if (needs) return true;
        try {
            const auto path = checked(filePath(id, day));
            return size != fs::file_size(path) || modified != fs::last_write_time(path);
        } catch (const std::exception &) { return true; }
    };
    auto process = [&](const std::string &symbol, int64_t day, FlowStore::Archive &archive) {
        const auto id = "coinbase:" + symbol;
        if (failedThisPass.contains({id, day})) return;
        if (!attempted.emplace(id, day, archive.generation).second) return;
        { std::lock_guard lock(mutex_);
          const auto &d = instruments_.at(id).days.at(day);
          if ((d.badHours || d.buildFailures) && options_.schedulerNow() < d.retryAfter) { incomplete.insert(id); return; } }
        bool work = needsWork(id, day, archive);
        if (!work) return;
        try {
            bool build = false;
            { std::lock_guard lock(mutex_);
              const auto &i = instruments_.at(id);
              const auto &d = i.days.at(day);
              build = i.integrity != archive.integrity || !d.coarse || d.invalid ||
                  std::binary_search(i.state.pending.begin(), i.state.pending.end(), day); }
            if (!build) {
                if (auto file = load(id, day)) publish(id, *file);
                else build = true;
            }
            if (build && !unchanged(id, day, archive, stop)) buildDay(id, day, archive, stop, result);
            { std::lock_guard lock(mutex_);
              auto &d = instruments_.at(id).days.at(day);
              if (d.badHours) { failedThisPass.emplace(id, day); incomplete.insert(id); }
              if (!d.badHours) { d.buildFailures = 0; d.retryAfter = {}; } }
        } catch (const Cancelled &) { throw; }
          catch (const std::exception &e) {
            if (stop.stop_requested()) throw Cancelled{};
            ++result.failed;
            failedThisPass.emplace(id, day); incomplete.insert(id);
            auto &i = instruments_.at(id);
            if (i.failures) i.failures->inc();
            { std::lock_guard lock(mutex_);
              auto &d = i.days.at(day);
              d.buildFailures = std::min<uint32_t>(d.buildFailures + 1, 6);
              d.retryAfter = options_.schedulerNow() + options_.damageRetry * (1u << (d.buildFailures - 1)); }
            sLog_Warning("Flow chunk build failed instrument=" << id << " day=" << day << " error=" << e.what());
        }
    };
    std::map<std::string, std::vector<int64_t>> priorityDays;
    auto priority = [&] {
        for (auto &[symbol, archive] : archives) {
            check(stop);
            const auto id = "coinbase:" + symbol;
            { std::lock_guard lock(mutex_);
              for (auto day : archive.days) instruments_.at(id).days.try_emplace(day); }
            auto days = liveDays(symbol, archive);
            priorityDays[symbol] = days;
            for (auto it = days.rbegin(); it != days.rend(); ++it) process(symbol, *it, archive);
        }
    };
    auto lastPriorityCheck = options_.schedulerNow();
    try {
        priority();
        // A refresh is needed only ahead of real older work, and no more often
        // than once per 30 s. Closed days that need nothing cost no inventories.
        for (auto &[symbol, archive] : archives) {
            check(stop);
            const auto id = "coinbase:" + symbol;
            std::vector<int64_t> days;
            { std::lock_guard lock(mutex_);
              for (const auto &[day, _] : instruments_.at(id).days) days.push_back(day); }
            for (auto it = days.rbegin(); it != days.rend(); ++it) {
                check(stop);
                const auto &live = priorityDays.at(symbol);
                if (std::binary_search(live.begin(), live.end(), *it)) continue;
                if (!needsWork(id, *it, archive)) continue;
                if (options_.schedulerNow() - lastPriorityCheck >= std::chrono::seconds(30)) {
                    lastPriorityCheck = options_.schedulerNow();
                    for (auto &[freshSymbol, known] : archives) {
                        check(stop);
                        try { known = sharedArchive(freshSymbol, stop); }
                        catch (const std::exception &e) {
                            ++result.failed;
                            incomplete.insert("coinbase:" + freshSymbol);
                            sLog_Warning("Flow chunk priority refresh failed instrument=" << freshSymbol << " error=" << e.what());
                        }
                    }
                    priority();
                }
                process(symbol, *it, archive);
            }
        }
    } catch (const Cancelled &) { result.stopped = true; }
    for (auto &[symbol, archive] : archives) {
        const auto id = "coinbase:" + symbol;
        // Record only the inventory revision checked across the pass's days.
        // A build's later archive refresh must force another pass to re-check
        // already-processed days before that revision is accepted.
        std::lock_guard lock(mutex_);
        auto &i = instruments_.at(id);
        if (!result.stopped && !incomplete.contains(id)) {
            const auto& checked = checkedArchives.at(symbol);
            i.generation = checked.generation; i.integrity = checked.integrity;
        }
        uint64_t remaining = 0;
        const auto newest = i.days.empty() ? -1 : i.days.rbegin()->first;
        for (const auto &[day, d] : i.days)
            if (((day != newest || d.badHours) && !d.coarse) ||
                std::binary_search(i.state.pending.begin(), i.state.pending.end(), day)) ++remaining;
        if (i.pending) i.pending->set(double(remaining));
    }
    result.stopped |= stop.stop_requested();
    return result;
}
FlowStore::PassResult FlowChunkStore::rebuildDay(const std::string &id, int64_t day, std::stop_token stop) {
    std::lock_guard pass(passMutex_);
    const auto archive = sharedArchive(symbolOf(id), stop);
    initializeInstrument(id, archive);
    FlowStore::PassResult result;
    try { buildDay(id, day, archive, stop, result); }
    catch (const Cancelled &) { result.stopped = true; }
    return result;
}
uint64_t FlowChunkStore::epoch(const std::string &id) const {
    std::lock_guard lock(mutex_);
    const auto it = instruments_.find(id);
    return it == instruments_.end() || !it->second.initialized ? 0 : chunks::epochOf(chunks::flowDomain(id), it->second.state);
}
std::optional<FlowChunkStore::Payload> FlowChunkStore::read(const std::string &id, int64_t level, int64_t start) {
    if (start < 0 || (level != Minute && level != Hour && level != DayMs) ||
        heatmap::chunkStartOf(level, start) != start) return {};
    const auto initial = stamp(id, level, start);
    if (!initial) return {};
    const auto before = initial->epoch;
    const auto symbol = symbolOf(id);
    const auto day = start / DayMs * DayMs;
    const auto count = level == DayMs ? 16 : 1;
    std::vector<std::vector<uint8_t>> pieces(16);
    std::vector<uint64_t> steps(16, 1);
    Payload out; out.epoch = before; out.stamp = *initial;
    for (int n = 0; n < count; ++n) {
        const auto date = day + n * DayMs;
        bool known = false, ready = false;
        Day::Entry section;
        uintmax_t fileSize = 0;
        fs::file_time_type fileModified{};
        uint64_t revision = 0;
        const size_t entry = level == Minute ? size_t((start - day) / Hour) : level == Hour ? 24 : 25;
        { std::lock_guard lock(mutex_); const auto &i = instruments_.at(id);
          if (std::binary_search(i.state.pending.begin(), i.state.pending.end(), date)) return {};
          const auto it = i.days.find(date);
          known = it != i.days.end();
          if (known) {
              const auto &indexed = it->second;
              ready = !indexed.invalid && (level == Minute ?
                  (indexed.hours & (1u << ((start - day) / Hour))) != 0 : indexed.coarse);
              section = indexed.entries[entry]; fileSize = indexed.fileSize;
              fileModified = indexed.fileModified; revision = indexed.indexRevision;
          } }
        if (!known && level == DayMs && sources_.sourceState(symbol, date, date + DayMs) == FlowStore::SourceState::Absent) continue;
        if (!ready) return {};
        auto revalidate = [&] {
            if (options_.beforeReadInvalidationForTest) options_.beforeReadInvalidationForTest();
            try { if (auto fresh = load(id, date, revision)) publish(id, *fresh, revision); }
            catch (const std::exception &) {}
            wake();
        };
        try {
            const auto path = checked(filePath(id, date));
            if (!section.length || fs::file_size(path) != fileSize ||
                fs::last_write_time(path) != fileModified) { revalidate(); return {}; }
            QFile file(QString::fromStdString(path.string()));
            if (!file.open(QIODevice::ReadOnly) || !file.seek(section.offset)) { revalidate(); return {}; }
            const auto payload = file.read(section.length);
            if (payload.size() != section.length ||
                sentinel::crc32(payload.data(), size_t(payload.size())) != section.crc ||
                fs::file_size(path) != fileSize || fs::last_write_time(path) != fileModified) {
                revalidate(); return {};
            }
            std::vector<uint8_t> part(reinterpret_cast<const uint8_t *>(payload.data()),
                                      reinterpret_cast<const uint8_t *>(payload.data()) + payload.size());
            if (level == DayMs) { pieces[n] = std::move(part); steps[n] = uint64_t(1) << section.step; }
            else { out.bytes = std::move(part); out.summaryLength = section.summary; out.stepExponent = section.step; }
        } catch (const std::exception &) { revalidate(); return {}; }
    }
    if (level == DayMs) {
        const auto data = chunks::assembleDayChunk(start, pieces, chunks::Status::Unrecorded, steps);
        out.bytes = chunks::encode(data, false, &out.summaryLength);
        out.stepExponent = uint8_t(std::countr_zero(data.priceStep));
    }
    if (options_.afterSectionReadForTest) options_.afterSectionReadForTest();
    if (!ifCurrent(id, level, start, *initial, [] {})) return {};
    return out;
}
std::optional<std::pair<uint64_t, std::array<std::vector<FlowChunkStore::Range>, 3>>>
FlowChunkStore::scanLevels(const std::string& id) {
    struct IndexedDay {
        int64_t day; uintmax_t size; fs::file_time_type modified; uint64_t revision;
    };
    std::array<std::set<int64_t>, 3> candidates;
    std::set<int64_t> coarseDays, allDays;
    int64_t oldestSourceDay = INT64_MAX;
    std::vector<IndexedDay> days;
    uint64_t before = 0, serial = 0;
    { std::lock_guard lock(mutex_);
      const auto found = instruments_.find(id);
      if (found == instruments_.end() || !found->second.initialized)
          return std::pair{uint64_t(0), std::array<std::vector<Range>, 3>{}};
      const auto& i = found->second;
      before = chunks::epochOf(chunks::flowDomain(id), i.state); serial = i.serial;
      oldestSourceDay = i.oldestSourceDay;
      for (const auto& [day, d] : i.days) {
          allDays.insert(day);
          if (d.invalid || std::binary_search(i.state.pending.begin(), i.state.pending.end(), day)) continue;
          days.push_back({day, d.fileSize, d.fileModified, d.indexRevision});
          if (d.coarse) {
              coarseDays.insert(day);
              candidates[1].insert(day);
              candidates[2].insert(heatmap::chunkStartOf(DayMs, day));
          }
          for (int h = 0; h < 24; ++h)
              if (d.hours & (1u << h)) candidates[0].insert(day + h * Hour);
      }
    }
    // One stat per indexed day for all three levels, regardless of subscriber count.
    std::set<int64_t> validDays;
    for (const auto& [day, size, modified, revision] : days) {
        try {
            const auto path = checked(filePath(id, day));
            if (options_.afterAvailabilityStatForTest) options_.afterAvailabilityStatForTest();
            if (fs::file_size(path) != size || fs::last_write_time(path) != modified)
                throw std::runtime_error("flow chunk file changed");
            validDays.insert(day);
        } catch (const std::exception&) {
            try { if (const auto fresh = load(id, day, revision)) publish(id, *fresh, revision); }
            catch (const std::exception&) {}
            wake();
        }
    }
    std::array<std::vector<Range>, 3> levels;
    for (size_t level = 0; level < levels.size(); ++level) {
        const auto span = heatmap::chunkSpanMs(heatmap::kChunkLevels[level]);
        for (const auto start : candidates[level]) {
            bool ready = level != 2 ? validDays.contains(start / DayMs * DayMs) : true;
            if (level == 2) for (int n = 0; n < 16 && ready; ++n) {
                const auto day = start + n * DayMs;
                if (validDays.contains(day) && coarseDays.contains(day)) continue;
                if (allDays.contains(day) || day > oldestSourceDay ||
                    sources_.sourceState(symbolOf(id), day, day + DayMs) !=
                                             FlowStore::SourceState::Absent) ready = false;
            }
            if (!ready) continue;
            auto& ranges = levels[level];
            if (!ranges.empty() && ranges.back().toMs == start) ranges.back().toMs += span;
            else ranges.push_back({start, start + span});
        }
    }
    if (options_.afterAvailabilityReadForTest) options_.afterAvailabilityReadForTest();
    { std::lock_guard lock(mutex_);
      const auto& i = instruments_.at(id);
      if (i.serial != serial || chunks::epochOf(chunks::flowDomain(id), i.state) != before) return {};
    }
    return std::pair{before, std::move(levels)};
}
FlowChunkStore::Availability FlowChunkStore::sealed(const std::string& id, int64_t level) {
    if (level != Minute && level != Hour && level != DayMs) return {};
    auto scanned = scanLevels(id);
    if (!scanned) return {};
    const auto n = level == Minute ? 0u : level == Hour ? 1u : 2u;
    return {scanned->first, std::move(scanned->second[n])};
}
void FlowChunkStore::changed(const std::string& id, std::optional<int64_t> day) {
    const auto serial = ++instruments_.at(id).serial;
    if (invalidate_) invalidate_(id, day);
    if (options_.onIndexChange) options_.onIndexChange(id, day, serial);
}
void FlowChunkStore::onInvalidation(std::function<void(const std::string&, std::optional<int64_t>)> handler) {
    std::lock_guard lock(mutex_); invalidate_ = std::move(handler);
}
std::optional<FlowChunkStore::Stamp> FlowChunkStore::stampLocked(const std::string& id, int64_t level, int64_t start) const {
    const auto span = heatmap::chunkSpanMs(level);
    if (!span || start < kHmc2MinMs || start > kHmc2EndMs - span || !heatmap::isChunkStart(level, start)) return {};
    const auto found = instruments_.find(id);
    if (found == instruments_.end() || !found->second.initialized) return {};
    const auto& i = found->second;
    Stamp out{chunks::epochOf(chunks::flowDomain(id), i.state), {}};
    const auto day = start / DayMs * DayMs;
    for (int n = 0; n < (level == DayMs ? 16 : 1); ++n) {
        const auto date = day + n * DayMs;
        if (std::binary_search(i.state.pending.begin(), i.state.pending.end(), date)) return {};
        const auto it = i.days.find(date);
        if (it == i.days.end()) {
            if (level != DayMs) return {};
            // Only days before the first RAWL2 day are stable unrecorded slots.
            if (date > i.oldestSourceDay) return {};
            out.days.push_back(0); continue; // read() must prove absence from RAWL2
        }
        const auto& d = it->second;
        if (d.invalid || (level == Minute ? !(d.hours & (1u << ((start - day) / Hour))) : !d.coarse)) return {};
        out.days.push_back(d.indexRevision);
    }
    return out;
}
std::optional<FlowChunkStore::Stamp> FlowChunkStore::stamp(const std::string& id, int64_t level, int64_t start) const {
    std::lock_guard lock(mutex_); return stampLocked(id, level, start);
}
bool FlowChunkStore::verify(const std::string& id, int64_t level, int64_t start, const Stamp& expected) {
    if (options_.beforeVerifyForTest) options_.beforeVerifyForTest();
    if (options_.rejectVerifyForTest && options_.rejectVerifyForTest()) return false;
    struct File { int64_t day; uintmax_t size; fs::file_time_type modified; uint64_t revision; };
    std::vector<File> files;
    if (!ifCurrent(id, level, start, expected, [&] {
        const auto& i = instruments_.at(id);
        const auto day = start / DayMs * DayMs;
        for (size_t n = 0; n < expected.days.size(); ++n) {
            const auto date = day + int64_t(n) * DayMs;
            if (const auto it = i.days.find(date); it != i.days.end())
                files.push_back({date, it->second.fileSize, it->second.fileModified, it->second.indexRevision});
        }
    })) return false;
    // Negative source claims are part of a 16-day frame too. A shared archive
    // refresh can discover a previously unrecorded day before our next pass.
    for (size_t n = 0; n < expected.days.size(); ++n) {
        if (expected.days[n]) continue;
        const auto date = start + int64_t(n) * DayMs;
        if (sources_.sourceState(symbolOf(id), date, date + DayMs) != FlowStore::SourceState::Absent)
            return false;
    }
    for (const auto& f : files) {
        try {
            const auto path = checked(filePath(id, f.day));
            if (fs::file_size(path) == f.size && fs::last_write_time(path) == f.modified) continue;
        } catch (const std::exception&) {}
        try { if (const auto fresh = load(id, f.day, f.revision)) publish(id, *fresh, f.revision); }
        catch (const std::exception&) {}
        wake(); return false;
    }
    return ifCurrent(id, level, start, expected, [] {});
}
bool FlowChunkStore::ifCurrent(const std::string& id, int64_t level, int64_t start,
                                const Stamp& expected, const std::function<void()>& action) const {
    std::lock_guard lock(mutex_);
    const auto current = stampLocked(id, level, start);
    if (!current || *current != expected) return false;
    action(); return true;
}
std::pair<uint64_t, int64_t> FlowChunkStore::fingerprint(const std::string& id) const {
    const auto floor = id.starts_with("coinbase:") ? sources_.liveRetentionFloor(id.substr(9)).value_or(0) : 0;
    std::lock_guard lock(mutex_);
    const auto it = instruments_.find(id);
    return {it == instruments_.end() ? 0 : it->second.serial, floor};
}
std::optional<FlowChunkStore::Snapshot> FlowChunkStore::snapshot(const std::string& id) {
    const auto before = fingerprint(id);
    auto scanned = scanLevels(id);
    if (!scanned || fingerprint(id) != before) return {};
    Snapshot out; out.serial = before.first; out.liveFromMs = before.second;
    out.epoch = scanned->first; out.levels = std::move(scanned->second);
    return out;
}
void FlowChunkStore::start() {
    if (thread_.joinable()) return;
    // Startup initialization runs on the worker: before it completes APIs return
    // unavailable, so expensive archive discovery never blocks the model thread.
    stopSource_ = std::stop_source{};
    thread_ = std::thread([this, stop = stopSource_.get_token()] { loop(stop); });
}
void FlowChunkStore::stop() { stopSource_.request_stop(); wake(); if (thread_.joinable()) thread_.join(); }
void FlowChunkStore::wake() { { std::lock_guard lock(wakeMutex_); woken_ = true; } wakeCv_.notify_all(); }
void FlowChunkStore::loop(std::stop_token stop) {
    sentinel::logging::setCurrentThreadName("flow-chunks");
    if (options_.lowerPriority && !setChunkWorkerPriority()) sLog_Warning("Flow chunk utility priority unchanged");
    while (!stop.stop_requested()) {
        try { runPass(stop); } catch (const std::exception &e) { sLog_Error("Flow chunk worker error=" << e.what()); }
        std::unique_lock lock(wakeMutex_);
        wakeCv_.wait_for(lock, options_.rescan, [&] { return woken_ || stop.stop_requested(); });
        woken_ = false;
    }
}
} // namespace recording
