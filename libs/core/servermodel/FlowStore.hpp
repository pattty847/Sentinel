#pragma once
#include "flow/FlowCodec.hpp"
#include "flow/FlowScales.hpp"
#include <chrono>
#include <array>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <map>
#include <memory>
#include <stop_token>
#include <thread>

namespace sentinel::roller { class JournalReader; }
namespace recording {
struct FlowStoreGridValues {
    sentinel::flow::FlowScales scales;
    double sizeFloor = 0;
    uint64_t configHash = 0;
    bool hasHmc2 = false;
    bool operator==(const FlowStoreGridValues &) const = default;
};
// Resolve symlinks and reject overlap in either direction with source, rollup,
// and protected recording roots.
std::filesystem::path flowRootFor(const std::string &configured, const std::filesystem::path &hmc2Root,
                                  const std::filesystem::path &journalRoot,
                                  const std::filesystem::path &rollupRoot = {},
                                  const std::vector<std::filesystem::path> &protectedRoots = {});

// Derived 1m trade bars. Persistent builds run on a dedicated background thread;
// bounded one-shot second windows use the caller's history worker.
// The owner must stop the store before destroying its watermark provider.
class FlowStore {
  public:
    struct Options {
        std::filesystem::path hmc2Root, journalRoot, root;
        std::vector<std::string> symbols;
        // Maximum committed hourThroughMs across the product's recorder layers.
        std::function<int64_t(const std::string &)> hourThrough;
        std::chrono::milliseconds rescan{60'000};
        std::filesystem::path rollupRoot;
        std::vector<std::filesystem::path> protectedRoots;
        // Tests keep the worker schedulable when timing stop/join.
        bool lowerPriority = true;
        // Cancellation-aware synchronization seam, called on the builder after a record.
        std::function<void(std::stop_token)> afterRecordForTest;
        std::function<void(const std::filesystem::path &)> beforeCommitForTest;
        std::function<void()> afterCatalogConstructForTest;
        std::function<void(std::stop_token)> afterInventoryFileForTest;
    };
    struct PassResult {
        uint64_t built = 0, failed = 0;
        bool stopped = false;
    };
    struct Range {
        int64_t fromMs = 0, toMs = 0; // half-open, including known empty buckets
        std::string status; // complete, partial, building
        sentinel::flow::FlowScales scales;
    };
    struct Reply {
        std::vector<sentinel::flow::FlowBar> bars;
        std::vector<uint32_t> revisions; // parallel to bars, from the FLD1 index
        std::vector<Range> ranges;
        int64_t sealedThroughMs = 0;
        // True if any intersecting hour is unsealed, missing, stale or corrupt.
        bool building = false;
    };
    explicit FlowStore(Options options);
    ~FlowStore();
    FlowStore(const FlowStore &) = delete;
    FlowStore &operator=(const FlowStore &) = delete;
    void start();
    void stop(); // idempotent, cancels replay and joins
    void wake();
    PassResult runPass(std::stop_token stop = {}); // synchronous test/offline entry; serialized
    // Thread-safe disk read, intended for the history worker pool. No journal replay.
    // Minute bucket starts in [fromMs, toMs); requests must be UTC minute aligned.
    Reply window(const std::string &symbol, int64_t fromMs, int64_t toMs);
    static constexpr int64_t kMaxReplayWindowMs = 3'600'000;
    // One-shot 1s replay; worker-local readers, no persistent writes or shared catalog.
    Reply buildWindow(const std::string &symbol, int64_t fromMs, int64_t toMs,
                      std::stop_token stop = {});
    std::filesystem::path filePath(const std::string &symbol, int64_t dayMs) const;
    const std::filesystem::path &root() const { return options_.root; }

  private:
    Options options_;
    std::mutex passMutex_, wakeMutex_;
    std::condition_variable wakeCv_;
    bool woken_ = false;
    std::chrono::steady_clock::time_point lastWindowWake_{};
    struct FileIdentity {
        uintmax_t size = 0;
        std::filesystem::file_time_type modified{};
        bool present = false;
        bool operator==(const FileIdentity &) const = default;
    };
    struct DayIndex {
        uint32_t hours = 0, revision = 0;
        std::string sources; // sidecar manifest
        std::vector<std::filesystem::path> sourcePaths;
        std::array<uint8_t, 32> fingerprint{};
        FlowStoreGridValues grid;
        std::filesystem::path gridPath; // current HMC2 generation, transient
        bool gridKnown = false;
        FileIdentity file, state;
        bool valid = false;
    };
    mutable std::mutex indexMutex_;
    std::map<std::pair<std::string, int64_t>, DayIndex> index_;
    std::map<std::string, std::map<std::filesystem::path, FileIdentity>> journalInventory_;
    std::map<std::string, std::unique_ptr<sentinel::roller::JournalReader>> catalogs_; // builder thread only
    std::map<std::string, int64_t> firstHour_; // builder thread only
    std::stop_source stopSource_;
    std::thread thread_;
    void builderLoop(std::stop_token stop);
    void passSymbol(const std::string &symbol, std::stop_token stop, PassResult &result);
    void writeDay(const sentinel::flow::DayFile &day, uint32_t hours, uint32_t revision,
                  const FlowStoreGridValues &grid, const std::string &sources,
                  std::stop_token stop);
    bool ownedPath(const std::filesystem::path &path) const;
    static FileIdentity stamp(const std::filesystem::path &path);
    DayIndex loadIndex(const std::string &symbol, int64_t day) const;
    DayIndex indexFor(const std::string &symbol, int64_t day);
    void publishIndex(const std::string &symbol, int64_t day, const DayIndex &index);
    std::map<std::filesystem::path, FileIdentity> inventory(const std::string &symbol, std::stop_token stop) const;
};
} // namespace recording
