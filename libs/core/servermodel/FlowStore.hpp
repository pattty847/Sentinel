#pragma once
#include "flow/FlowCodec.hpp"
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <stop_token>
#include <thread>

namespace recording {
// Resolve symlinks and reject overlap in either direction with either source tree.
std::filesystem::path flowRootFor(const std::string &configured, const std::filesystem::path &hmc2Root,
                                  const std::filesystem::path &journalRoot);

// Derived 1m trade bars. All replay runs on a dedicated background thread.
// The owner must stop the store before destroying its watermark provider.
class FlowStore {
  public:
    struct Options {
        std::filesystem::path hmc2Root, journalRoot, root;
        std::vector<std::string> symbols;
        // Maximum committed hourThroughMs across the product's recorder layers.
        std::function<int64_t(const std::string &)> hourThrough;
        std::chrono::milliseconds rescan{60'000};
        // Same fixed-point volume units on history and live builders.
        int64_t baseScale = 100'000'000, quoteScale = 100'000'000;
        // Cancellation-aware synchronization seam, called on the builder after a record.
        std::function<void(std::stop_token)> afterRecordForTest;
        std::function<void(const std::filesystem::path &)> beforeCommitForTest;
    };
    struct PassResult {
        uint64_t built = 0, failed = 0;
        bool stopped = false;
    };
    struct Reply {
        std::vector<sentinel::flow::FlowBar> bars;
        std::vector<uint32_t> revisions; // parallel to bars, from the FLD1 index
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
    std::filesystem::path filePath(const std::string &symbol, int64_t dayMs) const;
    const std::filesystem::path &root() const { return options_.root; }

  private:
    Options options_;
    std::mutex passMutex_, wakeMutex_;
    std::condition_variable wakeCv_;
    bool woken_ = false;
    std::stop_source stopSource_;
    std::thread thread_;
    void builderLoop(std::stop_token stop);
    void passSymbol(const std::string &symbol, std::stop_token stop, PassResult &result);
    void writeDay(const sentinel::flow::DayFile &day, uint32_t hours, uint32_t revision, const std::string &sources,
                  std::stop_token stop);
    bool ownedPath(const std::filesystem::path &path) const;
};
} // namespace recording
