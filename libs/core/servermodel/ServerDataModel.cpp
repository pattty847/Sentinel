#include "ServerDataModel.hpp"
#include "SentinelLogging.hpp"

ServerDataModel::ServerDataModel(QObject* parent) 
    : QObject(parent)
    , m_logger(std::make_unique<TickBinaryLogger>())
    , m_aggregator(std::make_unique<TimeframeAggregator>())
    , m_heatmapStreamer(std::make_unique<HeatmapTwapStreamer>(*this))
{
    // Forward aggregation signals
    connect(m_aggregator.get(), &TimeframeAggregator::barClosed, this, &ServerDataModel::barClosed);
    connect(m_aggregator.get(), &TimeframeAggregator::barUpdated, this, &ServerDataModel::barUpdated);

    connect(m_heatmapStreamer.get(), &HeatmapTwapStreamer::heatmapSliceReady,
            this, &ServerDataModel::heatmapSliceReady);

    m_heatmapStreamer->start();
}

ServerDataModel::~ServerDataModel() {
    // Unique ptr handles cleanup
}

SymbolHotData& ServerDataModel::ensureSymbol(const std::string& symbol) {
    // Upgradable lock pattern
    {
        std::shared_lock lock(m_mutex);
        auto it = m_symbols.find(symbol);
        if (it != m_symbols.end()) {
            return *it->second;
        }
    }
    
    std::unique_lock lock(m_mutex);
    auto [it, inserted] = m_symbols.try_emplace(symbol, std::make_unique<SymbolHotData>(symbol));
    if (inserted) {
        sLog_Data("ServerDataModel: New symbol tracked: " << symbol);
        // Default initialization if not yet initialized by Snapshot
        // This ensures we don't crash, but ideally onLiveOrderBookInitialized is called first or soon.
        if (symbol == "BTC-USD") {
             it->second->liveBook.initialize(75000.0, 125000.0, 0.01);
        } else {
             it->second->liveBook.initialize(75000.0, 125000.0, 0.01);
        }
    }
    return *it->second;
}

std::vector<std::string> ServerDataModel::getSymbolsSnapshot() const {
    std::shared_lock lock(m_mutex);
    std::vector<std::string> symbols;
    symbols.reserve(m_symbols.size());
    for (const auto& [key, _] : m_symbols) {
        symbols.push_back(key);
    }
    return symbols;
}

const LiveOrderBook& ServerDataModel::getLiveOrderBook(const std::string& symbol) {
    return ensureSymbol(symbol).liveBook;
}

std::vector<OHLCVBar> ServerDataModel::getHistory(const std::string& symbol, Timeframe tf, size_t limit) const {
    if (m_aggregator) {
        return m_aggregator->getHistory(symbol, tf, limit);
    }
    return {};
}

void ServerDataModel::onTrade(const Trade& trade) {
    if (m_logger) {
        m_logger->logTrade(trade);
    }
    
    // Update aggregates
    if (m_aggregator) {
        m_aggregator->onTrade(trade);
    }
    
    auto& data = ensureSymbol(trade.product_id);
    data.lastTradePrice = trade.price;
    
    // Rebroadcast to streamers
    emit tradeBroadcast(trade);
}

void ServerDataModel::onLiveOrderBookUpdated(const QString& productId, const std::vector<BookDelta>& deltas) {
    std::string symbol = productId.toStdString();
    SymbolHotData& data = ensureSymbol(symbol);
    
    // Apply deltas to our local LiveOrderBook replica
    // We need to convert indices back to prices to use the public applyUpdates API
    // This is slightly inefficient (idx -> price -> idx) but keeps LiveOrderBook encapsulation intact.
    std::vector<BookLevelUpdate> updates;
    updates.reserve(deltas.size());
    
    auto now = std::chrono::system_clock::now();
    
    for (const auto& d : deltas) {
        double price = data.liveBook.index_to_price(d.idx);
        updates.push_back({d.isBid, price, d.qty});
    }
    
    // We don't need the output deltas, we just want to update the state
    data.liveBook.applyUpdates(updates, now, nullptr);
    
    // Persistence
    if (m_logger) {
        m_logger->logBookUpdate(symbol, deltas);
    }
    
    // Rebroadcast to streamers
    emit bookUpdateBroadcast(productId, deltas);
}

void ServerDataModel::onLiveOrderBookInitialized(const QString& productId, const std::vector<OrderBookLevel>& bids, const std::vector<OrderBookLevel>& asks) {
    std::string symbol = productId.toStdString();
    SymbolHotData& data = ensureSymbol(symbol);
    
    // Re-initialize the book to clear old state
    // TODO: Unify this logic with DataCache to avoid drift
    if (symbol == "BTC-USD") {
        data.liveBook.initialize(75000.0, 125000.0, 0.01);
    } else {
        data.liveBook.initialize(75000.0, 125000.0, 0.01);
    }
    
    std::vector<BookLevelUpdate> updates;
    updates.reserve(bids.size() + asks.size());
    
    for (const auto& level : bids) {
        updates.push_back({true, level.price, level.size});
    }
    for (const auto& level : asks) {
        updates.push_back({false, level.price, level.size});
    }
    
    auto now = std::chrono::system_clock::now();
    data.liveBook.applyUpdates(updates, now, nullptr);
    
    sLog_Data(QString("ServerDataModel: Initialized book for %1 with %2 bids, %3 asks")
              .arg(productId).arg(bids.size()).arg(asks.size()));
}
