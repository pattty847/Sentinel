// Restart cost of the startup candle rebuild in the production server graph,
// against SCRATCH copies only (a journal copy and the recorder state copied
// before it: roller.json, checkpoints, anchors, HMC2). Two processes, as in
// production: a capture side and the server (SentinelServerApp ownership
// graph, no listener, lag sampler on).
//
//   candle_startup_bench replay <runDir> <seconds> <product>...
//       Picks a durable ceiling per product <seconds> + 10 s before the end of
//       <runDir>/raw-l2 (the last record of a block), announces it, then
//       replays the journal's own records after it at their original pace:
//       each record as fan-out provisional input, each block end as durable,
//       and every record also into a scratch Writer (capture's disk load).
//       Writes <runDir>/capture.json when the ceilings are announced and
//       <runDir>/capture.done when the replay ends; exits after server.done.
//   candle_startup_bench server <runDir> <restore|baseline> <seconds> <config> <product>...
//       Loads <config> (the checked-in server config), points it at the run
//       directory, serves until the replay ended and commits settled, and
//       prints per product the startup milestones (ms since process start),
//       the rebuild, the install time, readiness at the first trade and, with
//       the rebuild, the 100 ms / 1 s bars compared with a continuous pass over
//       the same journal; then lag, late ticks, RSS and a digest of the HMC2
//       files the run wrote. baseline = the same binary and config with the
//       rebuild switched off (candleRestoreForTest = false).
// Never point it at a live recording root.
#include "../../apps/sentinel-server/SentinelServerApp.hpp"
#include "ConfigLoader.hpp"
#include "capture/CaptureFanout.hpp"
#include "roller/JournalFeed.hpp"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <sys/resource.h>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>

using namespace sentinel;
using namespace sentinel::roller;
using json = nlohmann::json;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

// The production graph without sockets, credentials or the HTTP listener.
struct ShadowServerTestAccess {
    static void prepare(SentinelServerApp& app, const fs::path& dir) {
        app.m_serverModel = std::make_unique<ServerDataModel>(app.m_serverConfig);
        app.m_serverModel->registerMetrics(app.m_metrics);
        app.m_eventLoopLag.registerMetrics(app.m_metrics);
        app.m_eventLoopLag.start();
        app.m_authenticator = std::make_unique<Authenticator>((dir / "no-credentials").string());
        app.m_server = std::make_unique<SentinelStreamServer>(*app.m_serverModel, *app.m_authenticator,
                                                              app.m_serverConfig, 0);
        app.m_serverModel->m_recorderTimer.stop();
    }
    static bool start(SentinelServerApp& app, const std::vector<std::string>& products) {
        return app.startShadow(products);
    }
    static ServerDataModel& model(SentinelServerApp& app) { return *app.m_serverModel; }
    static std::string metrics(SentinelServerApp& app) { return app.m_metrics.render(); }
    static double lag(SentinelServerApp& app, double q) { return app.m_eventLoopLag.quantileMs(q); }
};

namespace {
const Clock::time_point Began = Clock::now();
double sinceStartMs() { return std::chrono::duration<double, std::milli>(Clock::now() - Began).count(); }
int64_t wallMs() { return QDateTime::currentMSecsSinceEpoch(); }
int64_t receiveMs(const JournalRecord& r) { return r.record.time.systemNs / 1'000'000; }

void refuseLiveRoot(const fs::path& root) {
    const auto canonical = fs::weakly_canonical(root).string();
    for (const char* live : {"/Volumes/T7/sentinel-data", "/Users/copeharder/Programming/Sentinel/data",
                             "/Users/copeharder/Sentinel-runtime"})
        if (canonical.starts_with(live)) throw std::runtime_error("refusing a live data root");
}

void writeJson(const fs::path& path, const json& j) {
    const auto temp = path.string() + ".tmp";
    {
        std::ofstream out(temp);
        out << j.dump();
    }
    fs::rename(temp, path);
}
json readJson(const fs::path& path) {
    std::ifstream in(path);
    json j;
    in >> j;
    return j;
}

// The record as capture frames it for the fan-out (length prefix included).
std::string framed(const capture::Record& r) {
    std::string s;
    const auto put = [&](uint64_t v, int n) {
        for (int i = 0; i < n; ++i) s += char(v >> (8 * i));
    };
    put(28 + r.payload.size(), 4);
    put(uint32_t(r.kind), 4);
    put(uint64_t(r.time.systemNs), 8);
    put(uint64_t(r.time.steadyNs), 8);
    put(r.connection, 8);
    return s + r.payload;
}

// Start of the newest journal file opened at or before ms (else the first).
JournalPos fileStartBefore(const JournalReader& all, const std::string& product, int64_t ms) {
    size_t at = 0;
    for (size_t i = 0; i < all.files().size(); ++i)
        if (all.files()[i].header.at("opened_system_ns").get<int64_t>() / 1'000'000 <= ms) at = i;
    const auto& h = all.files()[at].header;
    return {product, h.at("run_id").get<std::string>(), h.at("first_block_ordinal").get<uint64_t>(), 0};
}

struct Replay {
    std::string product;
    JournalPos ceiling;
    int64_t ceilingMs = 0;
    std::vector<JournalRecord> records; // after the ceiling, in journal order
    std::vector<bool> blockEnd;          // records[i] is the last of its block
};

int64_t lastReceiveMs(const fs::path& root, const std::string& product) {
    JournalReader all(root, product);
    if (all.files().empty()) throw std::runtime_error("no journal for " + product);
    const auto& h = all.files().back().header;
    JournalReader reader(root, product,
                         JournalPos{product, h.at("run_id").get<std::string>(),
                                    h.at("first_block_ordinal").get<uint64_t>(), 0});
    JournalRecord r;
    int64_t last = 0;
    while (reader.next(r)) last = std::max(last, receiveMs(r));
    return last;
}

// The last block end at or before cutMs is the ceiling; the records after it
// through cutMs + seconds are replayed.
Replay plan(const fs::path& root, const std::string& product, int64_t cutMs, int seconds) {
    Replay out;
    out.product = product;
    JournalReader all(root, product);
    JournalReader reader(root, product, fileStartBefore(all, product, cutMs - 600'000));
    std::vector<JournalRecord> kept; // from shortly before the cut on
    JournalRecord r;
    const int64_t endMs = cutMs + int64_t(seconds) * 1000;
    while (reader.next(r)) {
        if (receiveMs(r) < cutMs - 30'000) continue;
        if (!kept.empty() && receiveMs(kept.back()) > endMs &&
            (r.pos.run != kept.back().pos.run || r.pos.block != kept.back().pos.block))
            break;
        kept.push_back(r);
    }
    const auto endsBlock = [&](size_t i) {
        return i + 1 == kept.size() || kept[i + 1].pos.run != kept[i].pos.run ||
               kept[i + 1].pos.block != kept[i].pos.block;
    };
    std::optional<size_t> ceiling;
    for (size_t i = 0; i < kept.size(); ++i)
        if (receiveMs(kept[i]) <= cutMs && endsBlock(i)) ceiling = i;
    if (!ceiling || *ceiling + 1 >= kept.size()) throw std::runtime_error("no replay window for " + product);
    out.ceiling = kept[*ceiling].pos;
    out.ceilingMs = receiveMs(kept[*ceiling]);
    for (size_t i = *ceiling + 1; i < kept.size(); ++i) {
        out.records.push_back(kept[i]);
        out.blockEnd.push_back(endsBlock(i));
    }
    return out;
}

// The copied recorder state must not be ahead of the ceiling.
void checkCheckpoint(const fs::path& dir, const Replay& rp) {
    const auto cp = readJson(dir / "hmc2" / rp.product / "roller.json");
    const auto pos = cp.at("pos").get<JournalPos>();
    if (pos.run == rp.ceiling.run && std::pair(pos.block, pos.record) > std::pair(rp.ceiling.block, rp.ceiling.record))
        throw std::runtime_error("copied checkpoint is newer than the ceiling for " + rp.product);
}

int runReplay(const fs::path& dir, int seconds, const std::vector<std::string>& products) {
    const auto root = dir / "raw-l2";
    refuseLiveRoot(root);
    int64_t cutMs = INT64_MAX;
    for (const auto& p : products) cutMs = std::min(cutMs, lastReceiveMs(root, p) - int64_t(seconds + 10) * 1000);
    std::vector<Replay> replays;
    for (const auto& p : products) {
        replays.push_back(plan(root, p, cutMs, seconds));
        checkCheckpoint(dir, replays.back());
    }
    metrics::MetricsRegistry registry;
    const fs::path socketDir = "/tmp/b2r3-bench-" + dir.filename().string();
    fs::create_directories(socketDir);
    fs::permissions(socketDir, fs::perms::owner_all);
    const auto socket = (socketDir / "capture.sock").string();
    if (fs::exists(socket)) fs::remove(socket);
    capture::FanoutConfig fc;
    fc.socketPath = QString::fromStdString(socket);
    capture::CaptureFanout fanout(fc, products, registry, [](const auto&) {});
    std::vector<std::unique_ptr<capture::Writer>> sinks;
    json ceilings = json::object();
    for (size_t i = 0; i < replays.size(); ++i) {
        auto& rp = replays[i];
        fanout.publish(i, {capture::JournalEventKind::Durable, rp.ceiling.run, rp.ceiling.block, rp.ceiling.record,
                           true, {}});
        capture::WriterConfig wc;
        wc.root = QString::fromStdString((dir / "capture-sink").string());
        wc.symbol = products[i];
        wc.fsyncBlocks = 1;
        sinks.push_back(std::make_unique<capture::Writer>(wc, json{{"product_metadata", rp.records.front().metadata}}));
        ceilings[products[i]] = {{"pos", rp.ceiling}, {"ms", rp.ceilingMs}, {"records", rp.records.size()},
                                 {"last", rp.records.back().pos}, {"lastMs", receiveMs(rp.records.back())},
                                 {"prev", rp.records.size() > 1 ? json(rp.records[rp.records.size() - 2].pos)
                                                                : json(rp.ceiling)}};
    }
    for (int i = 0; i < 20; ++i) QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    const int64_t wallStart = wallMs();
    writeJson(dir / "capture.json",
              {{"t0Ms", cutMs}, {"wallStartMs", wallStart}, {"socket", socket}, {"products", ceilings}});
    std::cout << "replay cutMs=" << cutMs << " " << ceilings.dump() << std::endl;
    std::vector<size_t> next(replays.size(), 0);
    size_t done = 0, published = 0;
    while (done < replays.size()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
        const int64_t due = cutMs + (wallMs() - wallStart);
        done = 0;
        for (size_t i = 0; i < replays.size(); ++i) {
            auto& rp = replays[i];
            for (; next[i] < rp.records.size() && receiveMs(rp.records[next[i]]) <= due; ++next[i]) {
                const auto& r = rp.records[next[i]];
                const auto bytes = framed(r.record);
                fanout.publish(i, {capture::JournalEventKind::Record, r.pos.run, r.pos.block, r.pos.record, true, bytes});
                sinks[i]->append(r.record);
                ++published;
                if (rp.blockEnd[next[i]]) {
                    fanout.publish(i, {capture::JournalEventKind::Durable, r.pos.run, r.pos.block, r.pos.record,
                                       true, {}});
                    sinks[i]->flush();
                }
            }
            if (next[i] == rp.records.size()) ++done;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    for (auto& w : sinks) w->close();
    writeJson(dir / "capture.done", {{"published", published}});
    std::cout << "replay done published=" << published << std::endl;
    const auto until = Clock::now() + std::chrono::seconds(240);
    while (!fs::exists(dir / "server.done") && Clock::now() < until) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return 0;
}

namespace logs {
std::mutex mutex;
std::vector<std::pair<double, std::string>> lines;
void handler(QtMsgType type, const QMessageLogContext&, const QString& message) {
    std::lock_guard lock(mutex);
    lines.emplace_back(sinceStartMs(), message.toStdString());
    if (type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg)
        std::cerr << int(lines.back().first) << " " << lines.back().second << "\n";
}
bool matches(const std::string& line, std::initializer_list<std::string> parts) {
    for (const auto& p : parts)
        if (line.find(p) == std::string::npos) return false;
    return true;
}
double at(std::initializer_list<std::string> parts) {
    std::lock_guard lock(mutex);
    for (const auto& [ms, line] : lines)
        if (matches(line, parts)) return ms;
    return -1;
}
std::string line(std::initializer_list<std::string> parts) {
    std::lock_guard lock(mutex);
    for (const auto& [ms, l] : lines)
        if (matches(l, parts)) return l;
    return {};
}
std::string field(const std::string& line, const std::string& key) {
    const auto at = line.find(" " + key + "=");
    if (at == std::string::npos) return "-";
    const auto begin = at + key.size() + 2;
    return line.substr(begin, line.find(' ', begin) - begin);
}
} // namespace logs

double metric(const std::string& text, const std::string& series) {
    const auto at = text.find("\n" + series + " ");
    return at == std::string::npos ? -1 : std::stod(text.substr(at + series.size() + 2));
}
int64_t committed(const fs::path& dir) {
    try {
        return readJson(dir / "roller.json").value("committedThroughMs", int64_t{0});
    } catch (const std::exception&) {
        return 0;
    }
}
// HMC2 files this run wrote (the checker's oracle excluded: it depends on
// when the hourly check ran).
std::string digest(const fs::path& root, fs::file_time_type since) {
    std::vector<fs::path> files;
    for (const auto& e : fs::recursive_directory_iterator(root))
        if (e.path().extension() == ".hmc2" && e.last_write_time() >= since &&
            fs::relative(e.path(), root).string().find("shadow-oracle") == std::string::npos)
            files.push_back(e.path());
    std::sort(files.begin(), files.end());
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (const auto& f : files) {
        std::ifstream in(f, std::ios::binary);
        const std::string bytes{std::istreambuf_iterator<char>(in), {}};
        const auto name = fs::relative(f, root).string();
        hash.addData(QByteArrayView(name.data(), qsizetype(name.size())));
        hash.addData(QByteArrayView(bytes.data(), qsizetype(bytes.size())));
    }
    return std::to_string(files.size()) + ":" + hash.result().toHex().left(16).toStdString();
}

// Continuous pass over the run's journal from about an hour before the window
// through the last replayed record (it must be reached).
struct Reference {
    std::unique_ptr<TimeframeAggregator> aggregator;
    bool reachedLast = false;
    // Closed bars per timeframe at or after the window start: the complete
    // window the model must hold once it has processed the last record.
    std::map<int64_t, std::vector<OHLCVBar>> window;
};
Reference reference(const fs::path& root, const std::string& product, int64_t windowStartMs, const JournalPos& last) {
    Reference out;
    out.aggregator = std::make_unique<TimeframeAggregator>(std::vector<int64_t>{100, 1000, 60000},
                                                           std::map<int64_t, int>{{100, 60000}, {1000, 10000}});
    JournalReader all(root, product);
    JournalReader reader(root, product, fileStartBefore(all, product, windowStartMs - 3'600'000));
    JournalFeed feed(product);
    feed.onTrade = [&](const Trade& t) { out.aggregator->onTrade(t); };
    int64_t sent = INT64_MIN;
    JournalRecord r;
    while (reader.next(r)) {
        feed.apply(r);
        const int64_t ms = receiveMs(r);
        if (ms / 1000 > sent) {
            sent = ms / 1000;
            out.aggregator->advance(product, ms - ServerDataModel::kJournalCandleLagMs);
        }
        if (r.pos == last) {
            out.reachedLast = true;
            break;
        }
    }
    for (const int64_t tf : {int64_t{100}, int64_t{1000}})
        for (const auto& b : out.aggregator->getHistory(product, tf, 100000))
            if (b.timestamp_ms >= windowStartMs) out.window[tf].push_back(b);
    return out;
}
bool sameBar(const OHLCVBar& a, const OHLCVBar& b) {
    return std::tie(a.timestamp_ms, a.open, a.high, a.low, a.close, a.volume, a.count) ==
           std::tie(b.timestamp_ms, b.open, b.high, b.low, b.close, b.volume, b.count);
}
// Differences between the model's closed bars at or after the window start
// and the reference window, bounded by the reference only: a missing or extra
// bar, the newest included, counts.
size_t differences(const Reference& ref, int64_t windowStartMs, const std::map<int64_t, std::vector<OHLCVBar>>& model) {
    size_t differing = 0;
    for (const auto& [tf, want] : ref.window) {
        std::vector<OHLCVBar> got;
        if (const auto it = model.find(tf); it != model.end())
            for (const auto& b : it->second)
                if (b.timestamp_ms >= windowStartMs) got.push_back(b);
        const size_t n = std::max(got.size(), want.size());
        for (size_t i = 0; i < n; ++i)
            if (i >= got.size() || i >= want.size() || !sameBar(got[i], want[i])) ++differing;
    }
    if (ref.window.size() != 2) ++differing;
    return differing;
}
// Completion markers, main thread only. The roller's durable observer
// (ShadowConfig::observeForTest, after tap.durableApplied() and its model
// posts) queues one per product when it applies the replay's exact last
// record, and one at the record before it (the negative check).
struct Marks {
    bool terminal = false, prefixSeen = false, prefixBeforeTerminal = false;
    std::map<int64_t, OHLCVBar> prefixNewest; // model's newest closed bars at the prefix marker
};

int runServer(const fs::path& dir, bool restore, int seconds, const std::string& configPath,
              const std::vector<std::string>& products) {
    refuseLiveRoot(dir / "raw-l2");
    refuseLiveRoot(dir / "hmc2");
    fs::create_directories(dir / "primary");
    const auto runStart = fs::file_time_type::clock::now();
    ServerConfig config;
    if (!ConfigLoader::loadServerConfig(configPath, &config)) throw std::runtime_error("config " + configPath);
    const auto until = Clock::now() + std::chrono::seconds(120);
    while (!fs::exists(dir / "capture.json") && Clock::now() < until)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const json clock = readJson(dir / "capture.json");
    const int64_t t0 = clock.at("t0Ms"), wallStart = clock.at("wallStartMs");
    const double startMs = sinceStartMs();
    QDir::setCurrent(QString::fromStdString(dir.string()));
    qInstallMessageHandler(logs::handler);
    config.defaultSymbols = products;
    config.streamPort = 0;
    config.recording.enabled = true;
    config.recording.source = "roller";
    config.recording.dir = (dir / "primary").string();
    config.recording.fallbackDir.clear();
    auto& shadow = config.rollerShadow;
    shadow.enabled = true;
    shadow.journalRoot = (dir / "raw-l2").string();
    shadow.outputRoot = (dir / "hmc2").string();
    shadow.socketPath = clock.at("socket").get<std::string>();
    shadow.products = products;
    shadow.liveNowForTest = [t0, wallStart] { return t0 + (wallMs() - wallStart); };
    shadow.candleRestoreForTest = restore;
    std::map<std::string, std::pair<JournalPos, JournalPos>> ends; // product -> (prev, last)
    for (const auto& p : products)
        ends[p] = {clock.at("products").at(p).at("prev").get<JournalPos>(),
                   clock.at("products").at(p).at("last").get<JournalPos>()};
    auto receiver = std::make_shared<std::atomic<ServerDataModel*>>(nullptr);
    std::map<std::string, Marks> marks;
    for (const auto& p : products) marks[p];
    shadow.observeForTest = [receiver, ends, &marks](const JournalRecord& r, bool applied) {
        if (!applied) return;
        const auto it = ends.find(r.pos.product);
        if (it == ends.end()) return;
        const bool last = r.pos == it->second.second, prev = r.pos == it->second.first;
        auto* model = receiver->load();
        if (!model || !(last || prev)) return;
        const std::string product = r.pos.product;
        QMetaObject::invokeMethod(
            model,
            [model, product, last, &marks] {
                auto& m = marks[product];
                if (last) {
                    m.terminal = true;
                    return;
                }
                m.prefixSeen = true;
                m.prefixBeforeTerminal = !m.terminal;
                for (const int64_t tf : {int64_t{100}, int64_t{1000}})
                    if (const auto h = model->getHistory(product, tf, 1); !h.empty()) m.prefixNewest[tf] = h.back();
            },
            Qt::QueuedConnection);
    };
    auto app = std::make_unique<SentinelServerApp>(config);
    ShadowServerTestAccess::prepare(*app, dir);
    auto& model = ShadowServerTestAccess::model(*app);
    receiver->store(&model);
    std::map<std::string, int> readyAtFirstTrade; // -1 not seen, 0 false, 1 true
    for (const auto& p : products) readyAtFirstTrade[p] = -1;
    QObject::connect(&model, &ServerDataModel::tradeBroadcast, [&](const Trade& t) {
        auto& seen = readyAtFirstTrade[t.product_id];
        if (seen < 0) seen = model.candleHistoryReady(t.product_id) ? 1 : 0;
    });
    const auto lateBefore = metric(ShadowServerTestAccess::metrics(*app), "sentinel_server_event_loop_late_ticks_total");
    if (!ShadowServerTestAccess::start(*app, products)) throw std::runtime_error("server start failed");
    double lagMax = 0, p99After60 = 0;
    std::vector<std::pair<double, double>> samples;
    auto nextSample = Clock::now() + std::chrono::seconds(1);
    const auto measuredUntil = Clock::now() + std::chrono::seconds(seconds);
    std::map<std::string, int64_t> stable;
    auto stableSince = Clock::now();
    const auto hardStop = Clock::now() + std::chrono::seconds(seconds + 150);
    for (;;) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        const auto now = Clock::now();
        if (now >= nextSample) {
            nextSample += std::chrono::seconds(1);
            const double p99 = ShadowServerTestAccess::lag(*app, 0.99), max = ShadowServerTestAccess::lag(*app, 1);
            samples.emplace_back(p99, max);
            if (now <= measuredUntil) {
                lagMax = std::max(lagMax, max);
                if (sinceStartMs() - startMs >= 60'000) p99After60 = std::max(p99After60, p99);
            }
        }
        if (now < measuredUntil) continue;
        if (now > hardStop) break;
        if (!fs::exists(dir / "capture.done")) continue;
        std::map<std::string, int64_t> c;
        for (const auto& p : products) c[p] = committed(dir / "hmc2" / p);
        if (c != stable) {
            stable = c;
            stableSince = now;
        } else if (now - stableSince > std::chrono::seconds(8)) {
            break;
        }
    }
    // Peak RSS of the serving run, before any comparison work or copy.
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    const auto text = ShadowServerTestAccess::metrics(*app);
    const auto lateAfter = metric(text, "sentinel_server_event_loop_late_ticks_total");
    const double servingReady = logs::at({"Roller serving ready products="});
    double failures = 0;
    for (const auto& p : products)
        failures += std::max(0.0, metric(text, "sentinel_candle_history_restore_failures_total{product=\"" + p + "\"}"));
    const json ceilings = clock.at("products");
    std::map<std::string, std::string> cmp;
    size_t differingTotal = 0;
    bool strictOk = true;
    for (const auto& p : products) {
        const auto restored = logs::line({"Candle history restored product=" + p + " "});
        if (!restore || restored.empty()) {
            cmp[p] = "n/a";
            continue;
        }
        const int64_t windowStartMs = std::stoll(logs::field(restored, "throughMs")) - kRestoreWindowMs;
        const auto ref = reference(dir / "raw-l2", p, windowStartMs, ceilings.at(p).at("last").get<JournalPos>());
        // Wait for the marker queued after the model posts of the exact last record.
        const auto deadline = Clock::now() + std::chrono::seconds(60);
        while (!marks[p].terminal && Clock::now() < deadline)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        const bool terminal = marks[p].terminal;
        // Negative check: at the record before the last one the marker had not
        // completed; report whether its closed bars already equalled the final
        // ones (the state an equal-newest-bar signal would have accepted).
        const auto& mk = marks[p];
        bool prefixBarsEqual = mk.prefixSeen && ref.window.size() == 2;
        for (const auto& [tf, want] : ref.window) {
            const auto it = mk.prefixNewest.find(tf);
            prefixBarsEqual = prefixBarsEqual && !want.empty() && it != mk.prefixNewest.end() &&
                              sameBar(it->second, want.back());
        }
        const bool negative = mk.prefixSeen && mk.prefixBeforeTerminal;
        std::map<int64_t, std::vector<OHLCVBar>> bars;
        for (const int64_t tf : {int64_t{100}, int64_t{1000}}) bars[tf] = model.getHistory(p, tf, 100000);
        const size_t differing = differences(ref, windowStartMs, bars);
        // The comparison fails without the model's newest bar.
        auto shorter = bars;
        if (!shorter[1000].empty()) shorter[1000].pop_back();
        const bool selfCheck = differences(ref, windowStartMs, shorter) > 0;
        strictOk = strictOk && ref.reachedLast && terminal && selfCheck && negative;
        differingTotal += differing;
        cmp[p] = std::to_string(ref.window.at(100).size() + ref.window.at(1000).size()) + "/" +
                 std::to_string(differing) + " reachedLast=" + (ref.reachedLast ? "1" : "0") +
                 " terminal=" + (terminal ? "1" : "0") + " selfCheck=" + (selfCheck ? "1" : "0") +
                 " prefixIncomplete=" + (negative ? "1" : "0") + " prefixBarsEqual=" + (prefixBarsEqual ? "1" : "0") +
                 " newest1000=" + std::to_string(ref.window.at(1000).back().timestamp_ms);
    }
    app.reset();
    for (const auto& p : products) {
        const auto restored = logs::line({"Candle history restored product=" + p + " "});
        const auto ready = logs::line({"Candle history ready symbol=" + p + " "});
        std::cout << "product=" << p << " mode=" << (restore ? "restore" : "baseline")
                  << " startedMs=" << logs::at({"Roller started product=" + p + " "})
                  << " restoreStartMs=" << logs::at({"Candle history restore start product=" + p + " "})
                  << " restoredMs=" << logs::at({"Candle history restored product=" + p + " "})
                  << " readyMs=" << logs::at({"Candle history ready symbol=" + p + " "})
                  << " seedMs=" << logs::at({"Journal live seed product=" + p + " "})
                  << " healthyMs=" << logs::at({"Roller writer healthy product=" + p})
                  << " outcome=" << logs::field(restored, "outcome") << " files=" << logs::field(restored, "files")
                  << " records=" << logs::field(restored, "records") << " trades=" << logs::field(restored, "trades")
                  << " bars100=" << logs::field(restored, "bars100")
                  << " bars1000=" << logs::field(restored, "bars1000") << " readMs=" << logs::field(restored, "readMs")
                  << " installUs=" << logs::field(ready, "installUs") << " restored=" << logs::field(ready, "restored")
                  << " readyAtFirstTrade=" << readyAtFirstTrade[p] << " barsExpectedDiffering=" << cmp[p]
                  << " committedMs=" << stable[p] << std::endl;
    }
    std::cout << "total mode=" << (restore ? "restore" : "baseline") << " products=" << products.size()
              << " servingReadyMs=" << servingReady << " lagMaxMs=" << lagMax << " lagP99After60sMs=" << p99After60
              << " lateTicks=" << (lateAfter - std::max(0.0, lateBefore))
              << " maxRssMb=" << double(usage.ru_maxrss) / (1024.0 * 1024.0) << " restoreFailures=" << failures
              << " differingBars=" << (restore ? std::to_string(differingTotal) : "n/a")
              << " strict=" << (restore ? (strictOk ? "ok" : "FAIL") : "n/a")
              << " hmc2=" << digest(dir / "hmc2", runStart) << std::endl;
    std::cout << "lag per second (p99,max):";
    for (const auto& [p99, max] : samples) std::cout << " " << p99 << "," << max;
    std::cout << std::endl;
    {
        std::ofstream log(dir / "server.log");
        std::lock_guard lock(logs::mutex);
        for (const auto& [ms, l] : logs::lines) log << int64_t(ms) << " " << l << "\n";
    }
    writeJson(dir / "server.done", {{"ok", true}});
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        const std::vector<std::string> args(argv + 1, argv + argc);
        if (args.size() >= 4 && args[0] == "replay")
            return runReplay(args[1], std::stoi(args[2]), {args.begin() + 3, args.end()});
        if (args.size() >= 6 && args[0] == "server" && (args[2] == "restore" || args[2] == "baseline"))
            return runServer(args[1], args[2] == "restore", std::stoi(args[3]), args[4],
                             {args.begin() + 5, args.end()});
    } catch (const std::exception& e) {
        std::cerr << "candle_startup_bench: " << e.what() << std::endl;
        return 1;
    }
    std::cerr << "usage: candle_startup_bench replay <runDir> <seconds> <product>...\n"
                 "       candle_startup_bench server <runDir> <restore|baseline> <seconds> <config> <product>..."
              << std::endl;
    return 1;
}
