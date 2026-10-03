#include "CaptureFanout.hpp"
#include "SentinelLogging.hpp"
#include "metrics/MetricsRegistry.hpp"
#include <QDir>
#include <QFileInfo>
#include <QLockFile>
#include <algorithm>
#include <deque>
#include <filesystem>
#include <thread>
#include <cstring>
#include <cerrno>
#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace sentinel::capture {
using Json = nlohmann::json;
Json positionJson(const JournalPosition& p) {
    return {{"product", p.product}, {"run_id", p.runId}, {"block", p.block}, {"record", p.record}};
}
JournalPosition parsePosition(const Json& j) {
    if (!j.at("block").is_number_unsigned() || !j.at("record").is_number_unsigned() ||
        j.at("record").get<uint64_t>() > UINT32_MAX) throw std::runtime_error("invalid position");
    JournalPosition p{j.at("product").get<std::string>(), j.at("run_id").get<std::string>(),
        j.at("block").get<uint64_t>(), j.at("record").get<uint32_t>()};
    validateSymbol(p.product);
    if (p.runId.empty() || p.runId.size() > 64) throw std::runtime_error("invalid run_id");
    return p;
}
namespace {
int64_t nowNs() { return std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count(); }
void put32(std::string& s, uint32_t n) { for (int i = 0; i < 4; ++i) s += char(n >> (i * 8)); }
uint32_t get32(const char* s) {
    uint32_t n = 0; for (int i = 0; i < 4; ++i) n |= uint32_t(static_cast<unsigned char>(s[i])) << (i * 8); return n;
}
std::string packet(const Json& header, std::string_view raw = {}) {
    const auto json = header.dump();
    std::string out; out.reserve(8 + json.size() + raw.size());
    put32(out, uint32_t(4 + json.size() + raw.size())); put32(out, uint32_t(json.size()));
    out += json; out.append(raw); return out;
}
}
#ifdef Q_OS_UNIX
QString prepareFanoutPath(const QString& supplied) {
    namespace fs = std::filesystem;
    const auto value = supplied.isEmpty() ? QDir::homePath() + "/Sentinel-runtime/run/capture.sock" : supplied;
    fs::path input(value.toStdString());
    if (!input.is_absolute() || input.filename().empty() || input.filename() == "." || input.filename() == "..")
        throw std::runtime_error("fanout socket must be an absolute file path");
    const auto normalized = fs::weakly_canonical(input);
    auto safe = [](fs::path p) {
        for (; !p.empty(); p = p.parent_path()) {
            if (p == "/Volumes" || fs::exists(p / ".git")) throw std::runtime_error("fanout socket refuses volumes and repositories");
            if (p == p.root_path()) break;
        }
    };
    safe(input.lexically_normal()); safe(normalized);
    // Do not follow the final socket or an explicitly symlinked private directory.
    if (fs::is_symlink(fs::symlink_status(input)) || fs::is_symlink(fs::symlink_status(input.parent_path())))
        throw std::runtime_error("fanout socket/parent must not be a symlink");
    const auto parent = normalized.parent_path();
    std::vector<fs::path> missing;
    for (auto p = parent; !fs::exists(p); p = p.parent_path()) missing.push_back(p);
    for (auto i = missing.rbegin(); i != missing.rend(); ++i)
        if (::mkdir(i->c_str(), 0700) && errno != EEXIST) throw std::runtime_error("cannot create fanout directory");
    struct stat st{};
    if (::lstat(parent.c_str(), &st) || !S_ISDIR(st.st_mode) || st.st_uid != ::geteuid() || (st.st_mode & 0777) != 0700)
        throw std::runtime_error("fanout directory must be owned by this user with mode 0700");
    if (normalized.string().size() >= sizeof(sockaddr_un::sun_path)) throw std::runtime_error("fanout socket path too long");
    return QString::fromStdString(normalized.string());
}
namespace {
void nonblock(int fd) {
    if (::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK) < 0 || ::fcntl(fd, F_SETFD, FD_CLOEXEC) < 0)
        throw std::runtime_error("fanout nonblocking fd failed");
#ifdef SO_NOSIGPIPE
    int yes = 1; ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
#endif
}
ssize_t sendBytes(int fd, const char* p, size_t n) {
#ifdef MSG_NOSIGNAL
    return ::send(fd, p, n, MSG_NOSIGNAL);
#else
    return ::send(fd, p, n, 0);
#endif
}
}
struct CaptureFanout::Impl {
    struct Batch { std::string run, raw; uint64_t block, epoch; uint32_t record; JournalEventKind kind; bool hasPosition; size_t cost() const { return raw.size() + run.size() + sizeof(Batch); } };
    struct Entry { JournalPosition pos; std::shared_ptr<const std::string> wire; int64_t time; size_t cost() const { return wire->size() + sizeof(Entry) + pos.product.size() + pos.runId.size(); } };
    struct Product {
        std::string name;
        // Single producer/consumer fixed slots. Payload allocated only at publish.
        std::array<std::unique_ptr<Batch>, 1024> pending;
        std::atomic<uint64_t> head{0}, tail{0}, epoch{0};
        std::shared_ptr<std::atomic<size_t>> pendingBytes = std::make_shared<std::atomic<size_t>>(0);
        std::deque<Entry> ring;
        size_t ringBytes = 0;
        uint64_t seenEpoch = 0;
        std::optional<JournalPosition> tip, durable;
        std::optional<int64_t> lastResnapshot;
        bool malformedLogged = false;
        metrics::Gauge *ringGauge = nullptr, *oldest = nullptr;
        metrics::Counter *hits = nullptr, *misses = nullptr, *drops = nullptr;
    };
    struct Client {
        int fd = -1;
        int product = -1;
        std::string input;
        std::deque<std::shared_ptr<const std::string>> queue;
        size_t bytes = 0, offset = 0;
        int64_t accepted = 0;
        metrics::Gauge* gauge = nullptr;
    };
    FanoutConfig config;
    QString socketPath, boundPath;
    std::unique_ptr<QLockFile> lock;
    std::vector<std::unique_ptr<Product>> products;
    std::array<Client, MaxClients> clients;
    std::function<void(const std::string&)> resnapshot;
    std::atomic<bool> stopping{false};
    int listener = -1, wake[2]{-1,-1};
    // Published once when pipe creation succeeds; closed only after writers stop.
    std::atomic<int> wakeWriter{-1};
    int64_t nextRetry = 0, retryMs = 30000;
    std::deque<int64_t> globalResnapshots;
    bool bound = false;
    std::thread thread;
    metrics::Gauge *connected = nullptr, *running = nullptr;
    metrics::Counter* setupFailures = nullptr;
    std::map<std::string, metrics::Counter*, std::less<>> disconnects;
    Impl(FanoutConfig c, const std::vector<std::string>& names, metrics::MetricsRegistry& r,
         std::function<void(const std::string&)> callback) : config(std::move(c)), resnapshot(std::move(callback)) {
        if (!config.nowNs) config.nowNs = nowNs;
        if (names.empty() || names.size() > MaxProducts || !config.ringBytes || !config.clientBytes || !config.ingressBytes ||
            config.retention.count() <= 0 || config.resnapshotInterval.count() < 20000) throw std::runtime_error("invalid fanout limits");
        running = &r.gauge("sentinel_fanout_running", "1 while the fanout worker is serving.");
        setupFailures = &r.counter("sentinel_fanout_setup_failures_total", "Fanout setup/service failures; journal capture continues.");
        connected = &r.gauge("sentinel_fanout_clients", "Connected local fanout clients.");
        for (const auto* reason : {"slow_client", "ingress_overflow", "peer_closed", "protocol", "shutdown", "capacity", "internal_error", "handshake_timeout", "malformed_ingress"})
            disconnects[reason] = &r.counter("sentinel_fanout_disconnects_total", "Fanout disconnects by reason.", {{"reason", reason}});
        for (size_t i = 0; i < clients.size(); ++i)
            clients[i].gauge = &r.gauge("sentinel_fanout_queue_bytes", "Pending wire bytes including a partially sent record.", {{"client", std::to_string(i)}});
        for (const auto& name : names) {
            validateSymbol(name);
            if (std::any_of(products.begin(), products.end(), [&](const auto& p) { return p->name == name; })) throw std::runtime_error("duplicate fanout product");
            auto p = std::make_unique<Product>(); p->name = name;
            const metrics::Labels label{{"product", name}};
            p->ringGauge = &r.gauge("sentinel_fanout_ring_bytes", "Ring bytes including entry accounting.", label);
            p->oldest = &r.gauge("sentinel_fanout_ring_oldest_age_seconds", "Oldest retained publication age.", label);
            p->drops = &r.counter("sentinel_fanout_ingress_drops_total", "Journal events refused by bounded fanout ingress; journal unaffected.", label);
            p->hits = &r.counter("sentinel_fanout_resume_hits_total", "Resume cursors found in ring.", label);
            p->misses = &r.counter("sentinel_fanout_resume_misses_total", "Resume cursors requiring journal catch-up.", label);
            r.gaugeFn("sentinel_fanout_ingress_bytes", "Writer-to-fanout pending bytes, outside QueuePool.", label,
                [bytes = p->pendingBytes]() -> std::optional<double> { return double(bytes->load(std::memory_order_relaxed)); });
            products.push_back(std::move(p));
        }
        socketPath = config.socketPath.isEmpty() ? QDir::homePath() + "/Sentinel-runtime/run/capture.sock" : config.socketPath;
        try { setup(); } catch (const std::exception& e) { unavailable(e); }
        try { thread = std::thread([this] { run(); }); } catch (...) { cleanup(); throw; }
    }
    void setup() {
        if (wake[0] < 0) {
            int pipeFds[2];
            if (::pipe(pipeFds)) throw std::runtime_error("fanout wake pipe failed");
            try { nonblock(pipeFds[0]); nonblock(pipeFds[1]); }
            catch (...) { ::close(pipeFds[0]); ::close(pipeFds[1]); throw; }
            wake[0] = pipeFds[0]; wake[1] = pipeFds[1];
            wakeWriter.store(wake[1], std::memory_order_release);
        }
        const auto checkedPath = prepareFanoutPath(socketPath);
        lock = std::make_unique<QLockFile>(checkedPath + ".lock"); lock->setStaleLockTime(0);
        if (!lock->tryLock()) throw std::runtime_error("fanout socket already owned");
        const auto path = checkedPath.toStdString();
        struct stat st{};
        if (!::lstat(path.c_str(), &st)) {
            if (!S_ISSOCK(st.st_mode) || st.st_uid != ::geteuid()) throw std::runtime_error("refusing non-socket fanout path");
            // Also refuse a live listener that does not use our lock protocol.
            int probe = ::socket(AF_UNIX, SOCK_STREAM, 0);
            if (probe < 0) throw std::runtime_error("cannot probe existing fanout socket");
            sockaddr_un old{}; old.sun_family = AF_UNIX; std::strcpy(old.sun_path, path.c_str());
            try { nonblock(probe); } catch (...) { ::close(probe); throw; }
            int result = ::connect(probe, reinterpret_cast<sockaddr*>(&old), sizeof(old));
            int error = errno; ::close(probe);
            if (result == 0 || error != ECONNREFUSED) throw std::runtime_error("refusing active or inaccessible fanout socket");
            // Lock ownership is mandatory even for recovery of a stale socket.
            if (::unlink(path.c_str())) throw std::runtime_error("cannot remove stale fanout socket");
        }
        listener = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (listener < 0) throw std::runtime_error("fanout socket failed");
        nonblock(listener);
        sockaddr_un address{}; address.sun_family = AF_UNIX; std::strcpy(address.sun_path, path.c_str());
        if (::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address))) throw std::runtime_error("fanout socket bind failed");
        boundPath = checkedPath; bound = true;
        if (::chmod(path.c_str(), 0600) || ::listen(listener, MaxClients)) throw std::runtime_error("fanout listen failed");
        running->set(1); retryMs = 30000;
        sLog_App("Capture fanout listening: path=" << checkedPath);
    }
    void cleanupSocket() {
        if (listener >= 0) ::close(listener);
        listener = -1;
        if (bound) { ::unlink(boundPath.toStdString().c_str()); bound = false; }
        if (lock && lock->isLocked()) lock->unlock();
        lock.reset();
    }
    void unavailable(const std::exception& e) {
        running->set(0);
        cleanupSocket();
        nextRetry = config.nowNs() + retryMs * 1000000;
        sLog_Error("Capture fanout unavailable; journal continues: error=" << e.what() << " retry_ms=" << retryMs);
        retryMs = std::min<int64_t>(retryMs * 2, 600000);
        setupFailures->inc();
    }
    void cleanup() {
        cleanupSocket();
        for (int fd : wake) if (fd >= 0) ::close(fd);
        wake[0] = wake[1] = -1; wakeWriter.store(-1);
    }
    ~Impl() { stop(); cleanup(); }
    void signal() noexcept {
        const int fd = wakeWriter.load(std::memory_order_acquire);
        if (fd >= 0) { const char b = 0; const auto ignored = ::write(fd, &b, 1); (void)ignored; }
    }
    void stop() { if (!stopping.exchange(true)) signal(); if (thread.joinable()) thread.join(); }
    void drop(Client& c, std::string_view reason) noexcept {
        if (c.fd < 0) return;
        // Best effort only: a saturated socket cannot guarantee delivery of an
        // extra message. EOF ALWAYS means journal resume; metrics retain reason.
        // Never insert a control frame into a partially sent record.
        try { if (!c.offset) {
            auto why = packet({{"type", "disconnect"}, {"reason", reason}, {"resume", "journal"}});
            sendBytes(c.fd, why.data(), why.size());
        } } catch (...) {} // disconnect must remain possible under memory pressure
        ::close(c.fd); c.fd = -1; c.product = -1; c.input.clear(); c.queue.clear(); c.bytes = c.offset = 0;
        c.gauge->set(0); connected->add(-1); disconnects.find(reason)->second->inc();
    }
    bool enqueue(Client& c, std::shared_ptr<const std::string> data) {
        if (c.fd < 0) return false;
        const auto cost = data->size() + sizeof(std::shared_ptr<const std::string>);
        if (cost > config.clientBytes || c.bytes > config.clientBytes - cost) { drop(c, "slow_client"); return false; }
        c.bytes += cost; c.queue.push_back(std::move(data)); c.gauge->set(double(c.bytes)); return true;
    }
    bool control(Client& c, const Json& j) { return enqueue(c, std::make_shared<const std::string>(packet(j))); }
    void prune(Product& p, int64_t now) {
        while (!p.ring.empty() && (p.ringBytes > config.ringBytes || now - p.ring.front().time >= config.retention.count() * 1000000)) {
            p.ringBytes -= p.ring.front().cost(); p.ring.pop_front();
        }
        p.ringGauge->set(double(p.ringBytes));
        p.oldest->set(p.ring.empty() ? 0 : std::max(0.0, double(now - p.ring.front().time) / 1e9));
    }
    void invalidate(size_t index, Product& p, std::string_view reason = "ingress_overflow") {
        p.ring.clear(); p.ringBytes = 0;
        for (auto& c : clients) if (c.product == int(index)) drop(c, reason);
    }
    void drain() {
        const auto now = config.nowNs();
        for (size_t i = 0; i < products.size(); ++i) {
            auto& p = *products[i];
            const auto epoch = p.epoch.load(std::memory_order_acquire);
            if (p.seenEpoch != epoch) { invalidate(i, p); p.seenEpoch = epoch; }
            auto tail = p.tail.load(std::memory_order_relaxed);
            // Snapshot head: a hot producer cannot monopolize the socket thread.
            const auto head = p.head.load(std::memory_order_acquire);
            while (tail != head) {
                auto batch = std::move(p.pending[tail % p.pending.size()]);
                p.pendingBytes->fetch_sub(batch->cost(), std::memory_order_relaxed);
                p.tail.store(++tail, std::memory_order_release);
                if (batch->epoch != p.seenEpoch) continue;
                JournalPosition pos{p.name, batch->run, batch->block, batch->record};
                if (batch->kind == JournalEventKind::Record) {
                    if (batch->raw.size() < 32 || size_t(get32(batch->raw.data())) + 4 != batch->raw.size()) {
                        invalidate(i, p, "malformed_ingress");
                        p.tip.reset(); p.durable.reset();
                        if (!p.malformedLogged) {
                            sLog_Error("Capture fanout malformed ingress: product=" << p.name << "; journal resume required");
                            p.malformedLogged = true;
                        }
                        continue;
                    }
                    auto wire = std::make_shared<const std::string>(packet(
                        {{"type", "record"}, {"pos", positionJson(pos)}, {"provisional", true}}, batch->raw));
                    Entry e{pos, wire, now}; p.tip = pos;
                    p.ringBytes += e.cost(); p.ring.push_back(std::move(e)); prune(p, now);
                    for (auto& c : clients) if (c.product == int(i)) enqueue(c, wire);
                } else {
                    Json message;
                    if (batch->kind == JournalEventKind::Durable) {
                        p.durable = pos;
                        message = {{"type", "durable"}, {"product", p.name}, {"through", positionJson(pos)}};
                    } else {
                        p.durable = batch->hasPosition ? std::optional(pos) : std::nullopt;
                        // Remove the suffix permanently: a reconnect must never replay
                        // records withdrawn by a failed flush/abandoned segment.
                        while (!p.ring.empty() && (!p.durable || p.ring.back().pos.runId != p.durable->runId ||
                            std::pair{p.ring.back().pos.block, p.ring.back().pos.record} > std::pair{p.durable->block, p.durable->record})) {
                            p.ringBytes -= p.ring.back().cost(); p.ring.pop_back();
                        }
                        p.tip = p.durable;
                        message = {{"type", "retract"}, {"product", p.name},
                            {"after", p.durable ? positionJson(*p.durable) : Json(nullptr)}};
                    }
                    auto wire = std::make_shared<const std::string>(packet(message));
                    for (auto& c : clients) if (c.product == int(i)) enqueue(c, wire);
                }
            }
            prune(p, now);
        }
    }
    void command(Client& c, const Json& j) {
        const auto type = j.at("type").get<std::string>();
        const auto product = j.at("product").get<std::string>();
        auto it = std::find_if(products.begin(), products.end(), [&](const auto& p) { return p->name == product; });
        if (it == products.end()) throw std::runtime_error("unknown product");
        auto& p = **it; const int index = int(it - products.begin());
        if (type == "hello" || type == "resume") {
            if (c.product >= 0) throw std::runtime_error("already subscribed");
            if (j.value("version", 0) != 1) throw std::runtime_error("unsupported fanout version");
            const auto now = config.nowNs(); prune(p, now);
            std::optional<JournalPosition> resume;
            if (j.contains("pos") && !j.at("pos").is_null()) { resume = parsePosition(j.at("pos")); if (resume->product != product) throw std::runtime_error("resume product mismatch"); }
            auto begin = p.ring.end();
            if (resume) {
                auto found = std::find_if(p.ring.begin(), p.ring.end(), [&](const Entry& e) { return e.pos == *resume; });
                if (found != p.ring.end()) { begin = std::next(found); p.hits->inc(); }
                else {
                    p.misses->inc(); begin = p.ring.begin();
                    if (!control(c, {{"type", "gap"}, {"reason", "resume_not_retained"}, {"resume_after", positionJson(*resume)},
                        {"journal_until", p.ring.empty() ? Json(nullptr) : positionJson(p.ring.front().pos)},
                        {"until_inclusive", false}})) return;
                }
            } else begin = p.ring.begin();
            if (!control(c, {{"type", "tip"}, {"version", 1}, {"product", product}, {"pos", p.tip ? positionJson(*p.tip) : Json(nullptr)},
                {"durable", p.durable ? positionJson(*p.durable) : Json(nullptr)}})) return;
            for (auto entry = begin; entry != p.ring.end(); ++entry) if (!enqueue(c, entry->wire)) return;
            c.product = index; // same worker: replay -> live is atomic
        } else if (type == "resnapshot") {
            if (c.product != index) throw std::runtime_error("resnapshot requires subscription");
            if (config.controlReady && !config.controlReady()) {
                control(c, {{"type", "resnapshot"}, {"product", product}, {"status", "unavailable"}}); return;
            }
            const auto now = config.nowNs();
            const bool limited = p.lastResnapshot && now - *p.lastResnapshot < config.resnapshotInterval.count() * 1000000;
            while (!globalResnapshots.empty() && now - globalResnapshots.front() >= 60'000'000'000LL) globalResnapshots.pop_front();
            const bool globalLimited = globalResnapshots.size() >= 3;
            if (!limited && !globalLimited) {
                p.lastResnapshot = now; globalResnapshots.push_back(now);
                if (resnapshot) resnapshot(product);
            }
            control(c, {{"type", "resnapshot"}, {"product", product},
                {"status", limited ? "rate_limited" : globalLimited ? "global_rate_limited" : "forwarded"}});
        } else throw std::runtime_error("unknown command");
    }
    void read(Client& c) {
        // One bounded read per turn: malformed/flooding clients cannot monopolize.
        char buffer[4096]; const auto n = ::recv(c.fd, buffer, sizeof(buffer), 0);
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) { drop(c, "peer_closed"); return; }
        if (n <= 0) return;
        c.input.append(buffer, size_t(n));
        if (c.input.size() > 8192) { drop(c, "protocol"); return; }
        try {
            size_t newline;
            while (c.fd >= 0 && (newline = c.input.find('\n')) != std::string::npos) {
                auto line = c.input.substr(0, newline); c.input.erase(0, newline + 1);
                command(c, Json::parse(line));
            }
        } catch (...) { drop(c, "protocol"); }
    }
    void write(Client& c) {
        size_t budget = 256 * 1024;
        while (c.fd >= 0 && !c.queue.empty() && budget) {
            const auto& s = *c.queue.front();
            const auto n = sendBytes(c.fd, s.data() + c.offset, std::min(budget, s.size() - c.offset));
            if (n <= 0) {
                if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) drop(c, "peer_closed");
                return;
            }
            c.offset += size_t(n); budget -= size_t(n);
            if (c.offset == s.size()) {
                c.bytes -= s.size() + sizeof(std::shared_ptr<const std::string>); c.offset = 0; c.queue.pop_front(); c.gauge->set(double(c.bytes));
            }
        }
    }
    void accept() {
        int fd = ::accept(listener, nullptr, nullptr);
        if (fd < 0) return;
        try { nonblock(fd); } catch (...) { ::close(fd); return; }
        int size = 64 * 1024; ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
        auto it = std::find_if(clients.begin(), clients.end(), [](const Client& c) { return c.fd < 0; });
        if (it == clients.end()) { auto why = packet({{"type", "disconnect"}, {"reason", "capacity"}}); sendBytes(fd, why.data(), why.size()); ::close(fd); disconnects.at("capacity")->inc(); return; }
        it->fd = fd; it->accepted = config.nowNs(); connected->add(1);
    }
    void run() noexcept {
        sentinel::logging::setCurrentThreadName("capture-fanout");
        while (!stopping.load(std::memory_order_acquire)) {
            try {
                if (listener < 0 && config.nowNs() >= nextRetry) setup();
                drain();
                std::array<pollfd, MaxClients + 2> fds{};
                fds[0] = {listener, POLLIN, 0}; fds[1] = {wake[0], POLLIN, 0};
                for (size_t i = 0; i < clients.size(); ++i) {
                    auto& c = clients[i];
                    if (c.fd >= 0 && c.product < 0 && config.nowNs() - c.accepted >= 5'000'000'000LL) drop(c, "handshake_timeout");
                    fds[i + 2] = {c.fd, short(POLLIN | (c.queue.empty() ? 0 : POLLOUT)), 0};
                }
                if (::poll(fds.data(), fds.size(), 1000) < 0 && errno != EINTR) throw std::runtime_error("fanout poll failed");
                if (fds[1].revents & POLLIN) { char bytes[1024]; while (::read(wake[0], bytes, sizeof(bytes)) > 0) {} }
                // Drain before handshakes so resume sees the latest published prefix.
                drain();
                for (size_t i = 0; i < clients.size(); ++i) {
                    auto& c = clients[i]; if (c.fd < 0 || c.fd != fds[i + 2].fd) continue;
                    auto flags = fds[i + 2].revents;
                    if (flags & POLLIN) read(c);
                    if (c.fd >= 0 && flags & POLLOUT) write(c);
                    if (c.fd >= 0 && flags & (POLLHUP | POLLERR | POLLNVAL)) drop(c, "peer_closed");
                }
                if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) throw std::runtime_error("fanout listener failed");
                if (fds[0].revents & POLLIN) accept();
            }
            catch (const std::exception& e) {
                for (size_t i = 0; i < products.size(); ++i) invalidate(i, *products[i], "internal_error");
                for (auto& c : clients) drop(c, "internal_error");
                unavailable(e);
            }
        }
        try { drain(); } catch (const std::exception& e) { sLog_Error("Capture fanout shutdown drain: " << e.what()); }
        for (auto& c : clients) { if (c.fd >= 0) write(c); drop(c, "shutdown"); }
        running->set(0);
    }
};
CaptureFanout::CaptureFanout(FanoutConfig c, const std::vector<std::string>& products, metrics::MetricsRegistry& registry,
                           std::function<void(const std::string&)> callback)
    : m(std::make_unique<Impl>(std::move(c), products, registry, std::move(callback))) {}
CaptureFanout::~CaptureFanout() = default;
void CaptureFanout::publish(size_t index, const JournalEvent& event) noexcept {
    const auto run = event.runId, raw = event.bytes;
    if (m->stopping.load(std::memory_order_acquire) || index >= m->products.size()) return;
    auto& p = *m->products[index];
    const auto head = p.head.load(std::memory_order_relaxed);
    const auto cost = raw.size() + run.size() + sizeof(Impl::Batch);
    try {
        if (cost > m->config.ingressBytes || p.pendingBytes->load(std::memory_order_relaxed) > m->config.ingressBytes - cost ||
            head - p.tail.load(std::memory_order_acquire) == p.pending.size()) throw std::runtime_error("fanout ingress full");
        auto batch = std::make_unique<Impl::Batch>(Impl::Batch{std::string(run), std::string(raw), event.block, p.epoch.load(std::memory_order_relaxed), event.record, event.kind, event.hasPosition});
        p.pendingBytes->fetch_add(cost, std::memory_order_relaxed);
        p.pending[head % p.pending.size()] = std::move(batch);
        p.head.store(head + 1, std::memory_order_release);
    } catch (...) { p.drops->inc(); p.epoch.fetch_add(1, std::memory_order_release); }
    m->signal();
}
void CaptureFanout::stop() { m->stop(); }
QString CaptureFanout::path() const { return m->socketPath; }
#else
QString prepareFanoutPath(const QString&) { throw std::runtime_error("capture fanout requires Unix domain sockets"); }
struct CaptureFanout::Impl {};
CaptureFanout::CaptureFanout(FanoutConfig, const std::vector<std::string>&, metrics::MetricsRegistry&, std::function<void(const std::string&)>) { throw std::runtime_error("capture fanout requires Unix"); }
CaptureFanout::~CaptureFanout() = default;
void CaptureFanout::publish(size_t, const JournalEvent&) noexcept {}
void CaptureFanout::stop() {}
QString CaptureFanout::path() const { return {}; }
#endif
} // namespace sentinel::capture
