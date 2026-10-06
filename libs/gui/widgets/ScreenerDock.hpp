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
#include <QPointer>
#include <QHash>
#include <QFont>

class SentinelStreamClient;
class QShowEvent;
class QHideEvent;

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
    // Server errors lack asset/request identity, so this warning preserves any pending fetch.
    void showServiceError(const QString& message);

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
    void onFetchTimeout();

private:
    void changeEvent(QEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void watchWindow();
    bool automaticRefreshAllowed() const;
    void updateAutoTimer();
    void requestAutomaticFetch();
    void requestFetch();
    void applyRows(const QJsonArray& rows);
    void adjustDefaultNameColumnWidth();
    void remeasureFontColumns();
    void updateColumns();
    void setStatus(const QString& text, bool error = false);

    // Stream client — not owned
    SentinelStreamClient* m_client = nullptr;

    // Auto-refresh timer (client-side; server does one-shot fetches per request)
    QTimer* m_autoTimer = nullptr;
    QTimer* m_fetchTimer = nullptr;

    // UI
    QComboBox*   m_assetCombo     = nullptr;
    QSlider*     m_intervalSlider = nullptr;
    QLabel*      m_intervalLabel  = nullptr;
    QCheckBox*   m_autoCheck      = nullptr;
    QToolButton* m_runBtn         = nullptr;
    QTableView*  m_table          = nullptr;
    QLabel*      m_statusLabel    = nullptr;

    QStandardItemModel* m_model = nullptr;
    QPointer<QWidget> m_hostWindow;

    bool    m_autoEnabled    = false;
    bool    m_exposed        = false;
    bool    m_columnsResized = false;
    bool    m_adjustingColumnWidths = false;
    bool    m_fontRefreshPending = false;
    int     m_nameColumnPreferredWidth = 0;
    QHash<int, int> m_userColumnWidths;
    QFont   m_measuredFont;
    bool    m_fetchPending   = false;
    QString m_currentAsset   = "crypto";
    QString m_lastReceived;
    int     m_intervalSec    = 120;
};
