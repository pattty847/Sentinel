#include "legacy_v2_fixture.hpp"
#include "capture/CaptureRouting.hpp"
#include <QUuid>
#include <algorithm>
#include <stdexcept>

namespace sentinel::capture {
std::unique_ptr<Writer> LegacyV2FixtureWriter::make(WriterConfig config, nlohmann::json metadata) {
    return std::unique_ptr<Writer>(new Writer(std::move(config), std::move(metadata), true));
}

namespace {
// The removed writer's range cadence: a new range per connection, per UTC hour,
// after RoutingIntervalNs or after MaxRoutingFrames frames.
bool due(const RoutingBatch& batch, const Record& r) {
    return batch.count && (batch.count >= MaxRoutingFrames || r.connection != batch.connection ||
        r.time.steadyNs - batch.first.steadyNs >= RoutingIntervalNs ||
        r.time.systemNs / 3600000000000LL != batch.first.systemNs / 3600000000000LL);
}
} // namespace

void writeLegacyV2(std::vector<ProductCapture> products, const std::vector<Record>& records) {
    if (products.size() < 2 || products.size() > MaxProducts) throw std::runtime_error("v2 fixture needs 2..32 products");
    std::sort(products.begin(), products.end(), [](const auto& a, const auto& b) { return a.config.symbol < b.config.symbol; });
    std::vector<std::string> symbols;
    for (const auto& product : products) {
        validateSymbol(product.config.symbol);
        if (!symbols.empty() && symbols.back() == product.config.symbol) throw std::runtime_error("duplicate capture product");
        symbols.push_back(product.config.symbol);
    }
    const auto run = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    const auto started = Stamp::now().systemNs;
    std::vector<std::unique_ptr<Writer>> writers;
    for (auto& product : products) {
        product.metadata["run_id"] = run;
        product.metadata["run_started_system_ns"] = started;
        product.metadata["connection_products"] = symbols;
        product.metadata["routing"] = RoutingId;
        writers.push_back(LegacyV2FixtureWriter::make(std::move(product.config), std::move(product.metadata)));
    }
    const auto each = [&](const Record& record) { for (auto& writer : writers) writer->append(record); };
    RoutingBatch batch;
    std::optional<uint64_t> expectedSequence;
    const auto finishBatch = [&] {
        if (batch.empty()) return;
        for (size_t i = 0; i < writers.size(); ++i)
            writers[i]->append({Kind::FrameReference, batch.last, batch.connection, batch.receipt(symbols[i]).dump()});
        batch.clear();
    };
    Record terminal{Kind::CaptureStopped, Stamp::now(), 0, R"({"reason":"session closed"})"};
    for (const auto& record : records) {
        if (record.kind == Kind::CaptureStopped) { terminal = record; continue; }
        terminal.connection = record.connection;
        if (record.kind == Kind::TransportUp) expectedSequence = 0;
        if (record.kind != Kind::Frame) { finishBatch(); each(record); continue; }
        if (due(batch, record)) finishBatch();
        const auto identity = frameReceipt(record.payload, symbols);
        const auto& sequence = identity.at("sequence_num");
        if (!sequence.is_number_unsigned() || (expectedSequence && sequence.get<uint64_t>() != *expectedSequence)) {
            finishBatch();
            each({Kind::BookInvalidated, record.time, record.connection, R"({"reason":"capture connection sequence gap"})"});
        }
        expectedSequence = sequence.is_number_unsigned() && sequence.get<uint64_t>() != UINT64_MAX ?
            std::optional<uint64_t>(sequence.get<uint64_t>() + 1) : std::nullopt;
        const auto targets = identity.at("products").get<std::vector<std::string>>();
        batch.add(record, identity);
        for (size_t i = 0; i < writers.size(); ++i)
            if (std::binary_search(targets.begin(), targets.end(), symbols[i])) writers[i]->append(record);
    }
    finishBatch();
    each(terminal);
    for (auto& writer : writers) writer->close();
}
} // namespace sentinel::capture
