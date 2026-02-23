// Sentinel — ScreenerDock
// Role: Displays TradingView screener data (crypto + stocks) via SentinelStreamClient.
// Threading: Runs on main GUI thread; stream client signals are queued automatically by Qt.
#pragma once

#include "DockablePanel.hpp"

#include <QStandardItemModel>
#include <QTableView>
#include <QLabel>
#include <QToolButton>
#include <QCheckBox>
#include <QComboBox>
#include <QSlider>
#include <QTimer>

class SentinelStreamClient;

class ScreenerDock : public DockablePanel {
    Q_OBJECT

public:
    explicit ScreenerDock(QWidget* parent = nullptr);
    ~ScreenerDock() override;

    void buildUi() override;
    void onSymbolChanged(const QString& symbol) override;
    QSize minimumSizeHint() const override { return {480, 320}; }

    // Called by MainWindowGpu after the stream client is created.
    void setStreamClient(SentinelStreamClient* client);

signals:
    // Emitted when the user clicks a row.
    // assetType is "crypto" or "stock" — callers use this to decide routing.
    void rowSelected(const QString& symbol, const QString& assetType);

private slots:
    void onScreenerUpdate(const QString& asset, int rowCount, const QByteArray& rowsJson);

    void onRunClicked();
    void onAutoToggled(bool checked);
    void onAssetChanged(int index);
    void onIntervalChanged(int value);
    void onRowClicked(const QModelIndex& index);

    void onAutoTimer();

private:
    void requestFetch();
    void applyRows(const QJsonArray& rows);
    void setStatus(const QString& text, bool error = false);

    // Stream client — not owned
    SentinelStreamClient* m_client = nullptr;

    // Auto-refresh timer (client-side; server does one-shot fetches per request)
    QTimer* m_autoTimer = nullptr;

    // UI
    QComboBox*   m_assetCombo     = nullptr;
    QSlider*     m_intervalSlider = nullptr;
    QLabel*      m_intervalLabel  = nullptr;
    QCheckBox*   m_autoCheck      = nullptr;
    QToolButton* m_runBtn         = nullptr;
    QTableView*  m_table          = nullptr;
    QLabel*      m_statusLabel    = nullptr;

    QStandardItemModel* m_model = nullptr;

    bool    m_autoEnabled    = false;
    bool    m_columnsResized = false;
    QString m_currentAsset   = "crypto";
    int     m_intervalSec    = 120;

    static constexpr int kReconnectMs = 5000;
};
