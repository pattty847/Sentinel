// Sentinel — ScreenerDock
// Role: Displays TradingView screener data (crypto + stocks) via Python screener_server.py.
// Threading: Runs on main GUI thread; WebSocket signals are queued automatically by Qt.
#pragma once

#include "DockablePanel.hpp"

#include <QStandardItemModel>
#include <QTableView>
#include <QLabel>
#include <QPushButton>
#include <QComboBox>
#include <QSlider>
#include <QWebSocket>
#include <QTimer>

class ScreenerDock : public DockablePanel {
    Q_OBJECT

public:
    explicit ScreenerDock(QWidget* parent = nullptr);
    ~ScreenerDock() override;

    void buildUi() override;
    void onSymbolChanged(const QString& symbol) override;
    QSize minimumSizeHint() const override { return {480, 320}; }

signals:
    // Emitted when the user clicks a row.
    // assetType is "crypto" or "stock" — callers use this to decide routing.
    void rowSelected(const QString& symbol, const QString& assetType);

private slots:
    void onConnected();
    void onDisconnected();
    void onMessageReceived(const QString& message);
    void onSocketError(QAbstractSocket::SocketError error);

    void onRefreshClicked();
    void onAssetChanged(int index);
    void onIntervalChanged(int value);
    void onRowClicked(const QModelIndex& index);

    void connectToServer();
    void sendConfig();

private:
    void applyRows(const QJsonArray& rows);
    void setStatus(const QString& text, bool error = false);

    // WebSocket
    QWebSocket* m_ws = nullptr;
    QTimer* m_reconnectTimer = nullptr;

    // UI
    QComboBox* m_assetCombo = nullptr;   // Crypto / Stocks
    QSlider* m_intervalSlider = nullptr; // 30s – 300s
    QLabel* m_intervalLabel = nullptr;
    QPushButton* m_refreshBtn = nullptr;
    QTableView* m_table = nullptr;
    QLabel* m_statusLabel = nullptr;

    // Model — fixed columns: Symbol, Name, Price, Change, Change%, Volume, Exchange
    QStandardItemModel* m_model = nullptr;

    bool m_connected = false;
    QString m_currentAsset = "crypto";
    int m_intervalSec = 120;

    static constexpr quint16 kDefaultPort = 17200;
    static constexpr int kReconnectMs = 5000;
};
