#include "CaptureVerifier.hpp"
#include "CaptureRouting.hpp"
#include <QDirIterator>
#include <QDateTime>
#include <QTimeZone>
#include <QFileInfo>
#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>
#include <tuple>

namespace sentinel::capture {
DecimalGrid::Decimal DecimalGrid::parse(const std::string& value) {
    if (value.empty() || value.size() > 80) throw std::runtime_error("invalid decimal length");
    const auto dot = value.find('.');
    if (dot == 0 || (dot != std::string::npos && dot + 1 == value.size()))
        throw std::runtime_error("invalid decimal: " + value);
    auto end = value.size();
    if (dot != std::string::npos) while (end > dot + 1 && value[end - 1] == '0') --end;
    const auto places = dot == std::string::npos ? 0 : end - dot - 1;
    if (places > 18) throw std::runtime_error("decimal exceeds 18 places: " + value);
    uint64_t mantissa = 0;
    for (size_t i = 0; i < end; ++i) {
        if (i == dot) continue;
        const auto c = value[i];
        if (c < '0' || c > '9') throw std::runtime_error("invalid decimal: " + value);
        if (mantissa > (UINT64_MAX - (c - '0')) / 10) throw std::runtime_error("decimal overflow: " + value);
        mantissa = mantissa * 10 + (c - '0');
    }
    return {mantissa, static_cast<uint32_t>(places)};
}
DecimalGrid::DecimalGrid(const std::string& increment) : m_increment(parse(increment)) {
    if (!m_increment.mantissa) throw std::runtime_error("increment must be positive");
}
uint64_t DecimalGrid::atoms(const std::string& value) const {
    auto number = parse(value);
    if (number.places > m_increment.places) throw std::runtime_error("off-grid decimal: " + value);
    while (number.places++ < m_increment.places) {
        if (number.mantissa > UINT64_MAX / 10) throw std::runtime_error("decimal atom overflow");
        number.mantissa *= 10;
    }
    if (number.mantissa % m_increment.mantissa) throw std::runtime_error("off-grid decimal: " + value);
    return number.mantissa / m_increment.mantissa;
}

namespace {
struct File {
    QString path;
    int64_t started = 0;
    std::string run;
    uint64_t segment = 0;
};
struct Channel { uint64_t frames = 0, bytes = 0; };
std::string utcDay(int64_t ns) {
    return QDateTime::fromMSecsSinceEpoch(ns / 1000000, QTimeZone::UTC).toString("yyyy-MM-dd").toStdString();
}
void addDaily(nlohmann::json& days, const std::string& day, const char* key, uint64_t amount) {
    auto& value = days[day];
    if (value.is_null()) value = {{"frames", 0}, {"received_bytes", 0}, {"file_bytes", 0}, {"l2_events", 0}};
    value[key] = value.value(key, uint64_t(0)) + amount;
}
struct Replay {
    std::optional<DecimalGrid> prices, quantities;
    std::string symbol, run;
    std::vector<std::string> products;
    bool multi = false, snapshotPending = false, newest = false;
    uint64_t interruptedRuns = 0, rangeReceipts = 0, batchFrames = 0, batchBytes = 0;
    std::optional<uint64_t> batchFirst, batchLast;
    uint64_t connectionFrames = 0, connectionBytes = 0, references = 0, missingSnapshots = 0, l2Events = 0;
    uint64_t openRuns = 0, badTails = 0;
    bool lastIndexed = false;
    bool closedOk = true;
    nlohmann::json days = nlohmann::json::object(), connectionDays = nlohmann::json::object();
    nlohmann::json runReports = nlohmann::json::array(), baseline = nlohmann::json::object();
    std::map<uint64_t, uint64_t> bids, asks;
    uint64_t connection = 0, expectedSequence = 0;
    bool haveConnection = false, active = false, haveSequence = false, bookValid = false;
    uint64_t frames = 0, bytes = 0, connections = 0, reconnects = 0, downs = 0, gaps = 0;
    uint64_t invalidations = 0, resyncs = 0, acks = 0, errors = 0, unsequenced = 0;
    uint64_t unanchored = 0, replayed = 0, snapshots = 0, snapshotFrames = 0, snapshotBytes = 0;
    uint64_t snapshotMinBytes = UINT64_MAX, snapshotMaxBytes = 0;
    uint64_t snapshotEntries = 0, snapshotMinEntries = UINT64_MAX, snapshotMaxEntries = 0, zeroSnapshotEntries = 0;
    uint64_t systemClockRegressions = 0, captureStops = 0, engineErrors = 0, incompleteRuns = 0;
    uint64_t explicitGaps = 0;
    nlohmann::json gapDetails = nlohmann::json::array();
    bool sawStart = false, sawStop = false, sawSnapshot = false;
    int64_t previousSystem = -1, firstSteady = -1, lastSteady = -1, second = -1;
    uint64_t secondFrames = 0, seconds = 0;
    double duration = 0;
    std::vector<uint64_t> histogram = std::vector<uint64_t>(1000001, 0);
    std::map<std::string, Channel> channels;
    std::vector<std::string> details;

    void error(const std::string& message) {
        ++errors;
        if (details.size() < 30) details.push_back(message);
    }
    void invalidate() { bids.clear(); asks.clear(); bookValid = false; }
    void finishSecond() {
        if (second < 0) return;
        if (secondFrames >= histogram.size()) error("more than 1,000,000 frames in one second");
        ++histogram[std::min<size_t>(secondFrames, histogram.size() - 1)];
        ++seconds;
    }
    nlohmann::json connectionMetrics() const {
        return {{"frames", connectionFrames}, {"received_bytes", connectionBytes},
            {"connections", connections}, {"reconnects", reconnects}, {"transport_down_events", downs},
            {"sequence_gaps", gaps}, {"unsequenced_frames", unsequenced}, {"explicit_capture_gaps", explicitGaps},
            {"invalidations", invalidations}, {"resync_requests", resyncs}, {"engine_errors", engineErrors},
            {"capture_stops", captureStops}, {"errors", errors}, {"unanchored_l2_events", unanchored},
            {"missing_snapshot_connections", missingSnapshots}, {"bad_tails", badTails}};
    }
    void endConnection() {
        if (snapshotPending) ++missingSnapshots;
        snapshotPending = false;
    }
    void finishRun() {
        if (run.empty()) return;
        finishSecond();
        const bool interrupted = sawStart && !sawStop && !newest;
        if (interrupted) { ++interruptedRuns; error("interrupted run superseded by a newer run=" + run); }
        const bool open = sawStart && !sawStop && newest;
        if (!sawStart || !sawStop || !sawSnapshot) ++incompleteRuns;
        if (open) ++openRuns;
        const auto span = firstSteady < 0 ? 0 : std::max(1e-9, double(lastSteady - firstSteady) / 1e9);
        duration += span;
        auto metrics = connectionMetrics();
        for (auto& [key, value] : metrics.items()) value = value.get<uint64_t>() - baseline.value(key, uint64_t(0));
        const bool valid = sawStart && (open || sawSnapshot) && metrics["errors"] == 0 &&
            metrics["sequence_gaps"] == 0 && metrics["unsequenced_frames"] == 0 && metrics["explicit_capture_gaps"] == 0 &&
            metrics["unanchored_l2_events"] == 0 && metrics["missing_snapshot_connections"] == 0 &&
            metrics["bad_tails"] == 0 && (!sawStop || lastIndexed);
        metrics["days"] = std::move(connectionDays); connectionDays = nlohmann::json::object();
        metrics["run_id"] = run;
        metrics["products"] = products;
        metrics["symbol"] = symbol;
        metrics["ok"] = valid;
        metrics["interrupted"] = interrupted;
        metrics["incomplete"] = !sawStart || !sawStop || !sawSnapshot;
        metrics["open"] = open;
        metrics["closed"] = sawStop;
        metrics["duration_seconds"] = span;
        metrics["pending_routing_frames"] = multi ? batchFrames : 0;
        runReports.push_back(std::move(metrics));
        if (!open && !valid) closedOk = false;
        baseline = connectionMetrics();
        sawStart = sawStop = sawSnapshot = snapshotPending = false;
        firstSteady = lastSteady = second = previousSystem = -1;
        secondFrames = 0;
        connection = expectedSequence = 0;
        haveConnection = active = haveSequence = false;
        batchFrames = batchBytes = 0; batchFirst.reset(); batchLast.reset();
        invalidate();
    }
    void record(const Record& record) {
        const bool reference = record.kind == Kind::FrameReference;
        if (reference) {
            try {
                const auto range = nlohmann::json::parse(record.payload);
                validateRange(range, products);
                const auto first = range.at("first_seq").get<uint64_t>(), last = range.at("last_seq").get<uint64_t>();
                const auto count = range.at("count").get<uint64_t>(), received = range.at("bytes").get<uint64_t>();
                if (record.connection != connection || !active) error("range outside its transport lifetime");
                if ((haveSequence && first != expectedSequence) || range.at("sequence_gaps") != 0 ||
                    last < first || last == UINT64_MAX || last - first + 1 != count) {
                    ++gaps; invalidate();
                }
                if (batchFrames + range.at("foreign_count").get<uint64_t>() != count ||
                    batchBytes + range.at("foreign_bytes").get<uint64_t>() != received ||
                    (batchFirst && (*batchFirst < first || *batchLast > last))) error("range/raw accounting mismatch");
                expectedSequence = last + 1; haveSequence = true;
                const auto foreign = range.at("foreign_count").get<uint64_t>();
                const auto foreignBytes = range.at("foreign_bytes").get<uint64_t>();
                references += foreign; ++rangeReceipts;
                connectionFrames += foreign; connectionBytes += foreignBytes;
                const auto day = utcDay(record.time.systemNs);
                addDaily(connectionDays, day, "frames", foreign);
                addDaily(connectionDays, day, "received_bytes", foreignBytes);
                batchFrames = batchBytes = 0; batchFirst.reset(); batchLast.reset();
            } catch (const std::exception& e) { error(e.what()); invalidate(); }
            // Range clocks name the last original frame, including foreign ones.
        } else if (multi && record.kind == Kind::Frame) {
            try {
                const auto identity = frameReceipt(record.payload, products);
                const auto targets = identity.at("products").get<std::vector<std::string>>();
                if (!std::binary_search(targets.begin(), targets.end(), symbol)) error("raw frame routed to wrong product");
                ++batchFrames; batchBytes += record.payload.size();
                if (batchFrames > MaxRoutingFrames) error("too many raw frames without a routing range");
            } catch (const std::exception& e) { error(e.what()); }
        } else if (multi && batchFrames) {
            error("lifecycle marker before pending routing range");
            batchFrames = batchBytes = 0; batchFirst.reset(); batchLast.reset();
        }
        if (previousSystem >= 0 && record.time.systemNs < previousSystem) ++systemClockRegressions;
        previousSystem = record.time.systemNs;
        if (firstSteady < 0) firstSteady = record.time.steadyNs;
        if (lastSteady > record.time.steadyNs) error("steady clock regressed within run");
        lastSteady = std::max(lastSteady, record.time.steadyNs);
        const auto bucket = (record.time.steadyNs - firstSteady) / 1000000000;
        if (bucket > second) {
            if (second >= 0) {
                finishSecond();
                histogram[0] += bucket - second - 1; seconds += bucket - second - 1;
            }
            second = bucket; secondFrames = 0;
        }
        if (reference) return;
        if (record.kind == Kind::TransportUp) {
            endConnection(); snapshotPending = true;
            if (haveConnection && record.connection != connection + 1) error("non-contiguous connection id");
            if (record.connection == 0) error("zero connection id on transport up");
            if (connections && haveConnection) ++reconnects;
            ++connections; connection = record.connection;
            haveConnection = active = haveSequence = true;
            expectedSequence = 0; invalidate(); return;
        }
        if (record.kind == Kind::CaptureStarted) { if (sawStart) error("duplicate capture start"); sawStart = true; return; }
        if (record.kind == Kind::CaptureStopped) {
            endConnection();
            ++captureStops; sawStop = true; active = false; invalidate();
            try {
                const auto stop = nlohmann::json::parse(record.payload);
                if (stop.value("gap", false)) {
                    ++explicitGaps;
                    if (gapDetails.size() < 30) gapDetails.push_back(stop);
                }
            } catch (const std::exception& e) { error(std::string("invalid stop marker: ") + e.what()); }
            return;
        }
        if (record.kind == Kind::EngineError) { ++engineErrors; return; }
        if (record.kind == Kind::TransportDown) { endConnection(); ++downs; active = false; invalidate(); return; }
        if (record.kind == Kind::BookInvalidated) { ++invalidations; invalidate(); return; }
        if (record.kind == Kind::ResyncRequested) { ++resyncs; invalidate(); return; }
        ++frames; ++secondFrames; bytes += record.payload.size();
        ++connectionFrames;
        const auto receivedBytes = record.payload.size();
        connectionBytes += receivedBytes;
        const auto day = utcDay(record.time.systemNs);
        addDaily(connectionDays, day, "frames", 1);
        addDaily(connectionDays, day, "received_bytes", receivedBytes);
        addDaily(days, day, "frames", 1);
        addDaily(days, day, "received_bytes", receivedBytes);
        if (record.connection == 0) error("frame without connection id");
        if (!haveConnection) {
            // A single hourly file may start halfway through an existing connection.
            connection = record.connection; haveConnection = active = true;
        } else if (record.connection != connection || !active) {
            error("frame outside its transport lifetime"); invalidate();
        }
        bool channelCounted = false;
        try {
            const auto json = nlohmann::json::parse(record.payload);
            const auto channel = json.value("channel", std::string("<unclassified>"));
            if (channel.size() > 128) throw std::runtime_error("channel name too long");
            if (channels.size() >= 1024 && !channels.contains(channel)) throw std::runtime_error("too many channels");
            ++channels[channel].frames; channels[channel].bytes += record.payload.size();
            channelCounted = true;
            bool gap = false;
            if (json.contains("sequence_num") && json["sequence_num"].is_number_integer() &&
                (!json["sequence_num"].is_number_unsigned() ? json["sequence_num"].get<int64_t>() >= 0 : true)) {
                const auto sequence = json["sequence_num"].get<uint64_t>();
                if ((!multi && haveSequence && sequence != expectedSequence) ||
                    (multi && batchLast && sequence <= *batchLast)) {
                    ++gaps; invalidate(); gap = true;
                    if (details.size() < 30) details.push_back("sequence gap connection=" + std::to_string(connection) +
                        " expected=" + std::to_string(expectedSequence) + " got=" + std::to_string(sequence));
                }
                if (sequence == UINT64_MAX) throw std::runtime_error("sequence overflow");
                if (multi) { if (!batchFirst) batchFirst = sequence; batchLast = sequence; }
                else { expectedSequence = sequence + 1; haveSequence = true; }
            } else {
                ++unsequenced; invalidate();
            }
            if (channel == "subscriptions") ++acks; // exact ack remains a frame, not reconstructed JSON
            if (channel != "l2_data") return;
            bool snapshotFrame = false;
            if (!json.at("events").is_array()) throw std::runtime_error("L2 events must be an array");
            for (const auto& event : json.at("events")) {
                const auto eventProduct = event.at("product_id").get<std::string>();
                if (eventProduct != symbol) {
                    if (!std::binary_search(products.begin(), products.end(), eventProduct)) throw std::runtime_error("unexpected L2 product");
                    continue;
                }
                ++l2Events; addDaily(days, day, "l2_events", 1);
                const auto type = event.at("type").get<std::string>();
                const bool snapshot = type == "snapshot";
                if (!snapshot && type != "update") throw std::runtime_error("unknown L2 event type");
                const auto& updates = event.at("updates");
                if (!updates.is_array()) throw std::runtime_error("updates must be an array");
                if (snapshot) {
                    snapshotPending = false;
                    ++snapshots; snapshotFrame = sawSnapshot = true;
                    snapshotEntries += updates.size();
                    snapshotMinEntries = std::min<uint64_t>(snapshotMinEntries, updates.size());
                    snapshotMaxEntries = std::max<uint64_t>(snapshotMaxEntries, updates.size());
                    invalidate(); bookValid = !gap;
                }
                if (!bookValid) ++unanchored;
                for (const auto& update : updates) {
                    const auto price = prices->atoms(update.at("price_level").get<std::string>());
                    const auto quantity = quantities->atoms(update.at("new_quantity").get<std::string>());
                    const auto side = update.at("side").get<std::string>();
                    if (price == 0 || (side != "bid" && side != "offer")) throw std::runtime_error("invalid L2 price/side");
                    if (snapshot && quantity == 0) ++zeroSnapshotEntries;
                    if (!bookValid) continue;
                    auto& book = side == "bid" ? bids : asks;
                    if (quantity) book[price] = quantity;
                    else book.erase(price);
                    if (bids.size() + asks.size() > 2000000) throw std::runtime_error("book exceeds 2,000,000 level limit");
                }
                if (bookValid) ++replayed;
            }
            if (snapshotFrame) {
                ++snapshotFrames; snapshotBytes += record.payload.size();
                snapshotMinBytes = std::min<uint64_t>(snapshotMinBytes, record.payload.size());
                snapshotMaxBytes = std::max<uint64_t>(snapshotMaxBytes, record.payload.size());
            }
            // Absolute updates within one envelope are atomic for this invariant.
            if (bookValid && !bids.empty() && !asks.empty() && bids.rbegin()->first >= asks.begin()->first)
                throw std::runtime_error("crossed/locked book after L2 envelope");
        } catch (const std::exception& e) {
            if (!channelCounted) {
                ++channels["<invalid-envelope>"].frames;
                channels["<invalid-envelope>"].bytes += record.payload.size();
            }
            error("frame " + std::to_string(frames) + ": " + e.what()); invalidate();
        }
    }
};

// Read one bounded proof group at a time. Payload bytes are hashed and released;
// memory does not grow with the run or the size of its snapshots.
struct RunCursor {
    struct Item { Record stamp; nlohmann::json identity; };
    struct Group { std::map<uint64_t, Item> frames; std::optional<Record> boundary; };
    std::vector<File> files;
    size_t file = 0;
    std::unique_ptr<RecordReader> reader;
    explicit RunCursor(std::vector<File> input) : files(std::move(input)) {
        std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.segment < b.segment; });
    }
    bool next(Record& r) {
        while (file < files.size()) {
            if (!reader) reader = std::make_unique<RecordReader>(files[file].path);
            if (reader->next(r)) return true;
            reader.reset(); ++file;
        }
        return false;
    }
    Group group(const std::vector<std::string>& products) {
        Group result;
        Record r;
        while (next(r)) {
            if (r.kind != Kind::Frame) { result.boundary = std::move(r); break; }
            auto identity = frameReceipt(r.payload, products);
            const auto& seq = identity.at("sequence_num");
            if (!seq.is_number_unsigned()) throw std::runtime_error("unsequenced raw frame in routing proof");
            const auto n = seq.get<uint64_t>();
            std::string{}.swap(r.payload); // release payload capacity, retain only identity
            if (!result.frames.emplace(n, Item{std::move(r), std::move(identity)}).second)
                throw std::runtime_error("duplicate raw sequence in routing group");
            if (result.frames.size() > MaxRoutingFrames) throw std::runtime_error("routing group exceeds limit");
        }
        return result;
    }
};
void compareStreams(const std::map<std::string, std::vector<File>>& streams,
                    const std::vector<std::string>& products, bool interrupted) {
    std::vector<RunCursor> cursors;
    for (const auto& symbol : products) cursors.emplace_back(streams.at(symbol));
    for (;;) {
        std::vector<RunCursor::Group> groups;
        bool end = false, proof = false;
        for (auto& cursor : cursors) {
            groups.push_back(cursor.group(products));
            end = end || !groups.back().boundary;
            proof = proof || (groups.back().boundary && groups.back().boundary->kind == Kind::FrameReference);
        }
        std::map<uint64_t, RunCursor::Item> merged;
        uint64_t commonLast = UINT64_MAX;
        bool commonFrames = true;
        for (const auto& group : groups) {
            if (group.frames.empty()) commonFrames = false;
            else commonLast = std::min(commonLast, group.frames.rbegin()->first);
            for (const auto& [seq, item] : group.frames) {
                auto [where, inserted] = merged.emplace(seq, item);
                if (!inserted && (where->second.stamp != item.stamp || where->second.identity != item.identity))
                    throw std::runtime_error("routed raw copies differ in bytes/clocks");
            }
        }
        // Without a receipt the tail's foreign sequence coverage is unknown.
        // Still check all raw duplicates, and routing within the observed common
        // raw prefix. A durable receipt makes its entire group accountable.
        for (const auto& [seq, item] : merged) if (proof || (commonFrames && seq <= commonLast)) {
            const auto targets = item.identity.at("products").get<std::vector<std::string>>();
            for (size_t i = 0; i < products.size(); ++i)
                if (std::binary_search(targets.begin(), targets.end(), products[i]) != groups[i].frames.contains(seq))
                    throw std::runtime_error("missing or wrongly routed raw copy");
        }
        if (proof) {
            RoutingBatch batch;
            for (const auto& [seq, item] : merged) {
                batch.add(item.stamp, item.identity);
            }
            for (size_t i = 0; i < products.size(); ++i) {
                const auto& boundary = groups[i].boundary;
                if (!boundary && interrupted) continue;
                if (!boundary || boundary->kind != Kind::FrameReference ||
                    boundary->time != batch.last || boundary->connection != batch.connection ||
                    nlohmann::json::parse(boundary->payload) != batch.receipt(products[i]))
                    throw std::runtime_error("range receipt differs from recovered raw frames (missing copy or identity mismatch)");
            }
        } else {
            // Lifecycle markers must agree throughout the common prefix. A
            // stopped writer is never an excuse to ignore another's proof.
            const auto& first = groups.front().boundary;
            for (const auto& group : groups) {
                if (!group.frames.empty() && group.boundary)
                    throw std::runtime_error("raw frames missing routing range before lifecycle marker");
                if ((!end || !interrupted) && group.boundary != first)
                    throw std::runtime_error("routed lifecycle markers differ");
            }
        }
        if (end) {
            if (!interrupted && std::any_of(groups.begin(), groups.end(), [](const auto& g) { return g.boundary.has_value(); }))
                throw std::runtime_error("routed streams have different lengths");
            return;
        }
    }
}

VerificationReport verifyFiles(std::vector<File> files, const std::map<std::string, std::string>& newestRuns = {}) {
    VerificationReport report;
    Replay replay;
    uint64_t fileBytes = 0, compressedBytes = 0, rawBlockBytes = 0, blocks = 0, tornTails = 0, unindexed = 0, scanned = 0;
    std::vector<File> readable;
    for (auto& file : files) {
        try {
            const auto header = readHeader(file.path);
            file.started = header.at("run_started_system_ns").get<int64_t>();
            file.run = header.at("run_id").get<std::string>();
            file.segment = header.at("segment").get<uint64_t>();
            readable.push_back(std::move(file));
        }
        catch (const std::exception& e) { replay.error(file.path.toStdString() + ": " + e.what()); }
    }
    std::sort(readable.begin(), readable.end(), [](const File& a, const File& b) {
        return std::tie(a.started, a.run, a.segment) < std::tie(b.started, b.run, b.segment);
    });
    std::string run;
    uint64_t nextSegment = 0, nextBlock = 0, runs = 0;
    nlohmann::json product;
    for (size_t i = 0; i < readable.size(); ++i) {
        const auto& file = readable[i];
        try {
            const auto header = readHeader(file.path);
            const auto& thisRun = file.run;
            const auto segment = file.segment;
            const auto firstBlock = header.at("first_block_ordinal").get<uint64_t>();
            if (thisRun != run) {
                const auto nextProduct = header.at("product_metadata");
                DecimalGrid nextPrices(nextProduct.at("quote_increment").get<std::string>());
                DecimalGrid nextQuantities(nextProduct.at("base_increment").get<std::string>());
                replay.finishRun(); run = thisRun; replay.run = run; ++runs;
                replay.multi = header.at("format_version") == 2;
                replay.products = replay.multi ? header.at("connection_products").get<std::vector<std::string>>() :
                                               header.at("products").get<std::vector<std::string>>();
                replay.newest = std::all_of(replay.products.begin(), replay.products.end(), [&](const auto& symbol) {
                    const auto found = newestRuns.find(symbol); return found != newestRuns.end() && found->second == run;
                });
                nextSegment = segment; nextBlock = firstBlock;
                product = nextProduct;
                replay.symbol = product.at("product_id").get<std::string>();
                replay.prices = nextPrices;
                replay.quantities = nextQuantities;
            }
            if (segment != nextSegment || firstBlock != nextBlock) {
                replay.error("missing/duplicate segment or block in run=" + run);
                replay.invalidate();
            }
            if (replay.multi != (header.at("format_version") == 2)) throw std::runtime_error("format changed within run");
            if (nlohmann::json(replay.products) != (replay.multi ? header.at("connection_products") : header.at("products")))
                throw std::runtime_error("connection products changed within run");
            if (product != header.at("product_metadata")) throw std::runtime_error("product metadata changed within run");
            const auto result = scan(file.path, [&](const Record& record) { replay.record(record); });
            ++scanned;
            replay.lastIndexed = result.indexed;
            if ((!result.indexed || result.tornTail) && i + 1 < readable.size() && readable[i + 1].run == run)
                ++replay.badTails;
            fileBytes += result.fileBytes;
            addDaily(replay.days, utcDay(header.at("opened_system_ns").get<int64_t>()), "file_bytes", result.fileBytes);
            tornTails += result.tornTail;
            unindexed += !result.indexed;
            blocks += result.index.size();
            for (const auto& entry : result.index) {
                compressedBytes += entry.compressedBytes; rawBlockBytes += entry.rawBytes;
            }
            nextBlock = result.index.empty() ? firstBlock : result.index.back().ordinal + 1;
            nextSegment = segment + 1;
            if (result.tornTail) replay.invalidate();
        } catch (const std::exception& e) {
            replay.error(file.path.toStdString() + ": " + e.what()); replay.invalidate();
        }
    }
    replay.finishRun();
    if (scanned == 0 || (replay.connectionFrames == 0 && replay.openRuns == 0)) replay.error("no captured frames verified");
    uint64_t p99 = 0, count = 0;
    const auto rank = static_cast<uint64_t>(std::ceil(replay.seconds * .99));
    for (; p99 < replay.histogram.size(); ++p99) {
        count += replay.histogram[p99]; if (count >= rank) break;
    }
    const auto perDay = [&](uint64_t bytes) { return replay.duration > 0 ? double(bytes) / replay.duration * 86400.0 : 0; };
    nlohmann::json channels = nlohmann::json::object();
    for (const auto& [name, value] : replay.channels)
        channels[name] = {{"frames", value.frames}, {"received_bytes", value.bytes}, {"received_bytes_per_day", perDay(value.bytes)}};
    report.ok = replay.errors == 0 && !replay.runReports.empty() &&
        std::all_of(replay.runReports.begin(), replay.runReports.end(), [](const auto& run) { return run.at("ok") == true; });
    report.json = {{"ok", report.ok}, {"ok_closed_runs", replay.closedOk && scanned != 0},
        {"complete", report.ok && replay.openRuns == 0}, {"open_runs", replay.openRuns},
        {"interrupted_runs", replay.interruptedRuns}, {"range_receipts", replay.rangeReceipts},
        {"days", replay.days}, {"frame_references", replay.references}, {"l2_events", replay.l2Events},
        {"missing_snapshot_connections", replay.missingSnapshots}, {"run_reports", replay.runReports}, {"files", scanned}, {"runs", runs}, {"blocks", blocks},
        {"frames", replay.frames}, {"duration_seconds", replay.duration}, {"fps_mean", replay.duration > 0 ? replay.frames / replay.duration : 0},
        {"fps_p99_one_second_buckets", p99}, {"fps_bucket_count_including_idle", replay.seconds},
        {"received_bytes", replay.bytes}, {"received_bytes_per_day", perDay(replay.bytes)},
        {"file_bytes", fileBytes}, {"file_bytes_per_day", perDay(fileBytes)}, {"zstd_bytes", compressedBytes},
        {"zstd_bytes_per_day", perDay(compressedBytes)}, {"uncompressed_record_bytes", rawBlockBytes},
        {"connections", replay.connections}, {"reconnects", replay.reconnects}, {"transport_down_events", replay.downs},
        {"explicit_capture_gaps", replay.explicitGaps}, {"capture_gap_details", replay.gapDetails},
        {"sequence_gaps", replay.gaps}, {"unsequenced_frames", replay.unsequenced}, {"subscription_acks", replay.acks},
        {"invalidations", replay.invalidations}, {"resync_requests", replay.resyncs}, {"engine_errors", replay.engineErrors},
        {"capture_stops", replay.captureStops}, {"incomplete_runs", replay.incompleteRuns}, {"snapshots", replay.snapshots}, {"snapshot_frames", replay.snapshotFrames},
        {"snapshot_received_bytes", replay.snapshotBytes}, {"snapshot_min_frame_bytes", replay.snapshotFrames ? replay.snapshotMinBytes : 0},
        {"snapshot_max_frame_bytes", replay.snapshotMaxBytes}, {"snapshot_entries", replay.snapshotEntries},
        {"snapshot_min_entries", replay.snapshots ? replay.snapshotMinEntries : 0}, {"snapshot_max_entries", replay.snapshotMaxEntries},
        {"snapshot_zero_entries", replay.zeroSnapshotEntries}, {"replayed_l2_events", replay.replayed},
        {"unanchored_l2_events", replay.unanchored}, {"system_clock_regressions", replay.systemClockRegressions},
        {"torn_tails", tornTails}, {"unindexed_files", unindexed}, {"errors", replay.errors}, {"details", replay.details}, {"channels", channels},
        {"rate_basis", "sum of per-run steady-clock spans; p99 includes idle and partial end seconds; compressed bytes mix channels"}};
    return report;
}
} // namespace

VerificationReport verify(const QString& path) {
    std::vector<File> files;
    if (QFileInfo(path).isFile()) files.push_back({path});
    else if (QFileInfo(path).isDir()) {
        QDirIterator it(path, {"*.rawl2"}, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            if (files.size() >= 100000) throw std::runtime_error("verify at most 100,000 files at a time");
            files.push_back({it.next()});
        }
    }
    std::map<std::string, std::vector<File>> byProduct;
    std::vector<std::string> discoveryErrors;
    for (auto& file : files) {
        try {
            const auto header = readHeader(file.path);
            file.started = header.at("run_started_system_ns").get<int64_t>();
            file.run = header.at("run_id").get<std::string>();
            file.segment = header.at("segment").get<uint64_t>();
            byProduct[header.at("product_metadata").at("product_id").get<std::string>()].push_back(std::move(file));
        } catch (const std::exception& e) {
            // Keep damaged headers in the report; never silently omit a file.
            if (discoveryErrors.size() < 30) discoveryErrors.push_back(file.path.toStdString() + ": " + e.what());
        }
    }
    if (byProduct.empty()) {
        auto report = verifyFiles(std::move(files));
        report.json["products"] = nlohmann::json::object();
        return report;
    }
    // Find the archive root even for a product/month/file query. Newer runs
    // outside that query still supersede stop-less histories.
    QString archiveRoot = QFileInfo(path).isFile() ? QFileInfo(path).absolutePath() : QFileInfo(path).absoluteFilePath();
    bool productScope = QFileInfo(path).isFile();
    if (byProduct.size() == 1) {
        QDir ancestor(archiveRoot);
        do {
            if (ancestor.dirName().toStdString() == byProduct.begin()->first) {
                productScope = true; ancestor.cdUp(); archiveRoot = ancestor.absolutePath(); break;
            }
        } while (ancestor.cdUp());
    }
    std::map<std::string, std::pair<int64_t, std::string>> latest;
    QDirIterator inventory(archiveRoot, {"*.rawl2"}, QDir::Files, QDirIterator::Subdirectories);
    size_t discovered = 0;
    while (inventory.hasNext()) {
        if (++discovered > 100000) throw std::runtime_error("verify at most 100,000 files at a time");
        const auto candidate = inventory.next();
        try {
            const auto header = readHeader(candidate);
            const auto symbol = header.at("product_metadata").at("product_id").get<std::string>();
            const auto key = std::make_pair(header.at("run_started_system_ns").get<int64_t>(), header.at("run_id").get<std::string>());
            latest[symbol] = std::max(latest[symbol], key);
        } catch (const std::exception&) { /* Selected damaged headers are reported above. */ }
    }
    std::map<std::string, std::string> newestRuns;
    for (const auto& [symbol, key] : latest) newestRuns[symbol] = key.second;
    nlohmann::json products = nlohmann::json::object();
    std::map<std::string, std::vector<nlohmann::json>> runs;
    bool ok = discoveryErrors.empty(), closedOk = ok, complete = ok;
    for (auto& [symbol, stream] : byProduct) {
        auto report = verifyFiles(stream, newestRuns);
        ok = ok && report.ok;
        closedOk = closedOk && report.json.at("ok_closed_runs").get<bool>();
        complete = complete && report.json.at("complete").get<bool>();
        for (const auto& run : report.json.at("run_reports")) runs[run.at("run_id").get<std::string>()].push_back(run);
        products[symbol] = std::move(report.json);
    }
    nlohmann::json routingDetails = discoveryErrors, connections = nlohmann::json::array();
    uint64_t routingErrors = discoveryErrors.size(), deferred = 0;
    const auto routingError = [&](const std::string& message) {
        ++routingErrors; ok = closedOk = complete = false;
        if (routingDetails.size() < 30) routingDetails.push_back(message);
    };
    for (const auto& [id, members] : runs) {
        const auto errorsBefore = routingErrors;
        const auto& expected = members.front().at("products");
        std::vector<std::string> present;
        const nlohmann::json* representative = &members.front();
        bool open = false;
        for (const auto& member : members) {
            present.push_back(member.at("symbol").get<std::string>());
            open = open || member.at("open").get<bool>();
            if (member.at("products") != expected) routingError("connection membership mismatch run=" + id);
            if (member.at("frames").get<uint64_t>() > representative->at("frames").get<uint64_t>()) representative = &member;
        }
        std::sort(present.begin(), present.end());
        const bool allPresent = nlohmann::json(present) == expected;
        if (!allPresent && !productScope) routingError("missing product stream run=" + id);
        if (expected.size() > 1 && (!allPresent || open)) ++deferred;
        const bool interrupted = std::any_of(members.begin(), members.end(), [](const auto& m) { return m.at("interrupted") == true; });
        if (expected.size() > 1 && allPresent && !open) {
            std::map<std::string, std::vector<File>> streams;
            for (const auto& [symbol, files] : byProduct) for (const auto& file : files)
                if (file.run == id) streams[symbol].push_back(file);
            try { compareStreams(streams, expected.get<std::vector<std::string>>(), interrupted); }
            catch (const std::exception& e) { routingError("run=" + id + ": " + e.what()); }
        }
        auto connection = *representative;
        connection.erase("symbol");
        connection["ok"] = errorsBefore == routingErrors && std::all_of(members.begin(), members.end(),
            [](const auto& member) { return member.at("ok") == true; });
        connection["incomplete"] = std::any_of(members.begin(), members.end(),
            [](const auto& member) { return member.at("incomplete") == true; });
        connection["closed"] = std::all_of(members.begin(), members.end(),
            [](const auto& member) { return member.at("closed") == true; });
        connection["open"] = open;
        connection["interrupted"] = interrupted;
        connection["routing_checked"] = allPresent && !open;
        connections.push_back(std::move(connection));
    }
    nlohmann::json total = {{"runs", runs.size()}, {"open_runs", 0}, {"interrupted_runs", 0}, {"closed_runs", 0}, {"incomplete_runs", 0}, {"duration_seconds", 0.0}};
    for (const char* key : {"frames", "received_bytes", "connections", "reconnects", "transport_down_events",
                           "sequence_gaps", "unsequenced_frames", "explicit_capture_gaps", "invalidations",
                           "resync_requests", "engine_errors", "capture_stops"}) total[key] = uint64_t(0);
    for (const auto& run : connections) {
        for (const char* key : {"frames", "received_bytes", "connections", "reconnects", "transport_down_events",
                               "sequence_gaps", "unsequenced_frames", "explicit_capture_gaps", "invalidations",
                               "resync_requests", "engine_errors", "capture_stops"})
            total[key] = total[key].get<uint64_t>() + run.at(key).get<uint64_t>();
        total["interrupted_runs"] = total["interrupted_runs"].get<uint64_t>() + (run.at("interrupted") == true);
        total["closed_runs"] = total["closed_runs"].get<uint64_t>() + (run.at("closed") == true);
        total["incomplete_runs"] = total["incomplete_runs"].get<uint64_t>() + (run.at("incomplete") == true);
        total["open_runs"] = total["open_runs"].get<uint64_t>() + (run.at("open") == true);
        total["duration_seconds"] = total["duration_seconds"].get<double>() + run.at("duration_seconds").get<double>();
    }
    for (const char* key : {"files", "blocks", "file_bytes", "zstd_bytes", "uncompressed_record_bytes", "l2_events",
                           "snapshots", "replayed_l2_events", "unanchored_l2_events", "missing_snapshot_connections",
                           "frame_references", "range_receipts", "errors", "torn_tails", "unindexed_files"}) {
        uint64_t sum = 0;
        for (const auto& product : products) sum += product.at(key).get<uint64_t>();
        total[key] = sum;
    }
    uint64_t storedFrames = 0, storedBytes = 0;
    for (const auto& product : products) {
        storedFrames += product.at("frames").get<uint64_t>();
        storedBytes += product.at("received_bytes").get<uint64_t>();
    }
    nlohmann::json totalDays = nlohmann::json::object();
    for (const auto& run : connections) for (const auto& [day, counts] : run.at("days").items()) {
        addDaily(totalDays, day, "frames", counts.at("frames").get<uint64_t>());
        addDaily(totalDays, day, "received_bytes", counts.at("received_bytes").get<uint64_t>());
    }
    for (const auto& product : products) for (const auto& [day, counts] : product.at("days").items()) {
        addDaily(totalDays, day, "file_bytes", counts.at("file_bytes").get<uint64_t>());
        addDaily(totalDays, day, "l2_events", counts.at("l2_events").get<uint64_t>());
    }
    total["days"] = std::move(totalDays);
    total["counts_in_progress"] = total["open_runs"] != 0;
    total["stored_frames"] = storedFrames;
    total["stored_received_bytes"] = storedBytes;
    const auto duration = total["duration_seconds"].get<double>();
    for (const char* key : {"received_bytes", "file_bytes", "zstd_bytes"})
        total[std::string(key) + "_per_day"] = duration > 0 ? total[key].get<double>() / duration * 86400.0 : 0;
    total["fps_mean"] = duration > 0 ? total["frames"].get<double>() / duration : 0;
    // Preserve all v1 top-level keys and their meanings for one-product reports.
    VerificationReport report;
    report.json = products.size() == 1 ? products.begin().value() : total;
    report.ok = ok;
    report.json["ok"] = ok;
    report.json["ok_closed_runs"] = closedOk;
    report.json["complete"] = complete;
    report.json["totals"] = std::move(total);
    report.json["products"] = std::move(products);
    report.json["connection_runs"] = std::move(connections);
    report.json["routing_errors"] = routingErrors;
    report.json["routing_details"] = std::move(routingDetails);
    report.json["routing_checks_deferred"] = deferred;
    report.json["scope"] = productScope ? "product" : "capture-root";
    report.json["open_run_note"] = "Open is the newest run in every member stream without a stop marker; not proof the process is alive. Reverify after close; CLI exit 3 means in progress.";
    return report;
}

} // namespace sentinel::capture
