#include "JournalReader.hpp"
#include "SentinelLogging.hpp"
#include "metrics/MetricsRegistry.hpp"
#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <tuple>

namespace sentinel::roller {
using nlohmann::json;
void to_json(json& j, const JournalPos& p) { j = {{"product",p.product},{"run_id",p.run},{"block",p.block},{"record",p.record}}; }
void from_json(const json& j, JournalPos& p) {
    j.at("product").get_to(p.product); j.at("run_id").get_to(p.run);
    j.at("block").get_to(p.block); j.at("record").get_to(p.record);
}
namespace {
struct CorruptionState {
    metrics::Counter count;
    std::mutex mutex;
    std::set<std::tuple<std::string, uint64_t, uint64_t>> blocks;
};
std::shared_ptr<CorruptionState> corruptionState(const std::string& product) {
    static std::mutex mutex;
    static std::map<std::string, std::shared_ptr<CorruptionState>> products;
    std::lock_guard lock(mutex);
    auto& state = products[product];
    if (!state) state = std::make_shared<CorruptionState>();
    return state;
}
void reportCorruption(const std::string& product, const JournalFile& file,
                      const capture::BlockIndex& block, const char* reason) {
    const auto state = corruptionState(product);
    // Rare error path only. Remember run/segment/offset across anchor searches,
    // daily replay and independent batch comparisons in this process.
    std::lock_guard lock(state->mutex);
    if (state->blocks.emplace(file.header.at("run_id").get<std::string>(),
                              file.header.at("segment").get<uint64_t>(), block.offset).second) {
        state->count.inc();
        sLog_Error("Roller journal corrupt block product=" << product.c_str()
                   << " file=" << file.path.string().c_str() << " offset=" << block.offset
                   << " reason=" << reason << " action=skip_until_snapshot");
    }
}
auto order(const json& h) {
    return std::tuple(h.at("run_started_system_ns").get<int64_t>(), h.at("run_id").get<std::string>(),
                      h.at("segment").get<uint64_t>());
}
bool snapshot(const capture::Record& r, const std::string& product) {
    if (r.kind != capture::Kind::Frame || r.payload.find("snapshot") == std::string::npos) return false;
    const auto j = json::parse(r.payload, nullptr, false);
    if (!j.is_object() || j.value("channel", "") != "l2_data" || !j.contains("events")) return false;
    for (const auto& e : j["events"])
        if (e.value("product_id", "") == product && e.value("type", "") == "snapshot") return true;
    return false;
}
}
void registerJournalMetrics(metrics::MetricsRegistry& registry, const std::string& product) {
    // Serialize our check/register pair; registry identity is its own series
    // storage, so destruction/reconstruction at the same address is safe.
    static std::mutex registrationMutex;
    std::lock_guard lock(registrationMutex);
    if (registry.hasSeries("sentinel_roller_journal_corrupt_blocks_total", {{"product", product}})) return;
    const auto state = corruptionState(product);
    registry.counterFn("sentinel_roller_journal_corrupt_blocks_total",
        "Distinct corrupt journal blocks or framing regions skipped by this process.", {{"product", product}},
        [state] { return static_cast<double>(state->count.value()); });
}
JournalReader::JournalReader(std::filesystem::path root, std::string product, std::optional<JournalPos> start)
    : product_(std::move(product)), start_(std::move(start)) {
    capture::validateSymbol(product_);
    if (std::filesystem::is_directory(root / product_)) root /= product_;
    for (const auto& e : std::filesystem::recursive_directory_iterator(root)) {
        if (!e.is_regular_file() || e.path().extension() != ".rawl2") continue;
        auto h = capture::readHeader(QString::fromStdString(e.path().string()));
        if (h.at("product_metadata").at("product_id") == product_) files_.push_back({e.path(), std::move(h)});
    }
    std::sort(files_.begin(), files_.end(), [](const auto& a, const auto& b) { return order(a.header) < order(b.header); });
    if (files_.empty()) throw std::runtime_error("no journal files for " + product_);
    for (size_t i = 0; i < files_.size(); ++i) {
        if (i && order(files_[i-1].header) == order(files_[i].header)) throw std::runtime_error("duplicate journal segment");
        files_[i].superseded = i + 1 < files_.size();
    }
    if (start_) {
        if (start_->product != product_) throw std::runtime_error("journal position product mismatch");
        bool found = false;
        for (size_t i = 0; i < files_.size(); ++i) {
            const auto& h = files_[i].header;
            if (h.at("run_id") == start_->run && h.at("first_block_ordinal").get<uint64_t>() <= start_->block) {
                file_ = i; found = true;
            }
        }
        if (!found) throw std::runtime_error("journal position unavailable");
    }
}
bool JournalReader::next(JournalRecord& out) {
    while (file_ < files_.size()) {
        const auto& f = files_[file_];
        if (!reader_) reader_ = std::make_unique<capture::RecordReader>(
            QString::fromStdString(f.path.string()), !f.superseded,
            [this, &f](const capture::BlockIndex& block, const char* reason) {
                reportCorruption(product_, f, block, reason);
                gap_ = true;
                // A saved cursor in a now-damaged block cannot be delivered;
                // return its first readable successor with a gap.
                if (start_ && f.header.at("run_id") == start_->run && start_->block == block.ordinal)
                    start_.reset();
            });
        capture::Record record;
        if (reader_->next(record)) {
            const auto& scan = reader_->result();
            JournalPos pos{product_, f.header.at("run_id"), scan.recordOrdinal, scan.recordIndex};
            if (start_) {
                if (pos.run == start_->run && std::pair(pos.block, pos.record) < std::pair(start_->block, start_->record)) continue;
                if (pos != *start_) throw std::runtime_error("journal position missing record");
                start_.reset();
            }
            out = {std::move(record), std::move(pos), f.header.at("product_metadata"), std::exchange(gap_, false), f.header.at("format_version").get<int>()};
            return true;
        }
        const auto& result = reader_->result();
        pending_ = !f.superseded && !result.indexed;
        gap_ = gap_ || result.tornTail || (f.superseded && !result.indexed);
        nextBlock_ = f.header.at("first_block_ordinal").get<uint64_t>() + result.index.size();
        ++file_;
        if (file_ < files_.size()) {
            const auto& next = files_[file_].header;
            gap_ = gap_ || next.at("run_id") != f.header.at("run_id") ||
                next.at("first_block_ordinal").get<uint64_t>() != nextBlock_ ||
                next.at("segment").get<uint64_t>() != f.header.at("segment").get<uint64_t>() + 1;
        }
        reader_.reset();
    }
    if (start_) throw std::runtime_error("journal position beyond durable prefix");
    return false;
}
std::optional<JournalPos> JournalReader::anchor(int64_t receiveMs) const {
    // File opening times can regress. Walk journal order backwards, never binary
    // search wall time; then choose the last qualifying snapshot in that segment.
    for (auto f = files_.rbegin(); f != files_.rend(); ++f) {
        if (f->header.at("opened_system_ns").get<int64_t>() / 1'000'000 > receiveMs) continue;
        capture::RecordReader reader(QString::fromStdString(f->path.string()), !f->superseded,
            [&](const capture::BlockIndex& block, const char* reason) {
                reportCorruption(product_, *f, block, reason);
            });
        capture::Record r;
        std::optional<JournalPos> found;
        while (reader.next(r)) if (r.time.systemNs / 1'000'000 <= receiveMs && snapshot(r, product_)) {
            found = JournalPos{product_, f->header.at("run_id"), reader.result().recordOrdinal, reader.result().recordIndex};
        }
        if (found) return found;
    }
    return {};
}
} // namespace sentinel::roller
