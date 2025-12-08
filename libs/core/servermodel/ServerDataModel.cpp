#include "ServerDataModel.hpp"
#include "SentinelLogging.hpp"

ServerDataModel::ServerDataModel(QObject* parent) : QObject(parent) {}

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
    }
    return *it->second;
}

void ServerDataModel::onTrade(const Trade& trade) {
    // TODO: Log trade to binary log
    // TODO: Update aggregates
    ensureSymbol(trade.product_id); 
}

void ServerDataModel::onLiveOrderBookUpdated(const QString& productId, const std::vector<BookDelta>& deltas) {
    std::string symbol = productId.toStdString();
    SymbolHotData& data = ensureSymbol(symbol);
    
    // In a real implementation, we would apply deltas to the LiveOrderBook here if we were maintaining it independently.
    // However, since MarketDataCore already updates its own DataCache (LiveOrderBook), 
    // we might just rely on that for now, OR we duplicate the logic here for the server model.
    // For Phase 1, we just track that updates are happening.
}

