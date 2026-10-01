#include "CaptureVerifier.hpp"
#include <QDirIterator>
#include <QCryptographicHash>
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
    bool multi = false, snapshotPending = false;
    uint64_t connectionFrames = 0, connectionBytes = 0, references = 0, missingSnapshots = 0, l2Events = 0;
    uint64_t openRuns = 0, badTails = 0;
    bool lastIndexed = false;
    bool closedOk = true;
    QCryptographicHash digest{QCryptographicHash::Sha256};
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
        const bool open = sawStart && !sawStop;
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
        metrics["incomplete"] = !sawStart || !sawStop || !sawSnapshot;
        metrics["open"] = open;
        metrics["closed"] = sawStop;
        metrics["duration_seconds"] = span;
        metrics["stream_sha256"] = digest.result().toHex().toStdString();
        runReports.push_back(std::move(metrics));
        if (!open && !valid) closedOk = false;
        baseline = connectionMetrics();
        sawStart = sawStop = sawSnapshot = snapshotPending = false;
        firstSteady = lastSteady = second = previousSystem = -1;
        secondFrames = 0;
        connection = expectedSequence = 0;
        haveConnection = active = haveSequence = false;
        digest.reset();
        invalidate();
    }
    void record(const Record& record) {
        const bool reference = record.kind == Kind::FrameReference;
        nlohmann::json receipt;
        if (multi) {
            try {
                const bool frame = record.kind == Kind::Frame || reference;
                if (frame) {
                    receipt = reference ? nlohmann::json::parse(record.payload) : frameReceipt(record.payload, products);
                    if (reference) validateReceipt(receipt, products);
                    const auto targets = receipt.at("products").get<std::vector<std::string>>();
                    const bool targeted = std::binary_search(targets.begin(), targets.end(), symbol);
                    if (targeted == reference) throw std::runtime_error("frame/reference routed to wrong product");
                }
                const auto identity = nlohmann::json::array({frame ? uint32_t(Kind::Frame) : uint32_t(record.kind),
                    record.time.systemNs, record.time.steadyNs, record.connection,
                    frame ? receipt.dump() : record.payload}).dump() + "\n";
                digest.addData(QByteArrayView(identity.data(), identity.size()));
            } catch (const std::exception& e) { error(e.what()); invalidate(); return; }
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
        if (reference) ++references;
        else { ++frames; ++secondFrames; bytes += record.payload.size(); }
        ++connectionFrames;
        const auto receivedBytes = reference ? receipt.at("received_bytes").get<uint64_t>() : record.payload.size();
        connectionBytes += receivedBytes;
        const auto day = utcDay(record.time.systemNs);
        addDaily(connectionDays, day, "frames", 1);
        addDaily(connectionDays, day, "received_bytes", receivedBytes);
        if (!reference) {
            addDaily(days, day, "frames", 1);
            addDaily(days, day, "received_bytes", receivedBytes);
        }
        if (record.connection == 0) error("frame without connection id");
        if (!haveConnection) {
            // A single hourly file may start halfway through an existing connection.
            connection = record.connection; haveConnection = active = true;
        } else if (record.connection != connection || !active) {
            error("frame outside its transport lifetime"); invalidate();
        }
        bool channelCounted = false;
        try {
            const auto json = reference ? receipt : nlohmann::json::parse(record.payload);
            const auto channel = json.value("channel", std::string("<unclassified>"));
            if (channel.size() > 128) throw std::runtime_error("channel name too long");
            if (channels.size() >= 1024 && !channels.contains(channel)) throw std::runtime_error("too many channels");
            if (!reference) { ++channels[channel].frames; channels[channel].bytes += record.payload.size(); }
            channelCounted = true;
            bool gap = false;
            if (json.contains("sequence_num") && json["sequence_num"].is_number_integer() &&
                (!json["sequence_num"].is_number_unsigned() ? json["sequence_num"].get<int64_t>() >= 0 : true)) {
                const auto sequence = json["sequence_num"].get<uint64_t>();
                if (haveSequence && sequence != expectedSequence) {
                    ++gaps; invalidate(); gap = true;
                    if (details.size() < 30) details.push_back("sequence gap connection=" + std::to_string(connection) +
                        " expected=" + std::to_string(expectedSequence) + " got=" + std::to_string(sequence));
                }
                if (sequence == UINT64_MAX) throw std::runtime_error("sequence overflow");
                expectedSequence = sequence + 1; haveSequence = true;
            } else {
                ++unsequenced; invalidate();
            }
            if (channel == "subscriptions") ++acks; // exact ack remains a frame, not reconstructed JSON
            if (reference || channel != "l2_data") return;
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

VerificationReport verifyFiles(std::vector<File> files) {
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
    nlohmann::json products = nlohmann::json::object();
    std::map<std::string, std::vector<nlohmann::json>> runs;
    bool ok = discoveryErrors.empty(), closedOk = ok, complete = ok;
    for (auto& [symbol, stream] : byProduct) {
        auto report = verifyFiles(std::move(stream));
        ok = ok && report.ok;
        closedOk = closedOk && report.json.at("ok_closed_runs").get<bool>();
        complete = complete && report.json.at("complete").get<bool>();
        for (const auto& run : report.json.at("run_reports")) runs[run.at("run_id").get<std::string>()].push_back(run);
        products[symbol] = std::move(report.json);
    }
    const bool productScope = QFileInfo(path).isFile() ||
        (byProduct.size() == 1 && QFileInfo(QDir::cleanPath(path)).fileName().toStdString() == byProduct.begin()->first);
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
        if (expected.size() > 1 && (!allPresent || open)) { ++deferred; complete = false; }
        if (allPresent && !open) {
            for (const auto& member : members)
                if (member.at("stream_sha256") != members.front().at("stream_sha256")) {
                    routingError("routed stream bytes/clocks/order differ run=" + id); break;
                }
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
        connection["routing_checked"] = allPresent && !open;
        connections.push_back(std::move(connection));
    }
    nlohmann::json total = {{"runs", runs.size()}, {"open_runs", 0}, {"closed_runs", 0}, {"incomplete_runs", 0}, {"duration_seconds", 0.0}};
    for (const char* key : {"frames", "received_bytes", "connections", "reconnects", "transport_down_events",
                           "sequence_gaps", "unsequenced_frames", "explicit_capture_gaps", "invalidations",
                           "resync_requests", "engine_errors", "capture_stops"}) total[key] = uint64_t(0);
    for (const auto& run : connections) {
        for (const char* key : {"frames", "received_bytes", "connections", "reconnects", "transport_down_events",
                               "sequence_gaps", "unsequenced_frames", "explicit_capture_gaps", "invalidations",
                               "resync_requests", "engine_errors", "capture_stops"})
            total[key] = total[key].get<uint64_t>() + run.at(key).get<uint64_t>();
        total["closed_runs"] = total["closed_runs"].get<uint64_t>() + (run.at("closed") == true);
        total["incomplete_runs"] = total["incomplete_runs"].get<uint64_t>() + (run.at("incomplete") == true);
        total["open_runs"] = total["open_runs"].get<uint64_t>() + (run.at("open") == true);
        total["duration_seconds"] = total["duration_seconds"].get<double>() + run.at("duration_seconds").get<double>();
    }
    for (const char* key : {"files", "blocks", "file_bytes", "zstd_bytes", "uncompressed_record_bytes", "l2_events",
                           "snapshots", "replayed_l2_events", "unanchored_l2_events", "missing_snapshot_connections",
                           "frame_references", "errors", "torn_tails", "unindexed_files"}) {
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
    report.json["open_run_note"] = "Open means no stop marker observed; valid readable prefix only, not proof the process is alive. Reverify after close.";
    return report;
}

} // namespace sentinel::capture
