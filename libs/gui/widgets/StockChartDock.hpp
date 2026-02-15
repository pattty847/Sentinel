// Sentinel — StockChartDock
// Role: Daily OHLCV candle viewer for stocks, fed by yfinance via QProcess.
// Threading: GUI thread only. QProcess stdout parsed on GUI thread.
#pragma once

#include "DockablePanel.hpp"

#include <QQuickView>
#include <QProcess>
#include <QToolButton>
#include <QLineEdit>
#include <QLabel>
#include <QButtonGroup>

class StockChartDock : public DockablePanel {
    Q_OBJECT

public:
    explicit StockChartDock(QWidget* parent = nullptr);
    ~StockChartDock() override;

    void buildUi() override;
    void onSymbolChanged(const QString& symbol) override {}
    QSize minimumSizeHint() const override { return {480, 320}; }

    // Called externally (e.g. screener stock row click)
    void loadSymbol(const QString& ticker, const QString& companyName = {});

private slots:
    void onFetchClicked();
    void onPeriodChanged(const QString& period);
    void onProcessFinished(int exitCode, QProcess::ExitStatus status);
    void onProcessError(QProcess::ProcessError error);

private:
    void startFetch();
    void setStatus(const QString& msg, bool error = false);
    QObject* qmlRoot() const;

    // UI
    QLineEdit*    m_tickerInput  = nullptr;
    QToolButton*  m_fetchBtn     = nullptr;
    QLabel*       m_statusLabel  = nullptr;
    QWidget*      m_qmlContainer = nullptr;
    QQuickView*   m_quickView    = nullptr;

    // Period buttons — kept as pointers so we can highlight active one
    QButtonGroup* m_periodGroup  = nullptr;

    // Process
    QProcess*     m_process      = nullptr;

    QString m_currentTicker;
    QString m_currentCompany;
    QString m_currentPeriod = "5y";

    static constexpr const char* kScriptRelPath = "scripts/stocks/fetch_daily_ohlcv.py";
};
