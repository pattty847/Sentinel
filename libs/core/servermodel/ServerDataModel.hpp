#pragma once
#include <unordered_map>
#include <shared_mutex>
#include <string>
#include <memory>
#include <QObject>
#include "SymbolHotData.hpp"
#include "../marketdata/model/TradeData.h"

class ServerDataModel : public QObject {
    Q_OBJECT
public:
    explicit ServerDataModel(QObject* parent = nullptr);

    SymbolHotData& ensureSymbol(const std::string& symbol);

public slots:
    void onTrade(const Trade& trade);
    void onLiveOrderBookUpdated(const QString& productId, const std::vector<BookDelta>& deltas);

private:
    std::shared_mutex m_mutex;
    std::unordered_map<std::string, std::unique_ptr<SymbolHotData>> m_symbols;
};

