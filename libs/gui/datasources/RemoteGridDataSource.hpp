#pragma once
#include "IGridDataSource.hpp"
#include "../../core/protocol/SentinelStreamClient.hpp"

class RemoteGridDataSource : public IGridDataSource {
    Q_OBJECT
public:
    explicit RemoteGridDataSource(const QString& host, const QString& port, QObject* parent = nullptr);

    void subscribe(const QString& symbol) override;
    void unsubscribe(const QString& symbol) override;
    
    const LiveOrderBook& getDirectLiveOrderBook(const std::string& productId) const override;
    std::vector<Trade> getRecentTrades(const std::string& productId) const override;

    void connectToServer();

private slots:
    void onSnapshotReceived(const QString& productId, const std::vector<OrderBookLevel>& bids, const std::vector<OrderBookLevel>& asks);
    void onL2UpdateReceived(const QString& productId, const std::vector<BookLevelUpdate>& updates);

private:
    SentinelStreamClient m_client;
    // We need to maintain a local LiveOrderBook replica if we want to return refs
    // Or we might change the interface to not return references? 
    // IGridDataSource::getDirectLiveOrderBook returns const ref.
    // So RemoteGridDataSource MUST maintain a local replica.
    
    mutable std::unordered_map<std::string, std::unique_ptr<LiveOrderBook>> m_replicaBooks;
};

