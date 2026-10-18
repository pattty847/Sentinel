// Restart cost of the startup candle rebuild in the production server graph,
// against SCRATCH journal and roller copies only. Two processes, as in
// production: a capture side (fan-out and one Writer per product) and the
// server (SentinelServerApp ownership graph, no listener, lag sampler on).
//
//   candle_startup_bench capture <runDir> <seconds> <product>...
//       Journals into <runDir>/raw-l2 after its newest record: per product a
//       book snapshot and a heartbeat at T0 (the durable ceiling the server
//       finds), then one trade every 100 ms and a heartbeat every second at
//       the original pace for <seconds>. Writes <runDir>/capture.json when the
//       ceiling is durable, <runDir>/capture.done when it stops appending, and
//       exits after <runDir>/server.done.
//   candle_startup_bench server <runDir> <restore|baseline> <seconds> <product>...
//       Serves <runDir>/raw-l2 into <runDir>/hmc2 (roller.json and anchors from
//       an earlier run), samples the main-thread lag every second, and prints
//       per product the startup milestones (ms since process start), the
//       install time, readiness at the first trade, and overall lag, RSS and
//       an HMC2 digest. baseline = the same binary and chart config with the
//       rebuild switched off (candleRestoreForTest = false).
// Never point it at a live recording root.
#include "../../apps/sentinel-server/SentinelServerApp.hpp"
#include "capture/CaptureFanout.hpp"
#include "roller/JournalFeed.hpp"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <sys/resource.h>
#include <chrono>
#include <cstdio>
#include <ctime>
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

void refuseLiveRoot(const fs::path& root) {
    const auto canonical = fs::weakly_canonical(root).string();
    for (const char* live : {"/Volumes/T7/sentinel-data", "/Users/copeharder/Programming/Sentinel/data"})
        if (canonical.starts_with(live)) throw std::runtime_error("refusing a live data root");
}

std::string iso(int64_t ms) {
    const time_t s = static_cast<time_t>(ms / 1000);
    tm g{};
    gmtime_r(&s, &g);
    char buf[40];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", g.tm_year + 1900, g.tm_mon + 1,
                  g.tm_mday, g.tm_hour, g.tm_min, g.tm_sec, int(ms % 1000));
    return buf;
}
std::string priceText(double p) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.8f", p);
    return buf;
}

struct Tail {
    json metadata;
    int64_t lastMs = 0;
    double lastPrice = 0;
};
// The newest file of a product: its header metadata, last receive time and
// last trade price.
Tail tail(const fs::path& root, const std::string& product) {
    JournalReader all(root, product);
    if (all.files().empty()) throw std::runtime_error("no journal for " + product);
    const auto& header = all.files().back().header;
    Tail t;
    t.metadata = header.at("product_metadata");
    JournalReader reader(root, product,
                         JournalPos{product, header.at("run_id").get<std::string>(),
                                    header.at("first_block_ordinal").get<uint64_t>(), 0});
    JournalFeed feed(product);
    feed.onTrade = [&](const Trade& trade) { t.lastPrice = trade.price; };
    JournalRecord r;
    while (reader.next(r)) {
        feed.apply(r);
        t.lastMs = std::max(t.lastMs, r.record.time.systemNs / 1'000'000);
    }
    if (t.lastPrice <= 0) t.lastPrice = 1;
    return t;
}

capture::Record frame(int64_t ms, std::string payload) {
    return {capture::Kind::Frame, {ms * 1'000'000, capture::Stamp::now().steadyNs}, 1, std::move(payload)};
}
std::string snapshot(const std::string& product, double price) {
    return json{{"channel", "l2_data"},
                {"events", json::array({{{"type", "snapshot"},
                                         {"product_id", product},
                                         {"updates", json::array({{{"side", "bid"},
                                                                   {"price_level", priceText(price * 0.9999)},
                                                                   {"new_quantity", "5"}},
                                                                  {{"side", "offer"},
                                                                   {"price_level", priceText(price * 1.0001)},
                                                                   {"new_quantity", "5"}}})}}})}}
        .dump();
}
std::string trade(const std::string& product, const std::string& id, int64_t ms, double price) {
    return json{{"channel", "market_trades"},
                {"events", json::array({{{"type", "update"},
                                         {"trades", json::array({{{"trade_id", id},
                                                                  {"product_id", product},
                                                                  {"price", priceText(price)},
                                                                  {"size", "0.01"},
                                                                  {"side", "BUY"},
                                                                  {"time", iso(ms - 150)}}})}}})}}
        .dump();
}

void writeJson(const fs::path& path, const json& j) {
    const auto temp = path.string() + ".tmp";
    {
        std::ofstream out(temp);
        out << j.dump();
    }
    fs::rename(temp, path);
}

int runCapture(const fs::path& dir, int seconds, const std::vector<std::string>& products) {
    const auto root = dir / "raw-l2";
    refuseLiveRoot(root);
    std::vector<Tail> tails;
    int64_t t0 = 0;
    for (const auto& p : products) {
        tails.push_back(tail(root, p));
        t0 = std::max(t0, tails.back().lastMs);
    }
    t0 = (t0 / 1000 + 1) * 1000;
    metrics::MetricsRegistry registry;
    // The fan-out refuses sockets on volumes: a private directory under /tmp.
    const fs::path socketDir = "/tmp/b2r3-bench-" + dir.filename().string();
    fs::remove_all(socketDir);
    fs::create_directories(socketDir);
    fs::permissions(socketDir, fs::perms::owner_all);
    const auto socket = (socketDir / "capture.sock").string();
    capture::FanoutConfig fc;
    fc.socketPath = QString::fromStdString(socket);
    capture::CaptureFanout fanout(fc, products, registry, [](const auto&) {});
    std::vector<std::unique_ptr<capture::Writer>> writers;
    for (size_t i = 0; i < products.size(); ++i) {
        capture::WriterConfig wc;
        wc.root = QString::fromStdString(root.string());
        wc.symbol = products[i];
        wc.fsyncBlocks = 1;
        wc.onJournal = [&fanout, i](const capture::JournalEvent& e) { fanout.publish(i, e); };
        writers.push_back(std::make_unique<capture::Writer>(wc, json{{"product_metadata", tails[i].metadata}}));
        writers[i]->append(frame(t0, snapshot(products[i], tails[i].lastPrice)));
        writers[i]->append(frame(t0, "{\"channel\":\"heartbeats\"}"));
        writers[i]->flush();
    }
    const int64_t wallStart = wallMs();
    writeJson(dir / "capture.json", {{"t0Ms", t0}, {"wallStartMs", wallStart}, {"socket", socket}});
    std::cout << "capture t0Ms=" << t0 << " products=" << products.size() << std::endl;
    int64_t k = 1, records = 0;
    const int64_t last = int64_t(seconds) * 10;
    int64_t flushedSecond = 0;
    while (k <= last) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        const int64_t due = (wallMs() - wallStart) / 100;
        for (; k <= std::min(due, last); ++k) {
            const int64_t ms = t0 + k * 100;
            for (size_t i = 0; i < products.size(); ++i) {
                const double price = tails[i].lastPrice * (1 + 0.0001 * double(k % 7));
                writers[i]->append(frame(ms, trade(products[i], "bench-" + std::to_string(t0) + "-" + std::to_string(k), ms, price)));
                if (k % 10 == 0) writers[i]->append(frame(ms, "{\"channel\":\"heartbeats\"}"));
                ++records;
            }
        }
        if (k / 10 > flushedSecond) {
            flushedSecond = k / 10;
            for (auto& w : writers) w->flush();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    for (auto& w : writers) w->close();
    writeJson(dir / "capture.done", {{"records", records}, {"lastMs", t0 + last * 100}});
    std::cout << "capture done records=" << records << " lastMs=" << t0 + last * 100 << std::endl;
    const auto until = Clock::now() + std::chrono::seconds(180);
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
// Time of the first line containing every part, or -1.
double at(std::initializer_list<std::string> parts) {
    std::lock_guard lock(mutex);
    for (const auto& [ms, line] : lines) {
        bool all = true;
        for (const auto& p : parts) all = all && line.find(p) != std::string::npos;
        if (all) return ms;
    }
    return -1;
}
std::string line(std::initializer_list<std::string> parts) {
    std::lock_guard lock(mutex);
    for (const auto& [ms, l] : lines) {
        bool all = true;
        for (const auto& p : parts) all = all && l.find(p) != std::string::npos;
        if (all) return l;
    }
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
        std::ifstream in(dir / "roller.json");
        json j;
        in >> j;
        return j.value("committedThroughMs", int64_t{0});
    } catch (const std::exception&) {
        return 0;
    }
}
std::string digest(const fs::path& root) {
    std::vector<fs::path> files;
    // The recorder's output only: the hourly checker's oracle depends on when it ran.
    for (const auto& e : fs::recursive_directory_iterator(root))
        if (e.path().extension() == ".hmc2" &&
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
    return hash.result().toHex().left(16).toStdString();
}

int runServer(const fs::path& dir, bool restore, int seconds, const std::vector<std::string>& products) {
    refuseLiveRoot(dir / "raw-l2");
    refuseLiveRoot(dir / "hmc2");
    fs::create_directories(dir / "hmc2");
    fs::create_directories(dir / "primary");
    const auto until = Clock::now() + std::chrono::seconds(60);
    while (!fs::exists(dir / "capture.json") && Clock::now() < until)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    json clock;
    {
        std::ifstream in(dir / "capture.json");
        in >> clock;
    }
    const int64_t t0 = clock.at("t0Ms"), wallStart = clock.at("wallStartMs");
    const double startMs = sinceStartMs();
    QDir::setCurrent(QString::fromStdString(dir.string()));
    qInstallMessageHandler(logs::handler);
    ServerConfig config;
    config.defaultSymbols = products;
    config.streamPort = 0;
    config.recording.enabled = true;
    config.recording.source = "roller";
    config.recording.dir = (dir / "primary").string();
    config.recording.fallbackDir.clear();
    config.chart.timeframesMs = {100, 1000, 60000, 300000, 900000, 3600000, 14400000, 86400000};
    config.chart.historyBars = {{100, 18000}};
    auto& shadow = config.rollerShadow;
    shadow.enabled = true;
    shadow.journalRoot = (dir / "raw-l2").string();
    shadow.outputRoot = (dir / "hmc2").string();
    shadow.socketPath = clock.at("socket").get<std::string>();
    shadow.from = iso(t0 / 86'400'000 * 86'400'000).substr(0, 19) + "Z";
    shadow.products = products;
    shadow.liveNowForTest = [t0, wallStart] { return t0 + (wallMs() - wallStart); };
    shadow.candleRestoreForTest = restore;
    auto app = std::make_unique<SentinelServerApp>(config);
    ShadowServerTestAccess::prepare(*app, dir);
    auto& model = ShadowServerTestAccess::model(*app);
    std::map<std::string, int> readyAtFirstTrade; // -1 not seen, 0 false, 1 true
    for (const auto& p : products) readyAtFirstTrade[p] = -1;
    QObject::connect(&model, &ServerDataModel::tradeBroadcast, [&](const Trade& t) {
        auto& seen = readyAtFirstTrade[t.product_id];
        if (seen < 0) seen = model.candleHistoryReady(t.product_id) ? 1 : 0;
    });
    const auto lateBefore = metric(ShadowServerTestAccess::metrics(*app), "sentinel_server_event_loop_late_ticks_total");
    if (!ShadowServerTestAccess::start(*app, products)) {
        std::cerr << "server start failed" << std::endl;
        return 2;
    }
    double lagMax = 0, p99After60 = 0;
    std::vector<std::pair<double, double>> samples;
    auto nextSample = Clock::now() + std::chrono::seconds(1);
    const auto measuredUntil = Clock::now() + std::chrono::seconds(seconds);
    std::map<std::string, int64_t> stable;
    auto stableSince = Clock::now();
    const auto hardStop = Clock::now() + std::chrono::seconds(seconds + 90);
    for (;;) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        const auto now = Clock::now();
        if (now >= nextSample) {
            nextSample += std::chrono::seconds(1);
            const double p99 = ShadowServerTestAccess::lag(*app, 0.99), max = ShadowServerTestAccess::lag(*app, 1);
            const double t = sinceStartMs() - startMs;
            samples.emplace_back(p99, max);
            if (now <= measuredUntil) {
                lagMax = std::max(lagMax, max);
                if (t >= 60'000) p99After60 = std::max(p99After60, p99);
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
        } else if (now - stableSince > std::chrono::seconds(5)) {
            break;
        }
    }
    const auto text = ShadowServerTestAccess::metrics(*app);
    const auto lateAfter = metric(text, "sentinel_server_event_loop_late_ticks_total");
    const double servingReady = logs::at({"Roller serving ready products="});
    std::vector<std::string> out;
    for (const auto& p : products) {
        const auto restored = logs::line({"Candle history restored product=" + p + " "});
        const auto ready = logs::line({"Candle history ready symbol=" + p + " "});
        std::ostringstream s;
        s << "product=" << p << " mode=" << (restore ? "restore" : "baseline")
          << " startedMs=" << logs::at({"Roller started product=" + p + " "})
          << " restoreStartMs=" << logs::at({"Candle history restore start product=" + p + " "})
          << " restoredMs=" << logs::at({"Candle history restored product=" + p + " "})
          << " readyMs=" << logs::at({"Candle history ready symbol=" + p + " "})
          << " seedMs=" << logs::at({"Journal live seed product=" + p + " "})
          << " healthyMs=" << logs::at({"Roller writer healthy product=" + p})
          << " outcome=" << logs::field(restored, "outcome") << " files=" << logs::field(restored, "files")
          << " records=" << logs::field(restored, "records") << " trades=" << logs::field(restored, "trades")
          << " bars100=" << logs::field(restored, "bars100") << " bars1000=" << logs::field(restored, "bars1000")
          << " readMs=" << logs::field(restored, "readMs") << " installUs=" << logs::field(ready, "installUs")
          << " restored=" << logs::field(ready, "restored") << " readyAtFirstTrade=" << readyAtFirstTrade[p]
          << " committedMs=" << stable[p];
        out.push_back(s.str());
    }
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    const double failures = [&] {
        double sum = 0;
        for (const auto& p : products) {
            sum += std::max(0.0, metric(text, "sentinel_candle_history_restore_failures_total{product=\"" + p + "\"}"));
        }
        return sum;
    }();
    app.reset();
    writeJson(dir / "server.done", {{"ok", true}});
    {
        std::ofstream log(dir / "server.log");
        std::lock_guard lock(logs::mutex);
        for (const auto& [ms, l] : logs::lines) log << int64_t(ms) << " " << l << "\n";
    }
    for (const auto& l : out) std::cout << l << std::endl;
    std::cout << "total mode=" << (restore ? "restore" : "baseline") << " products=" << products.size()
              << " servingReadyMs=" << servingReady << " lagMaxMs=" << lagMax << " lagP99After60sMs=" << p99After60
              << " lateTicks=" << (lateAfter - std::max(0.0, lateBefore)) << " maxRssMb="
              << double(usage.ru_maxrss) / (1024.0 * 1024.0) << " restoreFailures=" << failures
              << " hmc2=" << digest(dir / "hmc2") << std::endl;
    std::cout << "lag per second (p99,max):";
    for (const auto& [p99, max] : samples) std::cout << " " << p99 << "," << max;
    std::cout << std::endl;
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        const std::vector<std::string> args(argv + 1, argv + argc);
        if (args.size() >= 4 && args[0] == "capture")
            return runCapture(args[1], std::stoi(args[2]), {args.begin() + 3, args.end()});
        if (args.size() >= 5 && args[0] == "server" && (args[2] == "restore" || args[2] == "baseline"))
            return runServer(args[1], args[2] == "restore", std::stoi(args[3]), {args.begin() + 4, args.end()});
    } catch (const std::exception& e) {
        std::cerr << "candle_startup_bench: " << e.what() << std::endl;
        return 1;
    }
    std::cerr << "usage: candle_startup_bench capture <runDir> <seconds> <product>...\n"
                 "       candle_startup_bench server <runDir> <restore|baseline> <seconds> <product>..."
              << std::endl;
    return 1;
}
