#pragma once
// Rebuildable FLC1 files only. FlowStore owns the shared RAWL2 catalog/inventory
// and must outlive this store. No network or live-model work belongs here.
#include "FlowStore.hpp"
#include "roller/JournalReader.hpp"
#include "flow/FlowEpoch.hpp"
#include "metrics/MetricsRegistry.hpp"

namespace recording {
void atomicFlowFile(const std::filesystem::path &, const QByteArray &);
class FlowChunkStore {
public:
    struct Options {
        std::filesystem::path root;
        sentinel::flow::chunks::RuleValues rules;
        std::map<std::string, uint64_t> bigFloors; // instrument-specific rule identity
        std::chrono::milliseconds rescan{60'000};
        bool lowerPriority = true;
        sentinel::metrics::MetricsRegistry *metrics = nullptr; // outlives store
        std::function<void(std::stop_token)> afterRecordForTest;
        std::function<void(int64_t)> afterDayForTest;
        std::function<void()> afterAvailabilityReadForTest;
    };
    struct Payload {
        uint64_t epoch = 0;
        uint32_t summaryLength = 0;
        uint8_t stepExponent = 0;
        std::vector<uint8_t> bytes;
    };
    struct Range { int64_t fromMs, toMs; bool operator==(const Range &) const = default; };
    struct SourceCheck {
        sentinel::roller::JournalPos cursor, first, last;
        uint64_t stamp = 0, prefix = 0;
        FlowStore::SourceState source = FlowStore::SourceState::Unknown;
    };
    struct HourResult {
        std::optional<SourceCheck> source;
        sentinel::flow::chunks::DaySection section;
        uint32_t late = 0, rejected = 0, regressions = 0;
        // Optional offline evidence; never retained by the store.
        std::vector<sentinel::flow::Trade> accepted, excluded;
    };
    FlowChunkStore(FlowStore &sources, Options options);
    ~FlowChunkStore();
    void start();
    void stop();
    void wake();
    FlowStore::PassResult runPass(std::stop_token stop = {});
    // Synchronous startup fences, without building. start/runPass call it too.
    void initialize(std::stop_token stop = {});
    // Offline targeted rebuild, sharing the production scan/composition/writer.
    FlowStore::PassResult rebuildDay(const std::string &instrument, int64_t day, std::stop_token stop = {});
    uint64_t epoch(const std::string &instrument) const;
    std::vector<Range> sealed(const std::string &instrument, int64_t levelMs);
    std::optional<Payload> read(const std::string &instrument, int64_t levelMs, int64_t startMs);
    std::filesystem::path filePath(const std::string &instrument, int64_t day) const;
    // Canonical independent scan, also used by the read-only offline diff tool.
    HourResult buildHour(const std::string &symbol, int64_t hour,
                         const FlowStoreGridValues &grid, const sentinel::roller::JournalReader &catalog,
                         std::stop_token stop = {}, bool evidence = false);
private:
    struct Day {
        uint32_t hours = 0; bool coarse = false; bool invalid = false;
        // Replay validation metadata only; no duplicate journal catalog or bytes.
        std::array<std::optional<SourceCheck>, 24> sources;
    };
    struct Instrument {
        sentinel::flow::chunks::EpochState state;
        std::map<int64_t, Day> days;
        bool initialized = false;
        uint64_t generation = UINT64_MAX;
        sentinel::metrics::Counter *late = nullptr, *regressions = nullptr, *failures = nullptr, *hours = nullptr;
        sentinel::metrics::Gauge *pending = nullptr;
    };
    FlowStore &sources_;
    Options options_;
    mutable std::mutex mutex_;
    std::mutex passMutex_, wakeMutex_;
    std::condition_variable wakeCv_;
    bool woken_ = false;
    std::map<std::string, Instrument> instruments_;
    std::stop_source stopSource_;
    std::thread thread_;
    std::filesystem::path checked(const std::filesystem::path &) const;
    void write(const std::filesystem::path &, const std::vector<uint8_t> &);
    std::optional<sentinel::flow::chunks::DayFile> load(const std::string &, int64_t);
    void initializeInstrument(const std::string &, const FlowStore::Archive &);
    void transition(const std::string &, int64_t);
    void finishPending(const std::string &, int64_t);
    void buildDay(const std::string &, int64_t, const FlowStore::Archive &, std::stop_token,
                  FlowStore::PassResult &);
    bool unchanged(const std::string &, int64_t, const FlowStore::Archive &, std::stop_token);
    void publish(const std::string &, const sentinel::flow::chunks::DayFile &);
    void loop(std::stop_token);
};
} // namespace recording
