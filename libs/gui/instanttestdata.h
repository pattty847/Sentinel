// libs/gui/instanttestdata.h
#pragma once

#include <QObject>
#include <QTimer>
#include <random>
#include "dynamiclod.h"
#include "../core/tradedata.h"

class InstantTestData : public QObject {
    Q_OBJECT
    
public:
    explicit InstantTestData(QObject* parent = nullptr);
    
    // Generate multi-timeframe test data instantly
    void generateFullHistoricalSet(int64_t endTime_ms, int64_t historyDuration_ms = 86400000); // 24h default
    
    // Start real-time simulation
    void startRealTimeSimulation();
    void stopRealTimeSimulation();
    
    struct OrderBookSnapshot {
        int64_t timestamp_ms;
        std::vector<OrderBookLevel> bids;
        std::vector<OrderBookLevel> asks;
    };
    
signals:
    void orderBookUpdate(const OrderBookSnapshot& snapshot);
    void tradeExecuted(int64_t timestamp_ms, double price, double size, bool isBuy);
    
private slots:
    void generateRealtimeUpdate();
    
private:
    QTimer* m_realtimeTimer;
    std::mt19937 m_rng;
    
    // Market simulation state
    double m_currentPrice = 113700.0; // BTC-like price
    double m_volatility = 0.001; // 0.1% volatility
    int64_t m_lastTimestamp = 0;
    
    // Order book state
    std::vector<OrderBookLevel> m_currentBids;
    std::vector<OrderBookLevel> m_currentAsks;
    
    // Generate realistic order book around price
    void generateOrderBook(double centerPrice, std::vector<OrderBookLevel>& bids, 
                          std::vector<OrderBookLevel>& asks);
    
    // Generate price movement with realistic patterns
    double generateNextPrice();
    
    // Generate historical data for a specific timeframe
    void generateTimeFrameData(DynamicLOD::TimeFrame tf, int64_t startTime, int64_t endTime);
};