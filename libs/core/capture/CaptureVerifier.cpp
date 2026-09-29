#include "CaptureVerifier.hpp"
#include <QDirIterator>
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
struct Replay {
    std::optional<DecimalGrid> prices, quantities;
    std::string symbol;
    std::map<uint64_t, uint64_t> bids, asks;
    uint64_t connection = 0, expectedSequence = 0;
    bool haveConnection = false, active = false, haveSequence = false, bookValid = false;
    uint64_t frames = 0, bytes = 0, connections = 0, reconnects = 0, downs = 0, gaps = 0;
    uint64_t invalidations = 0, resyncs = 0, acks = 0, errors = 0, unsequenced = 0;
    uint64_t unanchored = 0, replayed = 0, snapshots = 0, snapshotFrames = 0, snapshotBytes = 0;
    uint64_t snapshotMinBytes = UINT64_MAX, snapshotMaxBytes = 0;
    uint64_t snapshotEntries = 0, snapshotMinEntries = UINT64_MAX, snapshotMaxEntries = 0, zeroSnapshotEntries = 0;
    uint64_t systemClockRegressions = 0, captureStops = 0, engineErrors = 0, incompleteRuns = 0;
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
    void finishRun() {
        if (firstSteady >= 0) {
            finishSecond();
            if (!sawStart || !sawStop || !sawSnapshot) ++incompleteRuns;
            duration += std::max(1e-9, double(lastSteady - firstSteady) / 1e9);
        }
        sawStart = sawStop = sawSnapshot = false;
        firstSteady = lastSteady = second = previousSystem = -1;
        secondFrames = 0;
        connection = expectedSequence = 0;
        haveConnection = active = haveSequence = false;
        invalidate();
    }
    void record(const Record& record) {
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
            if (haveConnection && record.connection != connection + 1) error("non-contiguous connection id");
            if (record.connection == 0) error("zero connection id on transport up");
            if (connections && haveConnection) ++reconnects;
            ++connections; connection = record.connection;
            haveConnection = active = haveSequence = true;
            expectedSequence = 0; invalidate(); return;
        }
        if (record.kind == Kind::CaptureStarted) { sawStart = true; return; }
        if (record.kind == Kind::CaptureStopped) { ++captureStops; sawStop = true; active = false; invalidate(); return; }
        if (record.kind == Kind::EngineError) { ++engineErrors; return; }
        if (record.kind == Kind::TransportDown) { ++downs; active = false; invalidate(); return; }
        if (record.kind == Kind::BookInvalidated) { ++invalidations; invalidate(); return; }
        if (record.kind == Kind::ResyncRequested) { ++resyncs; invalidate(); return; }
        ++frames; ++secondFrames; bytes += record.payload.size();
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
            ++channels[channel].frames; channels[channel].bytes += record.payload.size(); channelCounted = true;
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
            if (channel != "l2_data") return;
            bool snapshotFrame = false;
            if (!json.at("events").is_array()) throw std::runtime_error("L2 events must be an array");
            for (const auto& event : json.at("events")) {
                if (event.at("product_id").get<std::string>() != symbol) throw std::runtime_error("unexpected L2 product");
                const auto type = event.at("type").get<std::string>();
                const bool snapshot = type == "snapshot";
                if (!snapshot && type != "update") throw std::runtime_error("unknown L2 event type");
                const auto& updates = event.at("updates");
                if (!updates.is_array()) throw std::runtime_error("updates must be an array");
                if (snapshot) {
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
} // namespace

VerificationReport verify(const QString& path) {
    VerificationReport report;
    Replay replay;
    uint64_t fileBytes = 0, compressedBytes = 0, rawBlockBytes = 0, blocks = 0, tornTails = 0, unindexed = 0, scanned = 0;
    std::vector<File> files;
    if (QFileInfo(path).isFile()) {
        files.push_back({path});
    } else if (QFileInfo(path).isDir()) {
        QDirIterator it(path, {"*.rawl2"}, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            if (files.size() >= 100000) throw std::runtime_error("verify at most 100,000 files at a time");
            files.push_back({it.next()});
        }
    }
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
    for (const auto& file : readable) {
        try {
            const auto header = readHeader(file.path);
            const auto& thisRun = file.run;
            const auto segment = file.segment;
            const auto firstBlock = header.at("first_block_ordinal").get<uint64_t>();
            if (thisRun != run) {
                const auto nextProduct = header.at("product_metadata");
                DecimalGrid nextPrices(nextProduct.at("quote_increment").get<std::string>());
                DecimalGrid nextQuantities(nextProduct.at("base_increment").get<std::string>());
                replay.finishRun(); run = thisRun; ++runs;
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
            if (product != header.at("product_metadata")) throw std::runtime_error("product metadata changed within run");
            const auto result = scan(file.path, [&](const Record& record) { replay.record(record); });
            ++scanned;
            fileBytes += result.fileBytes;
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
    if (scanned == 0 || replay.frames == 0) replay.error("no captured frames verified");
    uint64_t p99 = 0, count = 0;
    const auto rank = static_cast<uint64_t>(std::ceil(replay.seconds * .99));
    for (; p99 < replay.histogram.size(); ++p99) {
        count += replay.histogram[p99]; if (count >= rank) break;
    }
    const auto perDay = [&](uint64_t bytes) { return replay.duration > 0 ? double(bytes) / replay.duration * 86400.0 : 0; };
    nlohmann::json channels = nlohmann::json::object();
    for (const auto& [name, value] : replay.channels)
        channels[name] = {{"frames", value.frames}, {"received_bytes", value.bytes}, {"received_bytes_per_day", perDay(value.bytes)}};
    report.ok = replay.errors == 0 && replay.gaps == 0 && replay.unsequenced == 0 && replay.unanchored == 0 && tornTails == 0 && unindexed == 0 && replay.incompleteRuns == 0;
    report.json = {{"ok", report.ok}, {"files", scanned}, {"runs", runs}, {"blocks", blocks},
        {"frames", replay.frames}, {"duration_seconds", replay.duration}, {"fps_mean", replay.duration > 0 ? replay.frames / replay.duration : 0},
        {"fps_p99_one_second_buckets", p99}, {"fps_bucket_count_including_idle", replay.seconds},
        {"received_bytes", replay.bytes}, {"received_bytes_per_day", perDay(replay.bytes)},
        {"file_bytes", fileBytes}, {"file_bytes_per_day", perDay(fileBytes)}, {"zstd_bytes", compressedBytes},
        {"zstd_bytes_per_day", perDay(compressedBytes)}, {"uncompressed_record_bytes", rawBlockBytes},
        {"connections", replay.connections}, {"reconnects", replay.reconnects}, {"transport_down_events", replay.downs},
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
} // namespace sentinel::capture
