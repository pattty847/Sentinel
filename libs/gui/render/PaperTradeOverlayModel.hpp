#pragma once

#include <QObject>
#include <QVariantList>
#include <QVariantMap>
#include <QString>
#include <QPointer>
#include <QtQml/qqmlregistration.h>

#include "../../core/marketdata/model/TradeData.h"
#include "../../core/trading/TradingTypes.hpp"
#include "ITimeAxisMappingProvider.hpp"

#include <unordered_map>
#include <string>

class PaperTradeOverlayModel : public QObject {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(QString symbol READ symbol WRITE setSymbol NOTIFY symbolChanged)
    Q_PROPERTY(QObject* mappingProvider READ mappingProvider WRITE setMappingProvider NOTIFY mappingProviderChanged)
    Q_PROPERTY(QVariantList openOrders READ openOrders NOTIFY openOrdersChanged)
    Q_PROPERTY(QVariantMap activePosition READ activePosition NOTIFY activePositionChanged)

public:
    explicit PaperTradeOverlayModel(QObject* parent = nullptr);

    QString symbol() const { return m_symbol; }
    void setSymbol(const QString& symbol);
    QObject* mappingProvider() const { return m_mappingProviderObject; }
    void setMappingProvider(QObject* provider);

    QVariantList openOrders() const;
    QVariantMap activePosition() const;

public slots:
    void onTradeReceived(const Trade& trade);
    void onOrderUpdated(const trading::OrderUpdate& update);
    void onPositionUpdated(const trading::PositionUpdate& update);
    Q_INVOKABLE void logPositionOverlaySample(double displayedEntryY,
                                              double displayedMarkY,
                                              const QString& source);
    Q_INVOKABLE void logOrderOverlaySample(const QString& orderId,
                                           double displayedY,
                                           const QString& source);

signals:
    void symbolChanged();
    void mappingProviderChanged();
    void openOrdersChanged();
    void activePositionChanged();

private:
    struct ManualOrderState {
        std::string orderId;
        trading::OrderSide side = trading::OrderSide::Unknown;
        double qty = 0.0;
        double filledQty = 0.0;
        double price = 0.0;
        trading::OrderStatus status = trading::OrderStatus::New;
    };

    QString m_symbol;
    QPointer<QObject> m_mappingProviderObject;
    ITimeAxisMappingProvider* m_mappingProvider = nullptr;
    std::unordered_map<std::string, ManualOrderState> m_orders;
    trading::PositionUpdate m_position;
    bool m_hasPosition = false;
    double m_lastTradePrice = 0.0;
    bool m_hasLastTrade = false;
    qint64 m_lastPositionOverlayLogMs = 0;
    qint64 m_lastOrderOverlayLogMs = 0;

    static qint64 nowMs();
    bool shouldLogOverlaySample(qint64& lastLogMs) const;
    void appendDebugLog(const char* hypothesisId,
                        const char* message,
                        const QString& dataJson) const;
    QString currentViewportJson() const;
    QString currentMappingJson() const;
};
