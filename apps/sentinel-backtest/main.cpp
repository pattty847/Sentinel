#include "trading/AlgoBacktestAdapter.hpp"
#include "trading/AvendellaMM.hpp"
#include "trading/MarketEventSource.hpp"
#include "trading/ReplayEngine.hpp"
#include "trading/SimulationBroker.hpp"
#include "trading/TradeDrivenExecutionModel.hpp"

#include <QDateTime>
#include <QTimeZone>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>

namespace {

struct CliArgs {
    std::string inputPath;
    std::string symbol = "BTC-USD";
    trading::AlgoParams params;
    bool history = false, dump = false;
    int64_t fromMs = 0, toMs = 0;
    std::string journalRoot = trading::DefaultJournalRoot, legacyRoot = "data/market";
};

void printUsage() {
    std::cerr << "History: sentinel-backtest --product P --from UTC --to UTC [--journal ROOT] [--legacy ROOT] [--dump] (to exclusive)\n";
    std::cerr << "Usage: sentinel-backtest <trades.csv|trade_log.bin|trade_dir> [symbol] [spread_bps] [order_qty] [max_pos] [skew_bps]\n";
}

int64_t parseUtc(const std::string& value) {
    const auto text = QString::fromStdString(value);
    if (value.size() == 10) {
        const auto date = QDate::fromString(text, "yyyy-MM-dd");
        if (date.isValid() && date.toString("yyyy-MM-dd") == text)
            return QDateTime(date, QTime(0, 0), QTimeZone::UTC).toMSecsSinceEpoch();
    } else if (value.size() == 20 && value.back() == 'Z') {
        const auto time = QDateTime::fromString(text, Qt::ISODate);
        if (time.isValid() && time.toUTC().toString("yyyy-MM-ddTHH:mm:ssZ") == text)
            return time.toMSecsSinceEpoch();
    }
    throw std::invalid_argument("expected YYYY-MM-DD or YYYY-MM-DDTHH:MM:SSZ (UTC)");
}

std::string csvCell(const std::string& value) {
    std::string out = "\"";
    for (char c : value) { if (c == '"') out += '"'; out += c; }
    return out + '"';
}

std::optional<CliArgs> parseArgs(int argc, char** argv) {
    if (argc < 2) {
        printUsage();
        return std::nullopt;
    }

    CliArgs args;
    if (std::string(argv[1]).starts_with("--")) {
        args.history = true;
        bool product = false, from = false, to = false;
        for (int i = 1; i < argc; ++i) {
            const std::string flag = argv[i];
            if (flag == "--dump") { args.dump = true; continue; }
            if (i + 1 == argc) throw std::invalid_argument("missing value for " + flag);
            const std::string value = argv[++i];
            if (flag == "--product") { args.symbol = value; product = true; }
            else if (flag == "--from") { args.fromMs = parseUtc(value); from = true; }
            else if (flag == "--to") { args.toMs = parseUtc(value); to = true; }
            else if (flag == "--journal") args.journalRoot = value;
            else if (flag == "--legacy") args.legacyRoot = value;
            else if (flag == "--spread") args.params.spreadBps = std::stod(value);
            else if (flag == "--qty") args.params.orderQty = std::stod(value);
            else if (flag == "--maxpos") args.params.maxPositionQty = std::stod(value);
            else throw std::invalid_argument("unknown option " + flag);
        }
        if (!product || !from || !to || args.toMs <= args.fromMs)
            throw std::invalid_argument("--product, --from and exclusive --to are required");
        return args;
    }
    args.inputPath = argv[1];
    if (argc >= 3) {
        args.symbol = argv[2];
    }
    if (argc >= 4) {
        args.params.spreadBps = std::stod(argv[3]);
    }
    if (argc >= 5) {
        args.params.orderQty = std::stod(argv[4]);
    }
    if (argc >= 6) {
        args.params.maxPositionQty = std::stod(argv[5]);
    }
    if (argc >= 7) {
        args.params.skewBps = std::stod(argv[6]);
    }
    return args;
}

std::string formatMoney(double value) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(2) << value;
    return out.str();
}

} // namespace

int main(int argc, char** argv) try {
    auto args = parseArgs(argc, argv);
    if (!args.has_value()) {
        return 1;
    }

    if (!args->history && !std::filesystem::exists(args->inputPath)) {
        std::cerr << "Could not open trade source: " << args->inputPath << "\n";
        return 1;
    }

    std::unique_ptr<trading::IMarketEventSource> source;
    std::ifstream csvInput;
    const std::filesystem::path inputPath(args->inputPath);
    if (args->history) {
        source = trading::openTradeHistory(args->symbol, args->fromMs, args->toMs,
                                           args->journalRoot, args->legacyRoot);
    } else if (inputPath.extension() == ".csv") {
        csvInput.open(inputPath);
        if (!csvInput.is_open()) {
            std::cerr << "Could not open CSV trade file: " << args->inputPath << "\n";
            return 1;
        }
        source = std::make_unique<trading::CsvTradeEventSource>(csvInput);
    } else {
        source = std::make_unique<trading::TickBinaryTradeEventSource>(inputPath, args->symbol);
    }

    if (args->dump) {
        std::cout << "trade_id,ms,price,size,side,source\n" << std::setprecision(17);
        while (const auto event = source->next()) {
            if (!event->trade) continue;
            const auto& t = *event->trade;
            const char* side = t.side == AggressorSide::Buy ? "Buy"
                             : t.side == AggressorSide::Sell ? "Sell" : "Unknown";
            std::cout << csvCell(t.tradeId) << ',' << t.timestampMs << ',' << t.price << ','
                      << t.qty << ',' << side << ',' << source->sourceName() << '\n';
        }
        for (const auto& [down, up] : source->gaps())
            std::cout << "# gap," << down << ',' << up << '\n';
        return 0;
    }

    trading::SimulationBroker broker(
        {},
        std::make_unique<trading::TradeDrivenExecutionModel>(),
        0.0);
    trading::AlgoBacktestAdapter strategy(
        std::make_unique<trading::AvendellaMM>(),
        args->symbol,
        args->params);
    trading::BacktestConfig config;
    config.symbol = args->symbol;
    config.strategyId = strategy.id();
    trading::ReplayEngine replay;
    auto result = replay.run(*source, strategy, broker, config);

    std::cout << "strategy=" << result.summary.strategyId
              << " symbol=" << result.summary.symbol
              << " events=" << result.summary.eventCount
              << " fills=" << result.summary.fillCount
              << " realized_pnl=" << formatMoney(result.summary.realizedPnl)
              << " unrealized_pnl=" << formatMoney(result.summary.unrealizedPnl)
              << " total_pnl=" << formatMoney(result.summary.totalPnl)
              << " max_drawdown=" << formatMoney(result.summary.maxDrawdown)
              << "\n";

    for (const auto& fill : result.fillLog) {
        std::cout << "fill"
                  << " order_id=" << fill.orderId
                  << " status=" << trading::toString(fill.status)
                  << " side=" << trading::toString(fill.side)
                  << " qty=" << fill.filledQty
                  << " avg_price=" << formatMoney(fill.avgPrice)
                  << "\n";
    }

    return 0;
} catch (const std::exception& error) {
    std::cerr << "sentinel-backtest: " << error.what() << '\n';
    return 1;
}
