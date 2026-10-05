#pragma once

#include <QDockWidget>
#include <QHash>
#include <QLabel>
#include <QSet>
#include <QPointer>
#include <QString>

#include "../../core/trading/TradingTypes.hpp"
#include "../../core/trading/AlgoEngine.hpp"

class QTabWidget;
class QTableWidget;
class QDoubleSpinBox;
class QPushButton;
class QLabel;
class QGroupBox;
class QLineEdit;
class QPlainTextEdit;
class QProcess;
class QTimer;
class PnlCurveItem;
class IGridDataSource;
class MarketHealth;
struct Trade;

/**
 * PaperTradingDock — replaces the simple TradeBlotterDock.
 *
 * Tabs:
 *  1. Manual   — position info, B/S/F/C hotkeys, order log
 *  2. Algorithms — start/stop AvendellaMM, live stats
 *
 * Shared at the bottom: PnL curve + windowed selector.
 */
class PaperTradingDock : public QDockWidget {
    Q_OBJECT
public:
    explicit PaperTradingDock(QWidget* parent = nullptr);

    // Called once after construction to wire signals from the data source
    void setDataSource(IGridDataSource* source);

    // Current symbol used for manual trade actions
    void setSymbol(const QString& symbol);

public slots:
    void onTradeReceived(const Trade& trade);
    void onOrderUpdated(const trading::OrderUpdate& update);
    void onPositionUpdated(const trading::PositionUpdate& update);
    void onAlgoOrderEvent(const trading::AlgoOrderEvent& event);
    void onPnlSnapshot(const trading::PnlSnapshot& snapshot);

private slots:
    void onBuyMarketClicked();
    void onSellMarketClicked();
    void onBuyLimitClicked();
    void onSellLimitClicked();
    void onFlattenClicked();
    void onCancelAllClicked();
    void onStartAlgoClicked();
    void onStopAlgoClicked();

private:
    void buildUi();
    void refreshMarketHealth();
    QPointer<MarketHealth> m_marketHealth;
    QLabel* m_marketHealthLabel = nullptr;
    QTimer* m_healthTimer = nullptr;
    void buildManualTab(QWidget* parent);
    void buildAlgoTab(QWidget* parent);
    void buildBacktestTab(QWidget* parent);
    void buildPnlPanel(QWidget* parent);
    void resetForSymbolChange();
    void setAlgoState(const QString& text, const QString& reason, bool canStart, bool canStop);
    void updateNotional();
    void updatePricePrecision(double price);
    void sendManualCommand(trading::TradeAction action,
                           trading::OrderSide side,
                           trading::OrderType orderType,
                           bool hasPrice = false,
                           double price = 0.0);

    QString m_symbol;
    IGridDataSource* m_dataSource = nullptr;
    bool m_streamAvailable = false;
    bool m_algoActivityEligible = false;

    // Manual tab
    QLabel* m_lastPriceLabel = nullptr;
    QLabel* m_symbolLabel = nullptr;
    QLabel* m_notionalLabel = nullptr;
    QLabel* m_posLabel = nullptr;
    QLabel* m_avgPriceLabel = nullptr;
    QLabel* m_uPnlLabel = nullptr;
    QLabel* m_rPnlLabel = nullptr;
    QLabel* m_totalPnlLabel = nullptr;
    QDoubleSpinBox* m_manualQtySpin = nullptr;
    QDoubleSpinBox* m_limitPriceSpin = nullptr;
    QPushButton* m_useLastBtn = nullptr;
    double m_lastPrice = 0.0;
    bool m_limitUserOwned = false;
    QPushButton* m_buyMarketBtn = nullptr;
    QPushButton* m_sellMarketBtn = nullptr;
    QPushButton* m_buyLimitBtn = nullptr;
    QPushButton* m_sellLimitBtn = nullptr;
    QPushButton* m_flattenBtn = nullptr;
    QPushButton* m_cancelAllBtn = nullptr;
    QTableWidget* m_orderLog = nullptr;
    QHash<QString, int> m_orderIdToRow;

    // Algo tab — AvendellaMM panel
    QLabel* m_algoStatusLabel = nullptr;
    QLabel* m_algoFillCountLabel = nullptr;
    QLabel* m_algoPnlLabel = nullptr;
    QDoubleSpinBox* m_spreadSpin = nullptr;
    QDoubleSpinBox* m_orderQtySpin = nullptr;
    QDoubleSpinBox* m_maxPosSpin = nullptr;
    QPushButton* m_startBtn = nullptr;
    QPushButton* m_stopBtn = nullptr;
    QTimer* m_algoCommandTimer = nullptr;
    QString m_pendingAlgoAction;
    int m_algoFillCount = 0;
    double m_algoCumPnl = 0.0;
    QSet<QString> m_countedAlgoFillIds;

    // PnL curve
    PnlCurveItem* m_pnlCurve = nullptr;

    // Backtest tab
    QLineEdit*    m_btFilePath   = nullptr;
    QLineEdit*    m_btSymbol     = nullptr;
    QDoubleSpinBox* m_btSpread  = nullptr;
    QDoubleSpinBox* m_btQty     = nullptr;
    QDoubleSpinBox* m_btMaxPos  = nullptr;
    QPushButton*  m_btRunBtn    = nullptr;
    QPlainTextEdit* m_btOutput  = nullptr;
    QLabel*       m_btStatus    = nullptr;
    QProcess*     m_btProcess   = nullptr;
};
