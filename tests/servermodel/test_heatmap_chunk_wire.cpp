// Loopback TLS tests for the S3 heatmap chunk wire: a real SentinelStreamServer
// and a real SentinelStreamClient over 127.0.0.1. The private Session is
// compiled here (as in test_recording_server_stop.cpp) so tests can reach its
// budget counters and write queue.
#include "protocol/SentinelStreamServer.cpp"
#include "protocol/SentinelStreamClient.hpp"
#include "marketdata/auth/Authenticator.hpp"
#include "heatmap/ChunkCodec.hpp"
#include "servermodel/ChunkService.hpp"
#include "servermodel/Hmc2Store.hpp"
#include "servermodel/RecordingChunks.hpp"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <gtest/gtest.h>
#include <openssl/pem.h>
#include <condition_variable>
#include <future>
#include <random>

using namespace std::chrono_literals;
using heatmap::ChunkKey;
using heatmap::ChunkKind;
using recording::Hmc2Record;

struct HeatmapChunkWireTest {
    static void setChunks(SentinelStreamServer& server, std::shared_ptr<recording::ChunkService> chunks) {
        server.m_chunks = std::move(chunks);
    }
    static unsigned short port(SentinelStreamServer& server) {
        std::promise<unsigned short> result;
        net::post(server.m_ioc, [&] { result.set_value(server.m_acceptor->local_endpoint().port()); });
        return result.get_future().get();
    }
    static std::shared_ptr<Session> onlySession(SentinelStreamServer& server) {
        std::lock_guard lock(server.m_sessionsMutex);
        return server.m_sessions.size() == 1 ? *server.m_sessions.begin() : nullptr;
    }
    static size_t sessionCount(SentinelStreamServer& server) {
        std::lock_guard lock(server.m_sessionsMutex);
        return server.m_sessions.size();
    }
    template <class Fn> static auto onExecutor(Session& session, Fn fn) {
        std::promise<decltype(fn(session))> result;
        net::post(session.ws_.get_executor(), [&] {
            if constexpr (std::is_void_v<decltype(fn(session))>) { fn(session); result.set_value(); }
            else result.set_value(fn(session));
        });
        return result.get_future().get();
    }
    static void setChunkBytes(Session& session, size_t bytes) {
        onExecutor(session, [bytes](Session& s) { s.chunkBytes_ = bytes; });
    }
    static size_t chunkJobs(Session& session) {
        return onExecutor(session, [](Session& s) { return s.chunkJobs_; });
    }
    static size_t pendingTasks(SentinelStreamServer& server) { return server.m_pendingHistoryTasks.load(); }
    static recording::EncodedChunkLru& cache(SentinelStreamServer& server) { return server.m_chunks->cache(); }
    static size_t maxChunkBytes() { return Session::kMaxChunkBytes; }
    static void setDecodeBacklog(SentinelStreamClient& client, size_t bytes) { client.m_decodeBacklogBytes = bytes; }
    static size_t decodeBacklogFrames(SentinelStreamClient& client) {
        std::lock_guard lock(client.m_chunkOrderMutex);
        return client.m_decodeBacklogFrames;
    }
    static void decoder(SentinelStreamClient& client,
                        std::function<heatmap::ChunkEnvelope(std::span<const uint8_t>)> decode) {
        client.m_chunkDecoder = std::move(decode);
    }
    static void drainDecoder(SentinelStreamClient& client) {
        std::promise<void> drained;
        net::post(*client.m_decodePool, [&] { drained.set_value(); });
        drained.get_future().get();
    }
    static size_t orderSize(SentinelStreamClient& client) {
        std::lock_guard lock(client.m_chunkOrderMutex);
        return client.m_chunkOrder.size();
    }
    static void serverBinary(Session& session, const std::vector<uint8_t>& bytes) {
        session.do_write(std::string(bytes.begin(), bytes.end()), true);
    }
    static void clientBinary(SentinelStreamClient& client, std::vector<uint8_t> bytes) {
        client.handleBinaryMessage(std::make_shared<std::vector<uint8_t>>(std::move(bytes)));
    }
    static void sendText(SentinelStreamClient& client, std::string payload) {
        net::post(client.m_strand, [&client, payload = std::move(payload)]() mutable {
            client.m_writeQueue.push_back(std::move(payload));
            if (client.m_isConnected && client.m_writeQueue.size() == 1) client.doWrite();
        });
    }
};

namespace {
constexpr int64_t kEpoch = recording::kHmc2MinMs; // 2000-01-01, far in the past
constexpr int64_t kMin = heatmap::kMinuteMs, kHour = heatmap::kHourMs, kDay = heatmap::kDayMs;

Hmc2Record minuteRecord(int i, const std::string& layer) {
    Hmc2Record r;
    const int tick = layer == "near" ? 1 : i < 30 ? 1 : 2;
    r.header = {"BTC-USD", layer, kMin, 100., 100 * tick, {}, uint64_t(tick)};
    r.bucketStartMs = kEpoch + int64_t(i) * kMin;
    r.observedMs = i % 7 ? 60000 : 30000;
    r.flags = r.observedMs < 60000 ? recording::kPartial : 0;
    r.bidRowLo = r.askRowLo = 100 / tick;
    r.bidRowHi = r.askRowHi = 106 / tick - 1;
    r.entries = {{101 / tick, false, recording::encodeSize(2. + i / 100.), 0, r.observedMs},
                 {104 / tick, true, recording::encodeSize(3. + i / 100.), 0, r.observedMs}};
    return r;
}
Hmc2Record hourRecord() {
    auto r = minuteRecord(0, "deep");
    r.header.tfMs = kHour;
    r.bucketStartMs = kEpoch;
    r.observedMs = 3'000'000;
    r.flags = recording::kPartial;
    r.entries = {{101, false, recording::encodeSize(2), 0, 2'000'000},
                 {104, true, recording::encodeSize(3), 0, 3'000'000}};
    r.coverage = {{100, 102, false, 2'000'000}, {103, 105, false, 3'000'000}, {100, 105, true, 3'000'000}};
    return r;
}

bool writeSelfSignedCert(const std::string& certFile, const std::string& keyFile) {
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> ctx(EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr),
                                                                    EVP_PKEY_CTX_free);
    EVP_PKEY* rawKey = nullptr;
    if (!ctx || EVP_PKEY_keygen_init(ctx.get()) <= 0 || EVP_PKEY_CTX_set_rsa_keygen_bits(ctx.get(), 2048) <= 0 ||
        EVP_PKEY_keygen(ctx.get(), &rawKey) <= 0)
        return false;
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(rawKey, EVP_PKEY_free);
    std::unique_ptr<X509, decltype(&X509_free)> cert(X509_new(), X509_free);
    X509_set_version(cert.get(), 2);
    ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), 1);
    X509_gmtime_adj(X509_getm_notBefore(cert.get()), 0);
    X509_gmtime_adj(X509_getm_notAfter(cert.get()), 3600);
    X509_set_pubkey(cert.get(), key.get());
    auto* subject = X509_get_subject_name(cert.get());
    X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("localhost"),
                               -1, -1, 0);
    X509_set_issuer_name(cert.get(), subject);
    if (X509_sign(cert.get(), key.get(), EVP_sha256()) <= 0) return false;
    std::unique_ptr<BIO, decltype(&BIO_free)> pemKey(BIO_new_file(keyFile.c_str(), "w"), BIO_free);
    std::unique_ptr<BIO, decltype(&BIO_free)> pemCert(BIO_new_file(certFile.c_str(), "w"), BIO_free);
    return pemKey && pemCert &&
           PEM_write_bio_PrivateKey(pemKey.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) == 1 &&
           PEM_write_bio_X509(pemCert.get(), cert.get()) == 1;
}

// Everything the client emits, recorded on the emitting thread.
struct Inbox {
    std::mutex mutex;
    std::condition_variable changed;
    bool connected = false;
    std::vector<std::pair<quint64, SentinelStreamClient::HeatmapChunkPtr>> chunks;
    std::vector<SentinelStreamClient::HeatmapChunkError> errors;
    std::vector<protocol::chunkwire::Availability> availability;

    void attach(SentinelStreamClient& client) {
        auto notify = [this](auto&& fn) { { std::lock_guard lock(mutex); fn(); } changed.notify_all(); };
        QObject::connect(&client, &SentinelStreamClient::connected, &client,
                         [this, notify] { notify([&] { connected = true; }); }, Qt::DirectConnection);
        QObject::connect(&client, &SentinelStreamClient::heatmapChunkReceived, &client,
                         [this, notify](quint64 req, SentinelStreamClient::HeatmapChunkPtr chunk) {
                             notify([&] { chunks.emplace_back(req, std::move(chunk)); });
                         }, Qt::DirectConnection);
        QObject::connect(&client, &SentinelStreamClient::heatmapChunkFailed, &client,
                         [this, notify](const SentinelStreamClient::HeatmapChunkError& error) {
                             notify([&] { errors.push_back(error); });
                         }, Qt::DirectConnection);
        QObject::connect(&client, &SentinelStreamClient::heatmapAvailabilityReceived, &client,
                         [this, notify](const protocol::chunkwire::Availability& a) {
                             notify([&] { availability.push_back(a); });
                         }, Qt::DirectConnection);
    }
    template <class Pred> bool waitFor(Pred pred, std::chrono::milliseconds timeout = 5s) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, timeout, [&] { return pred(); });
    }
    size_t repliesFor(quint64 req) {
        size_t n = 0;
        for (const auto& [id, c] : chunks) n += id == req;
        for (const auto& e : errors) n += e.requestId == req;
        return n;
    }
    bool waitReplies(quint64 req, size_t count, std::chrono::milliseconds timeout = 5s) {
        return waitFor([&] { return repliesFor(req) >= count; }, timeout);
    }
    std::vector<SentinelStreamClient::HeatmapChunkPtr> chunksFor(quint64 req) {
        std::lock_guard lock(mutex);
        std::vector<SentinelStreamClient::HeatmapChunkPtr> out;
        for (const auto& [id, c] : chunks) if (id == req) out.push_back(c);
        return out;
    }
    std::vector<SentinelStreamClient::HeatmapChunkError> errorsFor(quint64 req) {
        std::lock_guard lock(mutex);
        std::vector<SentinelStreamClient::HeatmapChunkError> out;
        for (const auto& e : errors) if (e.requestId == req) out.push_back(e);
        return out;
    }
};

template <class Pred> bool poll(Pred pred, std::chrono::milliseconds timeout = 5s) {
    const auto until = std::chrono::steady_clock::now() + timeout;
    while (!pred()) {
        if (std::chrono::steady_clock::now() >= until) return false;
        std::this_thread::sleep_for(5ms);
    }
    return true;
}

// Controllable recorder watermarks; serve() optionally parks on a gate.
struct Watermarks {
    std::atomic<int64_t> minute{kEpoch + 3 * kDay}, hour{kEpoch + 3 * kDay};
    std::atomic<bool> block{false};
    std::promise<void> release;
    std::shared_future<void> gate = release.get_future().share();
    std::atomic<int> parked{0};
};

class ChunkWire : public testing::Test {
protected:
    int argc = 1;
    char name[16] = "chunk-wire";
    char* argv[2] = {name, nullptr};
    QCoreApplication app{argc, argv};
    QTemporaryDir dir;
    ServerConfig config;
    std::unique_ptr<ServerDataModel> model;
    std::unique_ptr<Authenticator> auth;
    std::unique_ptr<SentinelStreamServer> server;
    std::unique_ptr<SentinelStreamClient> client;
    Inbox inbox;
    Watermarks marks;

    std::filesystem::path root() const { return dir.path().toStdString() + "/recording"; }

    void SetUp() override {
        ASSERT_TRUE(dir.isValid());
        {
            recording::Hmc2Store store(root());
            for (int i = 0; i < 120; ++i) {
                if (i == 9) continue; // a recorder gap
                store.append(minuteRecord(i, "deep"));
                store.append(minuteRecord(i, "near"));
            }
            store.append(hourRecord());
        }
        config.heatmap.persistenceEnabled = false;
        config.recording.enabled = true;
        config.recording.dir = root().string();
        config.tls.certFile = dir.path().toStdString() + "/cert.pem";
        config.tls.keyFile = dir.path().toStdString() + "/key.pem";
        ASSERT_TRUE(writeSelfSignedCert(config.tls.certFile, config.tls.keyFile));
        model = std::make_unique<ServerDataModel>(config);
        ASSERT_TRUE(model->recordingDir());
        auth = std::make_unique<Authenticator>(dir.path().toStdString() + "/no-credentials");
        server = std::make_unique<SentinelStreamServer>(*model, *auth, config, 0);
    }
    void TearDown() override {
        if (marks.block.exchange(false)) marks.release.set_value();
        if (client) client->disconnectFromServer();
        client.reset();
        if (server) server->stop();
    }
    // Replace the model-backed service with controllable watermarks.
    void useControlledWatermarks() {
        HeatmapChunkWireTest::setChunks(*server, std::make_shared<recording::ChunkService>(
            root(),
            [this](const std::string&, const std::string&) {
                if (marks.block.load()) { ++marks.parked; marks.gate.wait(); }
                return recording::BookRecorder::Watermarks{marks.minute.load(), marks.hour.load()};
            },
            [] { return int64_t{0}; }));
    }
    void startAndConnect() {
        server->start();
        const auto port = HeatmapChunkWireTest::port(*server);
        client = std::make_unique<SentinelStreamClient>("127.0.0.1", std::to_string(port));
        inbox.attach(*client);
        client->connectToServer();
        ASSERT_TRUE(inbox.waitFor([&] { return inbox.connected; }));
        ASSERT_TRUE(poll([&] { return HeatmapChunkWireTest::sessionCount(*server) == 1; }));
    }
    std::vector<uint8_t> expectedWire(const ChunkKey& key, const recording::BookRecorder::Watermarks& w,
                                      uint64_t revision = 0) {
        recording::Hmc2Reader reader(root());
        auto state = recording::chunkState(key, w, revision);
        return heatmap::encodeChunk({ChunkKind::Chunk, key, state, recording::buildChunk(reader, key, w)});
    }
};

std::vector<uint8_t> reencode(const heatmap::ChunkFrame& frame) {
    return heatmap::encodeChunk({ChunkKind::Chunk, frame.key, frame.state, frame.columns});
}
} // namespace

TEST_F(ChunkWire, BinaryChunksDecodeExactlyAndHashMatchesGiveNotModified) {
    startAndConnect(); // the model-backed service: year-2000 chunks are long sealed
    const ChunkKey minute0{"BTC-USD", "hmc2.deep", kMin, kEpoch}, minute1{"BTC-USD", "hmc2.deep", kMin, kEpoch + kHour};
    const auto req = client->requestHeatmapChunks("BTC-USD", "hmc2.deep", kMin, {kEpoch, kEpoch + kHour});
    ASSERT_TRUE(inbox.waitReplies(req, 2));
    ASSERT_TRUE(inbox.errorsFor(req).empty());
    const auto chunks = inbox.chunksFor(req);
    ASSERT_EQ(chunks.size(), 2u);
    const recording::BookRecorder::Watermarks sealed{kEpoch + kDay, kEpoch + kDay};
    for (const auto& key : {minute0, minute1}) {
        const auto it = std::find_if(chunks.begin(), chunks.end(), [&](const auto& c) { return c->key == key; });
        ASSERT_NE(it, chunks.end());
        const auto& c = **it;
        EXPECT_EQ(c.kind, ChunkKind::Chunk);
        EXPECT_TRUE(c.state.sealed);
        EXPECT_EQ(c.state.committedThroughMs, key.startMs + kHour); // sealed header is watermark-free
        EXPECT_EQ(c.columns.layer, "deep"); // in-memory only; the wire carried "hmc2.deep"
        const auto expected = expectedWire(key, sealed);
        EXPECT_EQ(reencode(c), expected) << "decoded chunk differs from a local build";
        EXPECT_EQ(c.contentHash, heatmap::chunkContentHash(expected));
    }
    EXPECT_EQ(heatmap::bucketState(chunks[0]->key == minute0 ? chunks[0]->columns : chunks[1]->columns,
                                   kEpoch + 9 * kMin), heatmap::BucketState::Gap);

    // Hour level: one UTC day, deep source only.
    const auto hourReq = client->requestHeatmapChunks("BTC-USD", "hmc2.deep", kHour, {kEpoch});
    ASSERT_TRUE(inbox.waitReplies(hourReq, 1));
    ASSERT_EQ(inbox.chunksFor(hourReq).size(), 1u);
    EXPECT_EQ(reencode(*inbox.chunksFor(hourReq)[0]), expectedWire({"BTC-USD", "hmc2.deep", kHour, kEpoch}, sealed));

    // have_hash: match -> not_modified (no payload), mismatch -> full chunk.
    const auto hash0 = std::find_if(chunks.begin(), chunks.end(), [&](const auto& c) { return c->key == minute0; })
                           ->get()->contentHash;
    const auto nmReq = client->requestHeatmapChunks("BTC-USD", "hmc2.deep", kMin, {kEpoch, kEpoch + kHour},
                                                    {hash0, hash0 ^ 1});
    ASSERT_TRUE(inbox.waitReplies(nmReq, 2));
    const auto nm = inbox.chunksFor(nmReq);
    ASSERT_EQ(nm.size(), 2u);
    for (const auto& c : nm) {
        if (c->key == minute0) {
            EXPECT_EQ(c->kind, ChunkKind::NotModified);
            EXPECT_EQ(c->contentHash, hash0);
            EXPECT_TRUE(c->state.sealed);
            EXPECT_TRUE(c->columns.columns.empty());
        } else {
            EXPECT_EQ(c->kind, ChunkKind::Chunk);
            EXPECT_EQ(reencode(*c), expectedWire(minute1, sealed));
        }
    }
    EXPECT_GE(HeatmapChunkWireTest::cache(*server).size(), 3u); // sealed chunks are cached as exact bytes

    // Per-key errors echo the key; no source may be named by its HMC2 layer.
    const auto badReq = client->requestHeatmapChunks("BTC-USD", "deep", kMin, {kEpoch});
    const auto misaligned = client->requestHeatmapChunks("BTC-USD", "hmc2.near", kHour, {kEpoch});
    ASSERT_TRUE(inbox.waitReplies(badReq, 1));
    ASSERT_TRUE(inbox.waitReplies(misaligned, 1));
    for (const auto req2 : {badReq, misaligned}) {
        const auto errors = inbox.errorsFor(req2);
        ASSERT_EQ(errors.size(), 1u);
        EXPECT_EQ(errors[0].code, "invalid_request");
        EXPECT_EQ(errors[0].key.startMs, kEpoch);
    }
}

TEST_F(ChunkWire, OpenChunksCarryCommittedThroughAndOrderedRevisions) {
    useControlledWatermarks();
    startAndConnect();
    const ChunkKey key{"BTC-USD", "hmc2.near", kMin, kEpoch};
    auto fetch = [&](uint64_t have = 0) {
        const auto req = client->requestHeatmapChunks(key.symbol, key.source, kMin, {kEpoch}, {have});
        EXPECT_TRUE(inbox.waitReplies(req, 1));
        const auto chunks = inbox.chunksFor(req);
        EXPECT_EQ(chunks.size(), 1u) << (inbox.errorsFor(req).empty() ? "" : inbox.errorsFor(req)[0].message.toStdString());
        return chunks.empty() ? nullptr : chunks[0];
    };
    marks.minute = kEpoch + 20 * kMin + 7; // cutoff inside minute 20
    const auto rev21 = fetch();
    ASSERT_TRUE(rev21);
    EXPECT_FALSE(rev21->state.sealed);
    EXPECT_EQ(rev21->state.committedThroughMs, kEpoch + 20 * kMin + 7);
    EXPECT_EQ(rev21->state.revision, 21u);
    ASSERT_EQ(rev21->columns.scannedRanges.size(), 1u);
    EXPECT_EQ(rev21->columns.scannedRanges[0].endMs, kEpoch + 20 * kMin);
    EXPECT_EQ(heatmap::bucketState(rev21->columns, kEpoch + 20 * kMin), heatmap::BucketState::NotLoaded);
    EXPECT_EQ(reencode(*rev21), expectedWire(key, {marks.minute.load(), 0}, 21));

    marks.minute = kEpoch + 40 * kMin;
    const auto rev41 = fetch(rev21->contentHash);
    ASSERT_TRUE(rev41);
    EXPECT_EQ(rev41->kind, ChunkKind::Chunk);
    EXPECT_EQ(rev41->state.revision, 41u);
    EXPECT_GT(rev41->columns.columns.size(), rev21->columns.columns.size());
    const auto unchanged = fetch(rev41->contentHash); // open not_modified while the cutoff holds
    ASSERT_TRUE(unchanged);
    EXPECT_EQ(unchanged->kind, ChunkKind::NotModified);
    EXPECT_EQ(unchanged->state.revision, 41u);
    EXPECT_FALSE(HeatmapChunkWireTest::cache(*server).get(key)); // open chunks are never cached

    marks.minute = kEpoch + 2 * kHour;
    const auto sealed = fetch(rev41->contentHash);
    ASSERT_TRUE(sealed);
    EXPECT_TRUE(sealed->state.sealed);
    EXPECT_EQ(sealed->state.revision, 0u);
    EXPECT_TRUE(HeatmapChunkWireTest::cache(*server).get(key));

    // Client ordering: replies for one key can finish out of order on the
    // server's two workers. An older open revision never replaces a newer one.
    auto frame = [&](const std::shared_ptr<const heatmap::ChunkFrame>& c, uint64_t req) {
        return heatmap::encodeChunkEnvelope(req, reencode(*c));
    };
    const ChunkKey other{"BTC-USD", "hmc2.near", kMin, kEpoch + kHour};
    auto relabel = [&](const std::shared_ptr<const heatmap::ChunkFrame>& c) {
        auto copy = std::make_shared<heatmap::ChunkFrame>(*c);
        copy->key = other;
        copy->columns = {};
        copy->columns.symbol = "BTC-USD"; copy->columns.layer = "near";
        copy->columns.startMs = other.startMs; copy->columns.endMs = other.startMs + kHour;
        copy->state = {false, other.startMs + int64_t(c->state.revision - 1) * kMin, c->state.revision};
        if (c->state.revision > 1) copy->columns.scannedRanges = {{other.startMs, copy->state.committedThroughMs}};
        return std::shared_ptr<const heatmap::ChunkFrame>(copy);
    };
    HeatmapChunkWireTest::clientBinary(*client, frame(relabel(rev41), 9001));
    HeatmapChunkWireTest::clientBinary(*client, frame(relabel(rev21), 9002));
    ASSERT_TRUE(inbox.waitReplies(9002, 1));
    ASSERT_EQ(inbox.chunksFor(9001).size(), 1u);
    ASSERT_EQ(inbox.errorsFor(9002).size(), 1u);
    EXPECT_EQ(inbox.errorsFor(9002)[0].code, "superseded");
    // Sealed always wins, and then no open revision is accepted for that key.
    auto sealedOther = std::make_shared<heatmap::ChunkFrame>(*relabel(rev41));
    sealedOther->state = {true, other.startMs + kHour, 0};
    sealedOther->columns.scannedRanges = {{other.startMs, other.startMs + kHour}};
    HeatmapChunkWireTest::clientBinary(*client, frame(sealedOther, 9003));
    HeatmapChunkWireTest::clientBinary(*client, frame(relabel(rev41), 9004));
    ASSERT_TRUE(inbox.waitReplies(9004, 1));
    EXPECT_EQ(inbox.chunksFor(9003).size(), 1u);
    ASSERT_EQ(inbox.errorsFor(9004).size(), 1u);
    EXPECT_EQ(inbox.errorsFor(9004)[0].code, "superseded");
}

TEST_F(ChunkWire, AvailabilityOnSubscribeAndOnChange) {
    useControlledWatermarks();
    marks.minute = kEpoch + 90 * kMin;
    marks.hour = kEpoch + kHour;
    startAndConnect();
    client->subscribe("BTC-USD");
    ASSERT_TRUE(inbox.waitFor([&] { return !inbox.availability.empty(); }));
    auto first = [&] { std::lock_guard lock(inbox.mutex); return inbox.availability.front(); }();
    EXPECT_EQ(first.symbol, "BTC-USD");
    EXPECT_EQ(first.chunkWireVersion, heatmap::kChunkWireVersion);
    ASSERT_EQ(first.sources.size(), 2u);
    for (const auto& s : first.sources) {
        EXPECT_TRUE(s.id == "hmc2.near" || s.id == "hmc2.deep") << s.id;
        EXPECT_TRUE(s.migrationOnly);
        ASSERT_TRUE(s.latestGrid);
        EXPECT_EQ(s.latestGrid->rowTickUnits, s.id == "hmc2.near" ? 100 : 200);
        ASSERT_EQ(s.levels.size(), s.id == "hmc2.deep" ? 2u : 1u);
        const auto& m = s.levels[0];
        EXPECT_EQ(m.levelMs, kMin);
        EXPECT_EQ(m.chunkSpanMs, kHour);
        EXPECT_EQ(m.oldestMs, kEpoch);
        EXPECT_EQ(m.latestMs, kEpoch + 119 * kMin);
        EXPECT_EQ(m.committedThroughMs, kEpoch + 90 * kMin);
        if (s.id == "hmc2.deep") {
            EXPECT_EQ(s.levels[1].levelMs, kHour);
            EXPECT_EQ(s.levels[1].chunkSpanMs, kDay);
            EXPECT_EQ(s.levels[1].oldestMs, kEpoch);
            EXPECT_EQ(s.levels[1].committedThroughMs, kEpoch + kHour);
        }
    }
    // A watermark move is pushed without a new subscribe (1 s availability tick).
    marks.minute = kEpoch + 100 * kMin;
    ASSERT_TRUE(inbox.waitFor([&] { return inbox.availability.size() >= 2; }, 4s));
    auto second = [&] { std::lock_guard lock(inbox.mutex); return inbox.availability[1]; }();
    for (const auto& s : second.sources) EXPECT_EQ(s.levels[0].committedThroughMs, kEpoch + 100 * kMin);
    // Nothing changed: no duplicate push.
    std::this_thread::sleep_for(1500ms);
    EXPECT_EQ([&] { std::lock_guard lock(inbox.mutex); return inbox.availability.size(); }(), 2u);
}

TEST_F(ChunkWire, BudgetRefusalsAreExplicitErrorsNeverSilentDrops) {
    useControlledWatermarks();
    startAndConnect();
    auto session = HeatmapChunkWireTest::onlySession(*server);
    ASSERT_TRUE(session);
    // Malformed requests: one invalid_request reply that echoes req.
    HeatmapChunkWireTest::sendText(*client, R"({"type":"heatmap_chunk_request","req":77,"symbol":"BTC-USD",)"
                                            R"("source":"hmc2.near","level_ms":60000,"starts":[]})");
    std::vector<int64_t> tooMany(protocol::chunkwire::kMaxStarts + 1, kEpoch);
    const auto overLimit = client->requestHeatmapChunks("BTC-USD", "hmc2.near", kMin, tooMany);
    HeatmapChunkWireTest::sendText(*client, R"({"type":"heatmap_chunk_request","req":78,"symbol":"BTC-USD",)"
                                            R"("source":"hmc2.near","level_ms":60000,"starts":[1],"have_hash":["zz"]})");
    for (const quint64 req : {quint64(77), overLimit, quint64(78)}) {
        ASSERT_TRUE(inbox.waitReplies(req, 1)) << req;
        const auto errors = inbox.errorsFor(req);
        ASSERT_EQ(errors.size(), 1u);
        EXPECT_EQ(errors[0].code, "invalid_request");
        EXPECT_EQ(errors[0].key.symbol, "BTC-USD");
    }

    // Park every build: 4 jobs are admitted (session budget), the rest are refused.
    marks.block = true;
    std::vector<int64_t> starts;
    for (int i = 0; i < 10; ++i) starts.push_back(kEpoch + i * kHour);
    const auto req = client->requestHeatmapChunks("BTC-USD", "hmc2.near", kMin, starts);
    ASSERT_TRUE(inbox.waitReplies(req, 6));
    auto busy = inbox.errorsFor(req);
    ASSERT_EQ(busy.size(), 6u);
    for (const auto& e : busy) {
        EXPECT_EQ(e.code, "busy");
        EXPECT_TRUE(e.message.contains("session chunk budget")) << e.message.toStdString();
        EXPECT_GE(e.key.startMs, kEpoch + 4 * kHour);
    }
    EXPECT_EQ(HeatmapChunkWireTest::chunkJobs(*session), 4u);
    EXPECT_EQ(HeatmapChunkWireTest::pendingTasks(*server), 4u); // half the server pool stays free
    // Another request while saturated: refused, never queued.
    const auto more = client->requestHeatmapChunks("BTC-USD", "hmc2.near", kMin, {kEpoch});
    ASSERT_TRUE(inbox.waitReplies(more, 1));
    EXPECT_EQ(inbox.errorsFor(more).at(0).code, "busy");
    marks.block = false;
    marks.release.set_value();
    ASSERT_TRUE(inbox.waitReplies(req, 10));
    EXPECT_EQ(inbox.chunksFor(req).size(), 4u);
    EXPECT_TRUE(poll([&] { return HeatmapChunkWireTest::chunkJobs(*session) == 0; }));

    // Byte budget: queued chunk replies at the cap refuse new jobs.
    HeatmapChunkWireTest::setChunkBytes(*session, HeatmapChunkWireTest::maxChunkBytes());
    const auto bytesReq = client->requestHeatmapChunks("BTC-USD", "hmc2.near", kMin, {kEpoch});
    ASSERT_TRUE(inbox.waitReplies(bytesReq, 1));
    ASSERT_EQ(inbox.errorsFor(bytesReq).size(), 1u);
    EXPECT_EQ(inbox.errorsFor(bytesReq)[0].code, "busy");
    EXPECT_TRUE(inbox.errorsFor(bytesReq)[0].message.contains("bytes=")) ;
    HeatmapChunkWireTest::setChunkBytes(*session, 0);
    const auto after = client->requestHeatmapChunks("BTC-USD", "hmc2.near", kMin, {kEpoch});
    ASSERT_TRUE(inbox.waitReplies(after, 1));
    EXPECT_EQ(inbox.chunksFor(after).size(), 1u);
}

TEST_F(ChunkWire, HostileBinaryFramesAreRejectedWithoutCrashing) {
    startAndConnect();
    auto session = HeatmapChunkWireTest::onlySession(*server);
    ASSERT_TRUE(session);
    const ChunkKey key{"BTC-USD", "hmc2.deep", kMin, kEpoch};
    const auto good = heatmap::encodeChunkEnvelope(4242, expectedWire(key, {kEpoch + kDay, kEpoch + kDay}));
    std::vector<std::vector<uint8_t>> hostile;
    hostile.push_back({0x00});                                       // short envelope (rejected inline)
    hostile.push_back(std::vector<uint8_t>(good.begin(), good.begin() + good.size() / 2)); // truncated chunk
    auto flipped = good; flipped[good.size() - 3] ^= 0x40;           // payload corruption (hash)
    hostile.push_back(flipped);
    auto version = good; version[14 + 4] = 1;                        // v1 chunk: refused, no shim
    hostile.push_back(version);
    auto hugeCounts = good;                                          // columns/entries claim billions
    const size_t countsAt = 14 + 9 + good[14 + 8] + 1 + good[14 + 9 + good[14 + 8]] + 24 + 40 + 8 + 8 + 8;
    for (size_t i = 0; i < 8; ++i) hugeCounts[countsAt + i] = 0xff;
    hostile.push_back(hugeCounts);
    std::mt19937 rng(7);
    std::vector<uint8_t> noise(4096);
    for (auto& b : noise) b = uint8_t(rng());
    hostile.push_back(noise);
    auto noisyTail = good; for (size_t i = 60; i < noisyTail.size(); ++i) noisyTail[i] = uint8_t(rng());
    hostile.push_back(noisyTail);

    // Over the real socket: the server writes raw binary frames to the client.
    for (const auto& bytes : hostile) HeatmapChunkWireTest::serverBinary(*session, bytes);
    ASSERT_TRUE(inbox.waitFor([&] { return inbox.errors.size() >= hostile.size(); }));
    {
        std::lock_guard lock(inbox.mutex);
        for (const auto& e : inbox.errors) EXPECT_EQ(e.code, "malformed") << e.message.toStdString();
        EXPECT_TRUE(inbox.chunks.empty());
        // A readable envelope id is still reported for correlation.
        EXPECT_EQ(inbox.errors[2].requestId, 4242u);
        EXPECT_EQ(inbox.errors[0].requestId, 0u);
    }
    // The connection survives and a valid frame still decodes exactly.
    HeatmapChunkWireTest::serverBinary(*session, good);
    ASSERT_TRUE(inbox.waitFor([&] { return !inbox.chunks.empty(); }));
    EXPECT_EQ(reencode(*inbox.chunksFor(4242).at(0)), expectedWire(key, {kEpoch + kDay, kEpoch + kDay}));
    // Decode backlog bound: an oversized local backlog refuses instead of queueing.
    HeatmapChunkWireTest::setDecodeBacklog(*client, SentinelStreamClient::kMaxDecodeBacklogBytes);
    HeatmapChunkWireTest::clientBinary(*client, good);
    ASSERT_TRUE(inbox.waitFor([&] {
        return std::any_of(inbox.errors.begin(), inbox.errors.end(),
                           [](const auto& e) { return e.code == "client_overloaded" && e.requestId == 4242; });
    }));
    HeatmapChunkWireTest::setDecodeBacklog(*client, 0);
}

TEST(ChunkClientAdmission, EmptyTinyAndSmallFramesHaveAFixedOutstandingBound) {
    SentinelStreamClient client("127.0.0.1", "0");
    Inbox inbox;
    inbox.attach(client);
    heatmap::ChunkFrame frame;
    frame.kind = ChunkKind::NotModified;
    frame.key = {"BTC-USD", "hmc2.deep", kMin, kEpoch};
    const auto good = heatmap::encodeChunkEnvelope(77, heatmap::encodeChunk(frame));
    std::promise<void> entered, release;
    auto gate = release.get_future().share();
    std::atomic<size_t> calls{0};
    HeatmapChunkWireTest::decoder(client, [&](auto wire) {
        if (++calls == 1) entered.set_value();
        gate.wait();
        return heatmap::decodeChunkEnvelope(wire);
    });
    HeatmapChunkWireTest::clientBinary(client, good);
    entered.get_future().get();
    for (size_t i = 0; i < 1024; ++i) {
        HeatmapChunkWireTest::clientBinary(client, {});
        HeatmapChunkWireTest::clientBinary(client, {0});
        HeatmapChunkWireTest::clientBinary(client, std::vector<uint8_t>(13));
    }
    EXPECT_EQ(HeatmapChunkWireTest::decodeBacklogFrames(client), 1u);
    EXPECT_EQ(inbox.errors.size(), 1u); // malformed flood is coalesced, not queued
    for (size_t i = 0; i < 1024; ++i) HeatmapChunkWireTest::clientBinary(client, good);
    EXPECT_EQ(HeatmapChunkWireTest::decodeBacklogFrames(client), SentinelStreamClient::kMaxDecodeBacklogFrames);
    EXPECT_EQ(inbox.errors.size(), 2u); // only one additional overload notification
    release.set_value();
    HeatmapChunkWireTest::drainDecoder(client);
    EXPECT_EQ(calls.load(), SentinelStreamClient::kMaxDecodeBacklogFrames);
    EXPECT_EQ(inbox.chunks.size(), SentinelStreamClient::kMaxDecodeBacklogFrames);
    EXPECT_EQ(HeatmapChunkWireTest::decodeBacklogFrames(client), 0u);
    HeatmapChunkWireTest::clientBinary(client, good);
    HeatmapChunkWireTest::drainDecoder(client);
    EXPECT_EQ(calls.load(), SentinelStreamClient::kMaxDecodeBacklogFrames + 1);
}

TEST_F(ChunkWire, InProgressDecodeCannotCrossDisconnectOrReconnect) {
    server->start();
    client = std::make_unique<SentinelStreamClient>("127.0.0.1", std::to_string(HeatmapChunkWireTest::port(*server)));
    inbox.attach(*client);
    client->connectToServer();
    ASSERT_TRUE(inbox.waitFor([&] { return inbox.connected; }));
    heatmap::ChunkFrame sealed;
    sealed.kind = ChunkKind::NotModified;
    sealed.key = {"BTC-USD", "hmc2.deep", kMin, kEpoch};
    sealed.state = {true, kEpoch + kHour, 0};
    const auto good = heatmap::encodeChunkEnvelope(9001, heatmap::encodeChunk(sealed));
    auto error = sealed;
    error.kind = ChunkKind::Error;
    error.error = heatmap::ChunkError::Busy;
    const auto serverError = heatmap::encodeChunkEnvelope(9001, heatmap::encodeChunk(error));
    auto malformed = good;
    malformed.back() = 0;
    malformed.push_back(0); // exact-length control frame violation
    for (bool reconnect : {false, true}) {
        for (const auto& bytes : {good, serverError, malformed}) {
            client->connectToServer();
            std::promise<void> entered, release;
            auto gate = release.get_future().share();
            HeatmapChunkWireTest::decoder(*client, [&](auto wire) {
                entered.set_value();
                gate.wait(); // already inside decode, beyond the worker's first epoch check
                return heatmap::decodeChunkEnvelope(wire);
            });
            HeatmapChunkWireTest::clientBinary(*client, bytes);
            entered.get_future().get();
            client->disconnectFromServer();
            if (reconnect) client->connectToServer();
            release.set_value();
            HeatmapChunkWireTest::drainDecoder(*client);
            EXPECT_EQ(HeatmapChunkWireTest::orderSize(*client), 0u);
            EXPECT_EQ(inbox.repliesFor(9001), 0u);
        }
    }
    HeatmapChunkWireTest::decoder(*client, heatmap::decodeChunkEnvelope);
    sealed.state = {false, kEpoch, 1}; // old sealed reply must not supersede this open reply
    HeatmapChunkWireTest::clientBinary(*client, heatmap::encodeChunkEnvelope(9002, heatmap::encodeChunk(sealed)));
    HeatmapChunkWireTest::drainDecoder(*client);
    EXPECT_EQ(inbox.chunksFor(9002).size(), 1u);
    EXPECT_TRUE(inbox.errorsFor(9002).empty());
}

TEST_F(ChunkWire, SessionTeardownWithRequestsInFlight) {
    useControlledWatermarks();
    startAndConnect();
    std::weak_ptr<Session> session = HeatmapChunkWireTest::onlySession(*server);
    ASSERT_FALSE(session.expired());
    marks.block = true;
    const auto req = client->requestHeatmapChunks("BTC-USD", "hmc2.deep", kMin,
                                                  {kEpoch, kEpoch + kHour, kEpoch + 2 * kHour, kEpoch + 3 * kHour});
    ASSERT_TRUE(poll([&] { return marks.parked.load() >= 2; }));
    EXPECT_EQ(HeatmapChunkWireTest::pendingTasks(*server), 4u);
    // The client goes away while both workers are inside serve().
    client->disconnectFromServer();
    ASSERT_TRUE(poll([&] { return HeatmapChunkWireTest::sessionCount(*server) == 0; }));
    marks.block = false;
    marks.release.set_value();
    // Late replies find a closed or destroyed session and are discarded.
    EXPECT_TRUE(poll([&] { return HeatmapChunkWireTest::pendingTasks(*server) == 0; }));
    EXPECT_TRUE(poll([&] { return session.expired(); }, 2s)) << "a finished job must not keep the session alive";
    EXPECT_EQ(inbox.repliesFor(req), 0u);
    server->stop();

    // Stop with jobs parked: stop() joins workers only after release.
    server->start();
    client = std::make_unique<SentinelStreamClient>("127.0.0.1", std::to_string(HeatmapChunkWireTest::port(*server)));
    Inbox second;
    second.attach(*client);
    client->connectToServer();
    ASSERT_TRUE(second.waitFor([&] { return second.connected; }));
    std::promise<void> release2;
    marks.gate = release2.get_future().share();
    marks.parked = 0;
    marks.release = std::promise<void>(); // part one consumed it; TearDown may set this one
    marks.block = true;
    // Uncached keys: the sealed chunks built above are served from the LRU without parking.
    client->requestHeatmapChunks("BTC-USD", "hmc2.deep", kMin, {kEpoch + 4 * kHour, kEpoch + 5 * kHour});
    ASSERT_TRUE(poll([&] { return marks.parked.load() >= 2; }));
    auto stopped = std::async(std::launch::async, [&] { server->stop(); });
    EXPECT_EQ(stopped.wait_for(200ms), std::future_status::timeout);
    marks.block = false;
    release2.set_value();
    marks.release.set_value();
    EXPECT_EQ(stopped.wait_for(5s), std::future_status::ready);
    client->disconnectFromServer();
}

TEST(ChunkServiceWatermarks, RecorderCutoffsWinAndColdSeriesKeepAMargin) {
    QTemporaryDir dir;
    std::atomic<int64_t> minute{0};
    const int64_t now = kEpoch + 10 * kDay + 13 * kHour + 2 * kMin + 30'000; // 13:02:30
    recording::ChunkService service(dir.path().toStdString(),
        [&](const std::string&, const std::string&) { return recording::BookRecorder::Watermarks{minute.load(), 7}; },
        [&] { return now; });
    // No recorder watermark: 13:02:30 - 5 min = 12:57:30 -> minutes before 12:57,
    // hours before 11:00 (the rollup of 11:00-12:00 may still be written).
    const auto cold = service.effectiveWatermarks("BTC-USD", "deep");
    EXPECT_EQ(cold.minuteThroughMs, kEpoch + 10 * kDay + 12 * kHour + 57 * kMin);
    EXPECT_EQ(cold.hourThroughMs, kEpoch + 10 * kDay + 11 * kHour);
    minute = kEpoch + 42;
    const auto live = service.effectiveWatermarks("BTC-USD", "deep");
    EXPECT_EQ(live.minuteThroughMs, kEpoch + 42);
    EXPECT_EQ(live.hourThroughMs, 7);
    EXPECT_EQ(service.availabilityFingerprint("BTC-USD"), (std::vector<int64_t>{kEpoch + 42, 7, kEpoch + 42, 7}));
}
