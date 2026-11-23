// Canonical scans use a fresh feed per hour and update M before delivering the
// seal record's trades. Published bytes are immutable within an epoch. Fence
// first, persist the epoch, then rebuild and unfence. No lock spans replay or
// disk I/O; history workers recheck the epoch/fence after their read completes.
#include "FlowChunkStore.hpp"
#include "RollupStore.hpp"
#include "PersistenceIo.hpp"
#include "SentinelLogging.hpp"
#include "flow/FlowBarBuilder.hpp"
#include "flow/FlowCompose.hpp"
#include "roller/JournalFeed.hpp"
#include "heatmap/ChunkCodec.hpp"
#include <QFile>
#include <QDateTime>
#include <QTimeZone>
#include <algorithm>
#include <bit>

namespace recording {
namespace {
namespace fs = std::filesystem;
namespace flow = sentinel::flow;
namespace chunks = flow::chunks;
namespace roller = sentinel::roller;
constexpr int64_t Minute = 60'000, Hour = 60 * Minute, DayMs = 24 * Hour;
struct Cancelled {};
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
    const roller::JournalReader &catalog, std::stop_token stop) {
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
            cursor = roller::JournalPos{symbol, file.run, probe.result().recordOrdinal, probe.result().recordIndex};
            break;
        }
    }
    return cursor;
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
    auto protectedRoots = o.protectedRoots;
    protectedRoots.push_back(sources.root());
    const auto configured = options_.root.empty() ? fs::weakly_canonical(o.hmc2Root).string() + "-flc" : options_.root.string();
    options_.root = flowRootFor(configured, o.hmc2Root, o.journalRoot, o.rollupRoot, protectedRoots);
    if (options_.root.empty() || options_.rescan.count() <= 0 || options_.rules.sealWindowMs > Hour)
        throw std::invalid_argument("flow chunk store needs disjoint roots and W <= one hour");
    for (const auto &symbol : o.symbols) {
        auto &i = instruments_["coinbase:" + symbol];
        if (!options_.metrics) continue;
        auto &m = *options_.metrics;
        const sentinel::metrics::Labels labels{{"instrument", "coinbase:" + symbol}};
        i.late = &m.counter("sentinel_flow_late_trades_total", "Trades excluded by canonical seal scans, including rebuilds.", labels);
        i.regressions = &m.counter("sentinel_flow_receive_regressions_total", "Receive regressions in canonical hour scans, including rebuilds.", labels);
        i.failures = &m.counter("sentinel_flow_chunk_build_failures_total", "Failed chunk day builds, retried next pass.", labels);
        i.hours = &m.counter("sentinel_flow_chunk_hours_sealed_total", "Hour sections built, including rebuilds.", labels);
        i.pending = &m.gauge("sentinel_flow_chunk_pending_days", "Days waiting for build or epoch validation.", labels);
    }
}
FlowChunkStore::~FlowChunkStore() { stop(); }
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
std::optional<chunks::DayFile> FlowChunkStore::load(const std::string &id, int64_t day) {
    try {
        auto result = chunks::parseDayFile(bytes(checked(filePath(id, day))));
        if (result.instrument != id || result.dayStartMs != day) throw std::runtime_error("chunk file identity");
        return result;
    } catch (const std::exception &e) {
        bool log = false;
        { std::lock_guard lock(mutex_); auto &d = instruments_.at(id).days[day]; log = d.hours != 0; d = {0, false, true}; }
        if (log) sLog_Warning("Flow chunk withdrawn instrument=" << id << " day=" << day << " error=" << e.what());
        if (log) wake();
        return {};
    }
}
void FlowChunkStore::initializeInstrument(const std::string &id, const FlowStore::Archive &archive) {
    { std::lock_guard lock(mutex_); if (instruments_.at(id).initialized) return; }
    auto days = archive.days;
    const auto directory = checked(filePath(id, 0)).parent_path();
    if (fs::is_directory(directory)) for (const auto &entry : fs::directory_iterator(directory)) {
        if (entry.path().extension() != ".flc") continue;
        const auto date = QDate::fromString(QString::fromStdString(entry.path().stem().string()), "yyyy-MM-dd");
        if (date.isValid()) days.push_back(date.startOfDay(QTimeZone::UTC).toMSecsSinceEpoch());
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
      for (auto day : days) i.days.try_emplace(day); }
    for (auto day : days) if (auto file = load(id, day)) publish(id, *file);
}
void FlowChunkStore::initialize(std::stop_token stop) {
    std::lock_guard pass(passMutex_);
    for (const auto &symbol : sources_.options().symbols) {
        check(stop); initializeInstrument("coinbase:" + symbol, sources_.archive(symbol, stop));
    }
}
void FlowChunkStore::publish(const std::string &id, const chunks::DayFile &file) {
    std::lock_guard lock(mutex_);
    auto &i = instruments_.at(id);
    auto &d = i.days[file.dayStartMs]; d = {};
    if (!chunks::servable(i.state, file)) return;
    for (int h = 0; h < 24; ++h) if (file.sections[h].sealed) d.hours |= 1u << h;
    d.coarse = file.sections[24].sealed && file.sections[25].sealed;
}
void FlowChunkStore::transition(const std::string &id, int64_t day) {
    chunks::EpochState next;
    { std::lock_guard lock(mutex_); auto &i = instruments_.at(id);
      // Startup rule changes and crash recovery already fenced this day and
      // advanced the counter. Its new seal point may change the prefix too;
      // finish that transition instead of starting another one per rebuilt day.
      if (std::binary_search(i.state.pending.begin(), i.state.pending.end(), day)) return;
      i.days[day] = {0, false, true};
      i.state.pending.push_back(day);
      std::sort(i.state.pending.begin(), i.state.pending.end());
      next = i.state; }
    if (next.counter == UINT64_MAX) throw std::overflow_error("flow epoch exhausted");
    ++next.counter;
    write(filePath(id, day).parent_path() / "epoch", chunks::encodeEpochFile(next));
    { std::lock_guard lock(mutex_); instruments_.at(id).state = next; }
    sLog_Data("Flow epoch changed instrument=" << id << " day=" << day << " counter=" << next.counter);
}
FlowChunkStore::HourResult FlowChunkStore::buildHour(const std::string &symbol, int64_t hour,
    const FlowStoreGridValues &grid, const roller::JournalReader &catalog, std::stop_token stop, bool evidence) {
    HourResult out;
    const auto warmup = hour - Minute;
    const auto cursor = scanCursor(symbol, warmup, catalog, stop);
    if (!cursor) return out;
    roller::JournalReader reader(catalog, cursor);
    roller::JournalFeed feed(symbol);
    const auto &s = grid.scales;
    flow::FlowBarBuilder builder(Minute, s.nativeTick, s.priceScale, s.baseScale, s.quoteScale);
    std::optional<roller::JournalPos> first, last;
    int64_t watermark = INT64_MIN, firstReceive = INT64_MAX;
    bool partial = false;
    feed.onInvalid = [&](int64_t ms, const std::string &) { if (ms >= hour && ms < hour + Hour) partial = true; };
    feed.onTrade = [&](const ::Trade &value) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(value.timestamp.time_since_epoch()).count();
        if (ms < hour || ms >= hour + Hour) return;
        flow::Trade trade{value.trade_id, ms, value.price, value.size,
            value.side == AggressorSide::Buy ? flow::Side::Buy : value.side == AggressorSide::Sell ? flow::Side::Sell : flow::Side::Unknown};
        if (watermark >= ms / Minute * Minute + Minute + options_.rules.sealWindowMs) {
            ++out.late; if (evidence) out.excluded.push_back(trade); return;
        }
        if (builder.add(trade) == flow::FlowBarBuilder::AddResult::Accepted && evidence) out.accepted.push_back(std::move(trade));
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
        const auto source = sources_.sourceState(symbol, hour + m * Minute, hour + (m + 1) * Minute, true);
        appendRun(coverage, uint16_t(m), source == FlowStore::SourceState::Absent ? chunks::Status::Unrecorded :
            partial || source == FlowStore::SourceState::ReceiveTailOnly ? chunks::Status::Partial : chunks::Status::Complete);
    }
    auto bars = builder.bars();
    out.section = section(bars, Minute, hour, 60, gridOf(grid), coverage);
    out.section.sourcePrefix = FlowStore::prefixIdentity(catalog, *first, *last, stop);
    out.source = SourceCheck{*cursor, *first, *last, sources_.archiveStamp(symbol, catalog, *cursor, *last, stop),
                              out.section.sourcePrefix,
                              sources_.sourceState(symbol, hour, hour + Hour, true)};
    return out;
}
void FlowChunkStore::finishPending(const std::string &id, int64_t day) {
    chunks::EpochState next;
    { std::lock_guard lock(mutex_); next = instruments_.at(id).state; }
    if (!std::binary_search(next.pending.begin(), next.pending.end(), day)) return;
    chunks::finishDay(next, day);
    write(filePath(id, day).parent_path() / "epoch", chunks::encodeEpochFile(next));
    { std::lock_guard lock(mutex_); instruments_.at(id).state = next; }
}
void FlowChunkStore::buildDay(const std::string &id, int64_t day, const FlowStore::Archive &archive,
                               std::stop_token stop, FlowStore::PassResult &result) {
    const auto symbol = symbolOf(id);
    auto old = load(id, day);
    if (!archive.catalog) return;
    const auto cursor = scanCursor(symbol, day - Minute, *archive.catalog, stop);
    if (!cursor) { if (!old) finishPending(id, day); return; }
    roller::JournalReader probe(*archive.catalog, cursor);
    roller::JournalRecord input;
    bool sealable = false;
    while (probe.next(input)) {
        check(stop);
        if (input.record.time.systemNs / 1'000'000 >= day + Hour + options_.rules.sealWindowMs) { sealable = true; break; }
    }
    // A source-only forming day has no sealed bytes to revalidate. It remains
    // unavailable, but does not keep an archive recovery epoch pending forever.
    if (!sealable) { if (!old) finishPending(id, day); return; }
    const auto grid = sources_.gridFor(symbol, day, *archive.catalog, stop);
    chunks::DayFile file;
    file.instrument = id; file.dayStartMs = day; file.grid = gridOf(grid);
    file.sealWindowMs = options_.rules.sealWindowMs; file.ruleVersion = options_.rules.builderVersion;
    { std::lock_guard lock(mutex_); file.ruleIdentity = instruments_.at(id).state.ruleIdentity; }
    // Recompute header counters along with every canonical hour; there are no
    // per-hour counters on disk to recover incrementally after a restart.
    std::array<std::optional<SourceCheck>, 24> dependencies;
    uint64_t built = 0;
    bool changed = old && old->grid != file.grid;
    for (int h = 0; h < 24; ++h) {
        check(stop);
        const auto hour = day + h * Hour;
        auto scan = buildHour(symbol, hour, grid, *archive.catalog, stop);
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
    }
    check(stop);
    if (changed) transition(id, day);
    if (!built) return;
    compose(file);
    // Check that the source catalog/inventory stayed fixed during replay.
    // A concurrent rewrite/topology change is retried. Growth of the current
    // durable tail preserves its already validated blocks and is allowed.
    const auto after = sources_.archive(symbol, stop);
    if (after.integrity != archive.integrity) throw std::runtime_error("RAWL2 changed during chunk build");
    if (!old || *old != file) write(filePath(id, day), chunks::encodeDayFile(file));
    finishPending(id, day);
    publish(id, file);
    { std::lock_guard lock(mutex_); instruments_.at(id).days[day].sources = std::move(dependencies); }
    auto &i = instruments_.at(id); // registry references are immutable after construction
    if (i.late) i.late->inc(file.lateTrades);
    if (i.regressions) i.regressions->inc(file.regressions);
    if (i.hours) i.hours->inc(built);
    result.built += built;
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
    if (!saved.coarse || saved.invalid) return false;
    const auto symbol = symbolOf(id);
    for (int h = 0; h < 24; ++h) {
        check(stop);
        if (!saved.sources[h]) return false;
        auto &s = *saved.sources[h];
        const auto hour = day + h * Hour;
        // A newly inserted earlier run can change the canonical start even if
        // every previously used file is unchanged. Recheck it in reader order.
        if (scanCursor(symbol, hour - Minute, *archive.catalog, stop) != s.cursor ||
            sources_.sourceState(symbol, hour, hour + Hour, true) != s.source) return false;
        const auto stamp = sources_.archiveStamp(symbol, *archive.catalog, s.cursor, s.last, stop);
        if (stamp != s.stamp) {
            // Appending after a seal block must not replay an entire closed
            // day. Verify its canonical start and the existing prefix instead.
            roller::JournalReader reader(*archive.catalog, s.cursor);
            roller::JournalRecord record;
            std::optional<roller::JournalPos> first;
            while (reader.next(record)) {
                check(stop);
                if (record.record.time.systemNs / 1'000'000 >= hour - Minute) { first = record.pos; break; }
            }
            if (first != s.first || FlowStore::prefixIdentity(*archive.catalog, s.first, s.last, stop) != s.prefix) return false;
            s.stamp = stamp;
        }
    }
    { std::lock_guard lock(mutex_); instruments_.at(id).days.at(day).sources = saved.sources; }
    auto file = load(id, day);
    return file && file->grid == gridOf(sources_.gridFor(symbol, day, *archive.catalog, stop));
    } catch (const std::exception &) { check(stop); return false; }
}
FlowStore::PassResult FlowChunkStore::runPass(std::stop_token stop) {
    std::lock_guard pass(passMutex_);
    FlowStore::PassResult result;
    std::map<std::string, FlowStore::Archive> archives;
    // Initialize every instrument before replaying any of them. A rule change
    // on BTC must not hold unchanged ETH unavailable throughout BTC's backfill.
    for (const auto &symbol : sources_.options().symbols) {
        const auto id = "coinbase:" + symbol;
        try {
            check(stop);
            auto archive = sources_.archive(symbol, stop);
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
    for (const auto &[symbol, archive] : archives) {
        const auto id = "coinbase:" + symbol;
        try {
            check(stop);
            std::vector<int64_t> days;
            bool sourceChanged;
            { std::lock_guard lock(mutex_); auto &i = instruments_.at(id);
              for (auto day : archive.days) i.days.try_emplace(day);
              sourceChanged = i.generation != archive.generation;
              for (const auto &[day, state] : i.days) days.push_back(day); }
            // Every instrument gets newest-first recovery, including pending
            // epochs. Failed days stay in the inventory and are retried.
            uint64_t pendingCount = 0;
            for (auto day : days) {
                std::lock_guard lock(mutex_);
                const auto &i = instruments_.at(id);
                if (sourceChanged || !i.days.at(day).coarse) ++pendingCount;
            }
            auto &i = instruments_.at(id);
            if (i.pending) i.pending->set(double(pendingCount));
            bool failed = false;
            for (auto it = days.rbegin(); it != days.rend(); ++it) {
                check(stop);
                bool needsBuild;
                { std::lock_guard lock(mutex_); needsBuild = sourceChanged || !i.days.at(*it).coarse; }
                // Validate even otherwise idle files: corruption/loss removes
                // availability immediately and queues a deterministic rebuild.
                if (!needsBuild && !load(id, *it)) needsBuild = true;
                if (!needsBuild) continue;
                try {
                    if (sourceChanged && unchanged(id, *it, archive, stop)) {
                        if (pendingCount) --pendingCount;
                        if (i.pending) i.pending->set(double(pendingCount));
                        continue;
                    }
                    buildDay(id, *it, archive, stop, result);
                    if (pendingCount) --pendingCount;
                    if (i.pending) i.pending->set(double(pendingCount));
                } catch (const Cancelled &) { throw; }
                  catch (const std::exception &e) {
                    if (stop.stop_requested()) throw Cancelled{};
                    ++result.failed; failed = true;
                    if (i.failures) i.failures->inc();
                    sLog_Warning("Flow chunk build failed instrument=" << id << " day=" << *it << " error=" << e.what());
                }
            }
            { std::lock_guard lock(mutex_);
              if (!failed) i.generation = archive.generation;
              uint64_t remaining = 0;
              for (const auto &[day, d] : i.days)
                  if (!d.coarse || std::binary_search(i.state.pending.begin(), i.state.pending.end(), day)) ++remaining;
              if (i.pending) i.pending->set(double(remaining)); }

        } catch (const Cancelled &) { result.stopped = true; break; }
          catch (const std::exception &e) {
            if (stop.stop_requested()) { result.stopped = true; break; }
            ++result.failed;
            sLog_Warning("Flow chunk pass failed instrument=" << id << " error=" << e.what());
        }
    }
    result.stopped |= stop.stop_requested();
    return result;
}
FlowStore::PassResult FlowChunkStore::rebuildDay(const std::string &id, int64_t day, std::stop_token stop) {
    std::lock_guard pass(passMutex_);
    const auto archive = sources_.archive(symbolOf(id), stop);
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
    const auto before = epoch(id);
    if (!before) return {};
    const auto symbol = symbolOf(id);
    const auto day = start / DayMs * DayMs;
    const auto count = level == DayMs ? 16 : 1;
    std::vector<std::vector<uint8_t>> pieces(16);
    std::vector<uint64_t> steps(16, 1);
    Payload out; out.epoch = before;
    for (int n = 0; n < count; ++n) {
        const auto date = day + n * DayMs;
        bool known = false, ready = false;
        { std::lock_guard lock(mutex_); const auto &i = instruments_.at(id);
          if (std::binary_search(i.state.pending.begin(), i.state.pending.end(), date)) return {};
          const auto it = i.days.find(date);
          known = it != i.days.end();
          if (known) ready = level == Minute ? (it->second.hours & (1u << ((start - day) / Hour))) != 0 : it->second.coarse; }
        if (!known && level == DayMs && sources_.sourceState(symbol, date, date + DayMs) == FlowStore::SourceState::Absent) continue;
        if (!ready) return {};
        auto file = load(id, date);
        if (!file) return {};
        { std::lock_guard lock(mutex_); if (!chunks::servable(instruments_.at(id).state, *file)) return {}; }
        const size_t entry = level == Minute ? size_t((start - day) / Hour) : level == Hour ? 24 : 25;
        const auto &s = file->sections[entry];
        if (!s.sealed) return {};
        if (level == DayMs) { pieces[n] = s.payload; steps[n] = uint64_t(1) << s.stepExponent; }
        else { out.bytes = s.payload; out.summaryLength = s.summaryLength; out.stepExponent = s.stepExponent; }
    }
    if (level == DayMs) {
        const auto data = chunks::assembleDayChunk(start, pieces, chunks::Status::Unrecorded, steps);
        out.bytes = chunks::encode(data, false, &out.summaryLength);
        out.stepExponent = uint8_t(std::countr_zero(data.priceStep));
    }
    // Fence or epoch changes invalidate reads already in a history worker.
    { std::lock_guard lock(mutex_); const auto &i = instruments_.at(id);
      if (chunks::epochOf(chunks::flowDomain(id), i.state) != before) return {};
      for (int n = 0; n < count; ++n) {
          const auto date = day + n * DayMs;
          if (std::binary_search(i.state.pending.begin(), i.state.pending.end(), date)) return {};
          if (const auto it = i.days.find(date); it != i.days.end() && it->second.invalid) return {};
      } }
    return out;
}
std::vector<FlowChunkStore::Range> FlowChunkStore::sealed(const std::string &id, int64_t level) {
    if (level != Minute && level != Hour && level != DayMs) return {};
    const auto before = epoch(id);
    if (!before) return {};
    std::set<int64_t> candidates;
    { std::lock_guard lock(mutex_); const auto it = instruments_.find(id); if (it == instruments_.end()) return {};
      for (const auto &[day, d] : it->second.days) {
          if (level == Minute) { for (int h = 0; h < 24; ++h) if (d.hours & (1u << h)) candidates.insert(day + h * Hour); }
          else if (d.coarse) candidates.insert(heatmap::chunkStartOf(level, day));
      } }
    const auto span = level == Minute ? Hour : level == Hour ? DayMs : 16 * DayMs;
    std::vector<Range> out;
    for (auto start : candidates) if (read(id, level, start)) {
        if (!out.empty() && out.back().toMs == start) out.back().toMs += span;
        else out.push_back({start, start + span});
    }
    if (options_.afterAvailabilityReadForTest) options_.afterAvailabilityReadForTest();
    // A range can span many disk reads. Apply the same final fence as read(),
    // so a transition midway through enumeration cannot advertise stale keys.
    { std::lock_guard lock(mutex_); const auto &i = instruments_.at(id);
      if (chunks::epochOf(chunks::flowDomain(id), i.state) != before) return {};
      for (const auto &[day, d] : i.days) {
          if (!d.invalid && !std::binary_search(i.state.pending.begin(), i.state.pending.end(), day)) continue;
          if (std::any_of(out.begin(), out.end(), [&](const auto &r) { return day < r.toMs && day + DayMs > r.fromMs; })) return {};
      } }
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
    if (options_.lowerPriority && !lowerThreadPriority()) sLog_Warning("Flow chunk priority unchanged");
    while (!stop.stop_requested()) {
        try { runPass(stop); } catch (const std::exception &e) { sLog_Error("Flow chunk worker error=" << e.what()); }
        std::unique_lock lock(wakeMutex_);
        wakeCv_.wait_for(lock, options_.rescan, [&] { return woken_ || stop.stop_requested(); });
        woken_ = false;
    }
}
} // namespace recording
