#include "trading/AlgoBacktestAdapter.hpp"
#include "trading/AvendellaMM.hpp"
#include "trading/MarketEventSource.hpp"
#include "trading/ReplayEngine.hpp"
#include "trading/SimulationBroker.hpp"
#include "trading/TradeDrivenExecutionModel.hpp"

#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>

namespace {

struct CliArgs {
    std::string csvPath;
    std::string symbol = "BTC-USD";
    trading::AlgoParams params;
};

void printUsage() {
    std::cerr << "Usage: sentinel_backtest <trades.csv> [symbol] [spread_bps] [order_qty] [max_pos] [skew_bps]\n";
}

std::optional<CliArgs> parseArgs(int argc, char** argv) {
    if (argc < 2) {
        printUsage();
        return std::nullopt;
    }

    CliArgs args;
    args.csvPath = argv[1];
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

int main(int argc, char** argv) {
    auto args = parseArgs(argc, argv);
    if (!args.has_value()) {
        return 1;
    }

    std::ifstream input(args->csvPath);
    if (!input.is_open()) {
        std::cerr << "Could not open trade file: " << args->csvPath << "\n";
        return 1;
    }

    trading::CsvTradeEventSource source(input);
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
    auto result = replay.run(source, strategy, broker, config);

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
}
