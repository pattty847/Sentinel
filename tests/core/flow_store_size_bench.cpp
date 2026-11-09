#include "flow/FlowBarBuilder.hpp"
#include "flow/FlowCodec.hpp"
#include "roller/Grid.hpp"
#include "roller/JournalFeed.hpp"
#include <charconv>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>

using namespace sentinel;
using namespace sentinel::flow;
using namespace sentinel::roller;

int main(int argc, char** argv) {
    try {
        if (argc != 3) throw std::invalid_argument("usage: flow_store_size_bench <journal-root> <YYYY-MM-DD>");
        int y = 0, m = 0, d = 0;
        if (std::sscanf(argv[2], "%d-%d-%d", &y, &m, &d) != 3)
            throw std::invalid_argument("invalid date");
        const auto day = std::chrono::year{y}/std::chrono::month{unsigned(m)}/std::chrono::day{unsigned(d)};
        if (!day.ok()) throw std::invalid_argument("invalid date");
        const int64_t start = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::sys_days{day}.time_since_epoch()).count();
        const int64_t end = start + 86'400'000;
        JournalReader catalog(argv[1], "BTC-USD");
        if (catalog.files().empty()) throw std::runtime_error("no BTC-USD journal files");
        const JournalFile* chosen = &catalog.files().front();
        for (const auto& f : catalog.files()) {
            if (f.header.at("opened_system_ns").get<int64_t>() / 1'000'000 <= start) chosen = &f;
            else break;
        }
        const auto& h = chosen->header;
        JournalReader reader(argv[1], "BTC-USD", JournalPos{"BTC-USD",
            h.at("run_id").get<std::string>(), h.at("first_block_ordinal").get<uint64_t>(), 0});
        JournalFeed feed("BTC-USD");
        std::unique_ptr<FlowBarBuilder> builder;
        double tick = 0;
        uint64_t trades = 0, unknown = 0, sweeps = 0;
        bool inSweep = false;
        std::optional<uint64_t> previousId;
        int64_t previousMs = 0;
        Side previousSide = Side::Unknown;
        feed.onTrade = [&](const ::Trade& t) {
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t.timestamp.time_since_epoch()).count();
            if (ms < start || ms >= end) return;
            if (!builder) {
                tick = deriveGrid(chosen->header.at("product_metadata"), t.price).nearTick;
                builder = std::make_unique<FlowBarBuilder>(60'000, tick);
            }
            const Side side = t.side == AggressorSide::Buy ? Side::Buy :
                              t.side == AggressorSide::Sell ? Side::Sell : Side::Unknown;
            if (!builder->add({t.trade_id, ms, t.price, t.size, side})) return;
            ++trades; unknown += side == Side::Unknown;
            uint64_t id = 0;
            const auto result = std::from_chars(t.trade_id.data(), t.trade_id.data() + t.trade_id.size(), id);
            const bool numeric = result.ec == std::errc{} && result.ptr == t.trade_id.data() + t.trade_id.size();
            const bool continues = numeric && previousId && *previousId != UINT64_MAX &&
                id == *previousId + 1 && ms == previousMs && side == previousSide;
            if (continues && !inSweep) ++sweeps;
            inSweep = continues;
            previousId = numeric ? std::optional<uint64_t>(id) : std::nullopt;
            previousMs = ms; previousSide = side;
        };
        JournalRecord record;
        while (reader.next(record)) {
            if (record.record.time.systemNs / 1'000'000 >= end + 300'000) break;
            feed.apply(record);
        }
        if (!builder || trades == 0) throw std::runtime_error("no trades in chosen UTC day");
        DayFile file;
        file.product = "BTC-USD"; file.dayStartMs = start; file.timeframeMs = 60'000;
        file.nativeTick = tick; file.bars = builder->bars();
        const auto bytes = encodeFlowDay(file).size();
        std::cout << "day=" << argv[2] << " tick=" << tick << " bars=" << file.bars.size()
                  << " trades=" << trades << " bytes_per_day=" << bytes
                  << " projected_GB_per_product_year=" << (bytes * 365.0 / 1e9)
                  << " unknown_share=" << (double(unknown) / trades)
                  << " sweep_groups=" << sweeps << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "flow_store_size_bench: " << e.what() << '\n';
        return 1;
    }
}
