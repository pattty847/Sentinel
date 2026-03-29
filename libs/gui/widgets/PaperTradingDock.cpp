#include "PaperTradingDock.hpp"
#include "../render/PnlCurveItem.hpp"
#include "../datasources/IGridDataSource.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProcess>
#include <QTabWidget>
#include <QTableWidget>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QDoubleSpinBox>
#include <QToolButton>
#include <QFrame>
#include <QSplitter>
#include <QUuid>

namespace {
QString formatDollars(double value) {
    return QString("$%1").arg(value, 0, 'f', 2);
}

QString pnlStyle(double value, bool emphasize = false) {
    return QString("color: %1;%2")
        .arg(value >= 0.0 ? "#4caf50" : "#f44336")
        .arg(emphasize ? "font-weight: bold;" : "");
}
}

PaperTradingDock::PaperTradingDock(QWidget* parent)
    : QDockWidget(QStringLiteral("Paper Trading"), parent) {
    setObjectName(QStringLiteral("PaperTradingDock"));
    buildUi();
}

void PaperTradingDock::setDataSource(IGridDataSource* source) {
    m_dataSource = source;
}

void PaperTradingDock::setSymbol(const QString& symbol) {
    if (m_symbol == symbol) {
        return;
    }
    m_symbol = symbol;
    resetForSymbolChange();
}

void PaperTradingDock::buildUi() {
    auto* root = new QWidget(this);
    auto* rootLayout = new QVBoxLayout(root);
    rootLayout->setContentsMargins(4, 4, 4, 4);
    rootLayout->setSpacing(4);

    // Tab bar
    auto* tabs = new QTabWidget(root);
    tabs->setTabPosition(QTabWidget::North);

    auto* manualTab = new QWidget(tabs);
    buildManualTab(manualTab);
    tabs->addTab(manualTab, QStringLiteral("Manual"));

    auto* algoTab = new QWidget(tabs);
    buildAlgoTab(algoTab);
    tabs->addTab(algoTab, QStringLiteral("Algorithms"));

    auto* backtestTab = new QWidget(tabs);
    buildBacktestTab(backtestTab);
    tabs->addTab(backtestTab, QStringLiteral("Backtest"));

    rootLayout->addWidget(tabs, 2);

    // PnL curve at the bottom (always visible)
    auto* pnlGroup = new QGroupBox(QStringLiteral("PnL Curve"), root);
    buildPnlPanel(pnlGroup);
    rootLayout->addWidget(pnlGroup, 1);

    setWidget(root);
    setMinimumWidth(280);
}

void PaperTradingDock::buildManualTab(QWidget* parent) {
    auto* layout = new QVBoxLayout(parent);
    layout->setContentsMargins(4, 4, 4, 4);

    // Position info grid
    auto* posFrame = new QFrame(parent);
    posFrame->setFrameStyle(QFrame::Box);
    posFrame->setStyleSheet("QFrame { border: 1px solid #333; background: #161820; }");
    auto* posGrid = new QGridLayout(posFrame);
    posGrid->setSpacing(4);

    auto addRow = [&](int row, const QString& label, QLabel*& valueLabel) {
        posGrid->addWidget(new QLabel(label, posFrame), row, 0);
        valueLabel = new QLabel(QStringLiteral("---"), posFrame);
        valueLabel->setStyleSheet("QLabel { color: #e0e0e0; font-family: 'Roboto Mono'; font-size: 11px; }");
        posGrid->addWidget(valueLabel, row, 1);
    };

    addRow(0, QStringLiteral("Last:"), m_lastPriceLabel);
    addRow(1, QStringLiteral("Position:"), m_posLabel);
    addRow(2, QStringLiteral("Avg Price:"), m_avgPriceLabel);
    addRow(3, QStringLiteral("uPnL:"), m_uPnlLabel);
    addRow(4, QStringLiteral("rPnL:"), m_rPnlLabel);
    addRow(5, QStringLiteral("Total PnL:"), m_totalPnlLabel);

    layout->addWidget(posFrame);

    auto* ticketFrame = new QFrame(parent);
    ticketFrame->setFrameStyle(QFrame::Box);
    ticketFrame->setStyleSheet("QFrame { border: 1px solid #333; background: #161820; }");
    auto* ticketGrid = new QGridLayout(ticketFrame);
    ticketGrid->setSpacing(4);

    ticketGrid->addWidget(new QLabel(QStringLiteral("Qty"), ticketFrame), 0, 0);
    m_manualQtySpin = new QDoubleSpinBox(ticketFrame);
    m_manualQtySpin->setRange(0.0001, 1000000.0);
    m_manualQtySpin->setDecimals(4);
    m_manualQtySpin->setValue(0.01);
    ticketGrid->addWidget(m_manualQtySpin, 0, 1);

    ticketGrid->addWidget(new QLabel(QStringLiteral("Limit"), ticketFrame), 0, 2);
    m_limitPriceSpin = new QDoubleSpinBox(ticketFrame);
    m_limitPriceSpin->setRange(0.0, 100000000.0);
    m_limitPriceSpin->setDecimals(2);
    m_limitPriceSpin->setValue(0.0);
    ticketGrid->addWidget(m_limitPriceSpin, 0, 3);

    m_buyMarketBtn = new QPushButton(QStringLiteral("Buy Mkt"), ticketFrame);
    m_sellMarketBtn = new QPushButton(QStringLiteral("Sell Mkt"), ticketFrame);
    m_buyLimitBtn = new QPushButton(QStringLiteral("Buy Limit"), ticketFrame);
    m_sellLimitBtn = new QPushButton(QStringLiteral("Sell Limit"), ticketFrame);
    m_flattenBtn = new QPushButton(QStringLiteral("Flatten"), ticketFrame);
    m_cancelAllBtn = new QPushButton(QStringLiteral("Cancel All"), ticketFrame);

    m_buyMarketBtn->setStyleSheet("QPushButton { background: #143f8f; color: white; padding: 4px 10px; }");
    m_sellMarketBtn->setStyleSheet("QPushButton { background: #7a2a1f; color: white; padding: 4px 10px; }");
    m_buyLimitBtn->setStyleSheet("QPushButton { background: #1a472a; color: #7dff9b; padding: 4px 10px; }");
    m_sellLimitBtn->setStyleSheet("QPushButton { background: #55311a; color: #ffcc80; padding: 4px 10px; }");

    ticketGrid->addWidget(m_buyMarketBtn, 1, 0);
    ticketGrid->addWidget(m_sellMarketBtn, 1, 1);
    ticketGrid->addWidget(m_buyLimitBtn, 1, 2);
    ticketGrid->addWidget(m_sellLimitBtn, 1, 3);
    ticketGrid->addWidget(m_flattenBtn, 2, 0, 1, 2);
    ticketGrid->addWidget(m_cancelAllBtn, 2, 2, 1, 2);

    layout->addWidget(ticketFrame);

    connect(m_buyMarketBtn, &QPushButton::clicked, this, &PaperTradingDock::onBuyMarketClicked);
    connect(m_sellMarketBtn, &QPushButton::clicked, this, &PaperTradingDock::onSellMarketClicked);
    connect(m_buyLimitBtn, &QPushButton::clicked, this, &PaperTradingDock::onBuyLimitClicked);
    connect(m_sellLimitBtn, &QPushButton::clicked, this, &PaperTradingDock::onSellLimitClicked);
    connect(m_flattenBtn, &QPushButton::clicked, this, &PaperTradingDock::onFlattenClicked);
    connect(m_cancelAllBtn, &QPushButton::clicked, this, &PaperTradingDock::onCancelAllClicked);

    // Order log table
    m_orderLog = new QTableWidget(parent);
    m_orderLog->setColumnCount(6);
    m_orderLog->setHorizontalHeaderLabels({
        QStringLiteral("ID"), QStringLiteral("Side"), QStringLiteral("Qty"),
        QStringLiteral("Fill"), QStringLiteral("Price"), QStringLiteral("Status")});
    m_orderLog->horizontalHeader()->setStretchLastSection(true);
    m_orderLog->horizontalHeader()->setDefaultSectionSize(55);
    m_orderLog->verticalHeader()->setVisible(false);
    m_orderLog->setMaximumHeight(120);
    m_orderLog->setStyleSheet("QTableWidget { font-size: 10px; font-family: 'Roboto Mono'; }");
    layout->addWidget(m_orderLog);

    layout->addStretch();
}

void PaperTradingDock::buildAlgoTab(QWidget* parent) {
    auto* layout = new QVBoxLayout(parent);
    layout->setContentsMargins(4, 4, 4, 4);

    auto* algoGroup = new QGroupBox(QStringLiteral("AvendellaMM"), parent);
    auto* algoLayout = new QVBoxLayout(algoGroup);

    // Status row
    auto* statusRow = new QHBoxLayout();
    m_algoStatusLabel = new QLabel(QStringLiteral("STOPPED"), algoGroup);
    m_algoStatusLabel->setStyleSheet("QLabel { color: #888; font-family: 'Roboto Mono'; font-size: 11px; }");
    m_algoFillCountLabel = new QLabel(QStringLiteral("Fills: 0"), algoGroup);
    m_algoFillCountLabel->setStyleSheet("QLabel { color: #aaa; font-size: 10px; }");
    m_algoPnlLabel = new QLabel(QStringLiteral("PnL: $0.00"), algoGroup);
    m_algoPnlLabel->setStyleSheet("QLabel { color: #aaa; font-size: 10px; }");
    statusRow->addWidget(m_algoStatusLabel);
    statusRow->addWidget(m_algoFillCountLabel);
    statusRow->addWidget(m_algoPnlLabel);
    statusRow->addStretch();
    algoLayout->addLayout(statusRow);

    // Parameters grid
    auto* paramGrid = new QGridLayout();
    paramGrid->setSpacing(4);

    auto addParam = [&](int row, const QString& label, QDoubleSpinBox*& spin,
                        double min, double max, double step, double defVal, const QString& suffix) {
        paramGrid->addWidget(new QLabel(label, algoGroup), row, 0);
        spin = new QDoubleSpinBox(algoGroup);
        spin->setRange(min, max);
        spin->setSingleStep(step);
        spin->setValue(defVal);
        spin->setSuffix(suffix);
        spin->setDecimals(4);
        paramGrid->addWidget(spin, row, 1);
    };

    addParam(0, QStringLiteral("Spread (bps):"), m_spreadSpin, 1.0, 500.0, 1.0, 10.0, QStringLiteral(" bps"));
    addParam(1, QStringLiteral("Order Qty:"),    m_orderQtySpin, 0.0001, 100.0, 0.001, 0.01, {});
    addParam(2, QStringLiteral("Max Pos:"),      m_maxPosSpin,   0.001, 100.0, 0.01,  0.1,  {});

    algoLayout->addLayout(paramGrid);

    // Start / Stop buttons
    auto* btnRow = new QHBoxLayout();
    m_startBtn = new QPushButton(QStringLiteral("▶ Start"), algoGroup);
    m_startBtn->setStyleSheet("QPushButton { background: #1a472a; color: #4caf50; border: 1px solid #4caf50; padding: 3px 8px; }");
    m_stopBtn = new QPushButton(QStringLiteral("■ Stop"), algoGroup);
    m_stopBtn->setStyleSheet("QPushButton { background: #4a1a1a; color: #f44336; border: 1px solid #f44336; padding: 3px 8px; }");
    m_stopBtn->setEnabled(false);
    btnRow->addWidget(m_startBtn);
    btnRow->addWidget(m_stopBtn);
    btnRow->addStretch();
    algoLayout->addLayout(btnRow);

    layout->addWidget(algoGroup);
    layout->addStretch();

    connect(m_startBtn, &QPushButton::clicked, this, &PaperTradingDock::onStartAlgoClicked);
    connect(m_stopBtn, &QPushButton::clicked, this, &PaperTradingDock::onStopAlgoClicked);
}

void PaperTradingDock::buildBacktestTab(QWidget* parent) {
    auto* layout = new QVBoxLayout(parent);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(6);

    // ── Trade log file picker ───────────────────────────────────────────────
    auto* fileGroup = new QGroupBox(QStringLiteral("Trade Log"), parent);
    auto* fileLayout = new QHBoxLayout(fileGroup);
    fileLayout->setSpacing(4);
    m_btFilePath = new QLineEdit(fileGroup);
    m_btFilePath->setPlaceholderText(QStringLiteral("/path/to/trade_log.bin or directory/"));
    fileLayout->addWidget(m_btFilePath, 1);
    auto* browseBtn = new QPushButton(QStringLiteral("Browse"), fileGroup);
    browseBtn->setFixedWidth(60);
    fileLayout->addWidget(browseBtn);
    layout->addWidget(fileGroup);

    // ── Parameters ─────────────────────────────────────────────────────────
    auto* paramGroup = new QGroupBox(QStringLiteral("Parameters"), parent);
    auto* paramGrid = new QGridLayout(paramGroup);
    paramGrid->setSpacing(4);

    auto addLabeledSpin = [&](int row, const QString& label, QDoubleSpinBox*& spin,
                              double min, double max, double step, double def,
                              const QString& suffix) {
        paramGrid->addWidget(new QLabel(label, paramGroup), row, 0);
        spin = new QDoubleSpinBox(paramGroup);
        spin->setRange(min, max);
        spin->setSingleStep(step);
        spin->setValue(def);
        spin->setSuffix(suffix);
        spin->setDecimals(4);
        paramGrid->addWidget(spin, row, 1);
    };

    paramGrid->addWidget(new QLabel(QStringLiteral("Symbol:"), paramGroup), 0, 0);
    m_btSymbol = new QLineEdit(QStringLiteral("BTC-USD"), paramGroup);
    paramGrid->addWidget(m_btSymbol, 0, 1);

    addLabeledSpin(1, QStringLiteral("Spread (bps):"), m_btSpread,  1.0, 500.0, 1.0, 10.0, QStringLiteral(" bps"));
    addLabeledSpin(2, QStringLiteral("Order Qty:"),    m_btQty,     0.0001, 100.0, 0.001, 0.01, {});
    addLabeledSpin(3, QStringLiteral("Max Pos:"),      m_btMaxPos,  0.001, 100.0, 0.01,  0.1,  {});
    layout->addWidget(paramGroup);

    // ── Run button + status ─────────────────────────────────────────────────
    auto* runRow = new QHBoxLayout();
    m_btRunBtn = new QPushButton(QStringLiteral("▶ Run Backtest"), parent);
    m_btRunBtn->setStyleSheet("QPushButton { background: #1a3a6a; color: #82b4ff; border: 1px solid #4478cc; padding: 4px 12px; }");
    m_btStatus = new QLabel(QStringLiteral("Select a trade log and press Run."), parent);
    m_btStatus->setStyleSheet("QLabel { color: #888; font-size: 10px; }");
    runRow->addWidget(m_btRunBtn);
    runRow->addWidget(m_btStatus, 1);
    layout->addLayout(runRow);

    // ── Output area ─────────────────────────────────────────────────────────
    m_btOutput = new QPlainTextEdit(parent);
    m_btOutput->setReadOnly(true);
    m_btOutput->setFont(QFont(QStringLiteral("Roboto Mono"), 10));
    m_btOutput->setStyleSheet(
        "QPlainTextEdit { background: #0f1218; color: #c0c8d4; border: 1px solid #222; }");
    m_btOutput->setMinimumHeight(120);
    layout->addWidget(m_btOutput, 1);

    // ── Browse handler ─────────────────────────────────────────────────────
    connect(browseBtn, &QPushButton::clicked, this, [this]() {
        const QString path = QFileDialog::getOpenFileName(
            this,
            QStringLiteral("Select Trade Log"),
            QDir::homePath(),
            QStringLiteral("Binary Trade Logs (*.bin);;CSV Trade Logs (*.csv);;All Files (*)"));
        if (!path.isEmpty()) {
            m_btFilePath->setText(path);
        }
    });

    // ── Run handler ────────────────────────────────────────────────────────
    connect(m_btRunBtn, &QPushButton::clicked, this, [this]() {
        const QString inputPath = m_btFilePath->text().trimmed();
        if (inputPath.isEmpty()) {
            m_btStatus->setText(QStringLiteral("Error: no trade log selected."));
            m_btStatus->setStyleSheet("QLabel { color: #f44336; font-size: 10px; }");
            return;
        }
        if (!QFileInfo::exists(inputPath)) {
            m_btStatus->setText(QStringLiteral("Error: file not found."));
            m_btStatus->setStyleSheet("QLabel { color: #f44336; font-size: 10px; }");
            return;
        }

        // Locate sentinel_backtest binary relative to the running application.
        const QString appDir = QCoreApplication::applicationDirPath();
        QStringList candidates;
        // Dev build layouts.
        for (const auto& rel : {
                 QString("../sentinel-backtest/sentinel-backtest"),
                 QString("../../sentinel-backtest/sentinel-backtest"),
                 QString("../../../apps/sentinel-backtest/sentinel-backtest"),
                 QString("sentinel-backtest"),
             }) {
            candidates << QDir(appDir).absoluteFilePath(rel);
        }

        QString backtestBin;
        for (const auto& c : candidates) {
            if (QFileInfo::exists(c) && QFileInfo(c).isExecutable()) {
                backtestBin = QDir::cleanPath(c);
                break;
            }
        }

        if (backtestBin.isEmpty()) {
            m_btStatus->setText(QStringLiteral("Error: sentinel_backtest binary not found."));
            m_btStatus->setStyleSheet("QLabel { color: #f44336; font-size: 10px; }");
            return;
        }

        // Kill any previous run.
        if (m_btProcess && m_btProcess->state() != QProcess::NotRunning) {
            m_btProcess->kill();
            m_btProcess->waitForFinished(500);
        }

        m_btOutput->clear();
        m_btStatus->setText(QStringLiteral("Running..."));
        m_btStatus->setStyleSheet("QLabel { color: #ffc107; font-size: 10px; }");
        m_btRunBtn->setEnabled(false);

        const QString symbol  = m_btSymbol->text().trimmed().isEmpty()
                                    ? QStringLiteral("BTC-USD")
                                    : m_btSymbol->text().trimmed();
        const QString spread  = QString::number(m_btSpread->value(), 'f', 2);
        const QString qty     = QString::number(m_btQty->value(), 'f', 4);
        const QString maxPos  = QString::number(m_btMaxPos->value(), 'f', 4);

        if (!m_btProcess) {
            m_btProcess = new QProcess(this);
            connect(m_btProcess, &QProcess::readyReadStandardOutput, this, [this]() {
                m_btOutput->appendPlainText(QString::fromLocal8Bit(m_btProcess->readAllStandardOutput()).trimmed());
            });
            connect(m_btProcess, &QProcess::readyReadStandardError, this, [this]() {
                m_btOutput->appendPlainText(QStringLiteral("[stderr] ") +
                    QString::fromLocal8Bit(m_btProcess->readAllStandardError()).trimmed());
            });
            connect(m_btProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                    this, [this](int code, QProcess::ExitStatus status) {
                m_btRunBtn->setEnabled(true);
                if (status == QProcess::CrashExit || code != 0) {
                    m_btStatus->setText(QString("Exited with code %1").arg(code));
                    m_btStatus->setStyleSheet("QLabel { color: #f44336; font-size: 10px; }");
                } else {
                    m_btStatus->setText(QStringLiteral("Done."));
                    m_btStatus->setStyleSheet("QLabel { color: #4caf50; font-size: 10px; }");
                }
            });
        }

        m_btProcess->start(backtestBin, {inputPath, symbol, spread, qty, maxPos});
    });
}

void PaperTradingDock::buildPnlPanel(QWidget* parent) {
    auto* layout = new QVBoxLayout(parent);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(2);

    // Window buttons
    auto* windowRow = new QHBoxLayout();
    auto addWinBtn = [&](const QString& label, PnlCurveItem::Window w) {
        auto* btn = new QToolButton(parent);
        btn->setText(label);
        btn->setCheckable(true);
        btn->setFixedWidth(32);
        connect(btn, &QToolButton::clicked, this, [this, w]() {
            if (m_pnlCurve) m_pnlCurve->setWindow(w);
        });
        windowRow->addWidget(btn);
    };
    addWinBtn(QStringLiteral("1m"), PnlCurveItem::Window::Min1);
    addWinBtn(QStringLiteral("5m"), PnlCurveItem::Window::Min5);
    addWinBtn(QStringLiteral("1h"), PnlCurveItem::Window::Hour1);
    addWinBtn(QStringLiteral("All"), PnlCurveItem::Window::All);
    windowRow->addStretch();
    layout->addLayout(windowRow);

    m_pnlCurve = new PnlCurveItem(parent);
    m_pnlCurve->setMinimumHeight(80);
    layout->addWidget(m_pnlCurve);
}

void PaperTradingDock::resetForSymbolChange() {
    if (m_lastPriceLabel) {
        m_lastPriceLabel->setStyleSheet("QLabel { color: #e0e0e0; font-family: 'Roboto Mono'; font-size: 11px; }");
        m_lastPriceLabel->setText(QStringLiteral("---"));
    }
    if (m_posLabel) {
        m_posLabel->setText(QStringLiteral("0.0000"));
    }
    if (m_avgPriceLabel) {
        m_avgPriceLabel->setText(QStringLiteral("---"));
    }
    if (m_uPnlLabel) {
        m_uPnlLabel->setStyleSheet("color: #e0e0e0;");
        m_uPnlLabel->setText(QStringLiteral("$0.00"));
    }
    if (m_rPnlLabel) {
        m_rPnlLabel->setStyleSheet("color: #e0e0e0;");
        m_rPnlLabel->setText(QStringLiteral("$0.00"));
    }
    if (m_totalPnlLabel) {
        m_totalPnlLabel->setStyleSheet("color: #e0e0e0; font-weight: bold;");
        m_totalPnlLabel->setText(QStringLiteral("$0.00"));
    }
    if (m_orderLog) {
        m_orderLog->setRowCount(0);
        m_orderIdToRow.clear();
    }
    if (m_pnlCurve) {
        m_pnlCurve->clear();
    }
}

// ─── Slots ──────────────────────────────────────────────────────────────────

void PaperTradingDock::onTradeReceived(const Trade& trade) {
    if (!m_symbol.isEmpty() && QString::fromStdString(trade.product_id) != m_symbol) {
        return;
    }
    if (m_lastPriceLabel) {
        const QString sideColor = trade.side == AggressorSide::Buy ? "#4caf50"
                                : trade.side == AggressorSide::Sell ? "#f44336"
                                : "#e0e0e0";
        m_lastPriceLabel->setStyleSheet(QString("QLabel { color: %1; font-family: 'Roboto Mono'; font-size: 11px; }").arg(sideColor));
        m_lastPriceLabel->setText(formatDollars(trade.price));
    }
    if (m_limitPriceSpin && !m_limitPriceSpin->hasFocus() && trade.price > 0.0) {
        m_limitPriceSpin->setValue(trade.price);
    }
}

void PaperTradingDock::onOrderUpdated(const trading::OrderUpdate& update) {
    if (!m_symbol.isEmpty() && QString::fromStdString(update.symbol) != m_symbol) {
        return;
    }

    const QString oid = QString::fromStdString(update.orderId);
    int row = m_orderIdToRow.value(oid, -1);
    if (row < 0) {
        row = m_orderLog->rowCount();
        m_orderLog->insertRow(row);
        m_orderLog->setItem(row, 0, new QTableWidgetItem(oid));
        m_orderIdToRow.insert(oid, row);
        for (int c = 1; c < 6; ++c)
            m_orderLog->setItem(row, c, new QTableWidgetItem(QString()));
    }
    const bool isAlgo = !update.algoId.empty();
    const QColor rowColor = isAlgo ? QColor(20, 40, 60) : QColor(25, 25, 30);
    QColor statusColor = QColor(210, 220, 240);
    switch (update.status) {
    case trading::OrderStatus::Filled:
        statusColor = QColor(91, 227, 155);
        break;
    case trading::OrderStatus::Partial:
    case trading::OrderStatus::Open:
    case trading::OrderStatus::New:
        statusColor = QColor(120, 170, 255);
        break;
    case trading::OrderStatus::Canceled:
        statusColor = QColor(255, 183, 94);
        break;
    case trading::OrderStatus::Rejected:
        statusColor = QColor(255, 120, 120);
        break;
    }
    for (int c = 0; c < 6; ++c) {
        if (m_orderLog->item(row, c)) {
            m_orderLog->item(row, c)->setBackground(rowColor);
            m_orderLog->item(row, c)->setForeground(statusColor);
        }
    }
    m_orderLog->item(row, 1)->setText(QString::fromUtf8(trading::toString(update.side)));
    m_orderLog->item(row, 2)->setText(QString::number(update.qty, 'f', 4));
    m_orderLog->item(row, 3)->setText(QString::number(update.filledQty, 'f', 4));
    m_orderLog->item(row, 4)->setText(update.limitPrice > 0.0
        ? formatDollars(update.limitPrice)
        : formatDollars(update.avgPrice));
    m_orderLog->item(row, 5)->setText(QString::fromUtf8(trading::toString(update.status)));
    m_orderLog->scrollToBottom();

    if (!update.algoId.empty() && update.status == trading::OrderStatus::Filled) {
        if (!m_countedAlgoFillIds.contains(oid)) {
            m_countedAlgoFillIds.insert(oid);
            ++m_algoFillCount;
            if (m_algoFillCountLabel) {
                m_algoFillCountLabel->setText(QString("Fills: %1").arg(m_algoFillCount));
            }
        }
    }
}

void PaperTradingDock::onPositionUpdated(const trading::PositionUpdate& update) {
    if (!m_symbol.isEmpty() && QString::fromStdString(update.symbol) != m_symbol) {
        return;
    }

    if (m_posLabel)
        m_posLabel->setText(QString::number(update.positionQty, 'f', 4));
    if (m_avgPriceLabel)
        m_avgPriceLabel->setText(QString("$%1").arg(update.avgPrice, 0, 'f', 2));

    if (m_uPnlLabel) {
        m_uPnlLabel->setStyleSheet(pnlStyle(update.unrealizedPnl));
        m_uPnlLabel->setText(formatDollars(update.unrealizedPnl));
    }
    if (m_rPnlLabel) {
        m_rPnlLabel->setStyleSheet(pnlStyle(update.realizedPnl));
        m_rPnlLabel->setText(formatDollars(update.realizedPnl));
    }
    const double total = update.unrealizedPnl + update.realizedPnl;
    if (m_totalPnlLabel) {
        m_totalPnlLabel->setStyleSheet(pnlStyle(total, true));
        m_totalPnlLabel->setText(formatDollars(total));
    }
}

void PaperTradingDock::onAlgoOrderEvent(const trading::AlgoOrderEvent& event) {
    if (!m_symbol.isEmpty() && QString::fromStdString(event.symbol) != m_symbol) {
        return;
    }

}

void PaperTradingDock::onPnlSnapshot(const trading::PnlSnapshot& snap) {
    if (!m_symbol.isEmpty() && QString::fromStdString(snap.symbol) != m_symbol) {
        return;
    }
    if (!m_pnlCurve) return;
    m_pnlCurve->appendPoint(snap.timestampMs, snap.totalPnl, QString::fromStdString(snap.algoId));

    // Update algo pnl label if this is an algo snapshot
    if (!snap.algoId.empty() && m_algoPnlLabel) {
        m_algoCumPnl = snap.totalPnl;
        const QString style = snap.totalPnl >= 0 ? "color: #4caf50;" : "color: #f44336;";
        m_algoPnlLabel->setStyleSheet(style);
        m_algoPnlLabel->setText(QString("PnL: %1").arg(formatDollars(snap.totalPnl)));
    }
}

void PaperTradingDock::onBuyMarketClicked() {
    sendManualCommand(trading::TradeAction::PlaceOrder, trading::OrderSide::Buy, trading::OrderType::Market);
}

void PaperTradingDock::onSellMarketClicked() {
    sendManualCommand(trading::TradeAction::PlaceOrder, trading::OrderSide::Sell, trading::OrderType::Market);
}

void PaperTradingDock::onBuyLimitClicked() {
    sendManualCommand(trading::TradeAction::PlaceOrder, trading::OrderSide::Buy, trading::OrderType::Limit, true,
                      m_limitPriceSpin ? m_limitPriceSpin->value() : 0.0);
}

void PaperTradingDock::onSellLimitClicked() {
    sendManualCommand(trading::TradeAction::PlaceOrder, trading::OrderSide::Sell, trading::OrderType::Limit, true,
                      m_limitPriceSpin ? m_limitPriceSpin->value() : 0.0);
}

void PaperTradingDock::onFlattenClicked() {
    sendManualCommand(trading::TradeAction::Flatten, trading::OrderSide::Unknown, trading::OrderType::Market);
}

void PaperTradingDock::onCancelAllClicked() {
    sendManualCommand(trading::TradeAction::CancelAll, trading::OrderSide::Unknown, trading::OrderType::Market);
}

void PaperTradingDock::onStartAlgoClicked() {
    if (!m_dataSource || m_symbol.isEmpty()) return;

    trading::AlgoParams params;
    params.spreadBps = m_spreadSpin ? m_spreadSpin->value() : 10.0;
    params.orderQty = m_orderQtySpin ? m_orderQtySpin->value() : 0.01;
    params.maxPositionQty = m_maxPosSpin ? m_maxPosSpin->value() : 0.1;

    m_dataSource->sendAlgoCommand("AvendellaMM", "start", m_symbol.toStdString(), params);

    if (m_algoStatusLabel) {
        m_algoStatusLabel->setText(QStringLiteral("RUNNING"));
        m_algoStatusLabel->setStyleSheet("QLabel { color: #4caf50; font-weight: bold; font-family: 'Roboto Mono'; font-size: 11px; }");
    }
    if (m_startBtn) m_startBtn->setEnabled(false);
    if (m_stopBtn) m_stopBtn->setEnabled(true);
    m_algoFillCount = 0;
    m_countedAlgoFillIds.clear();
    if (m_algoFillCountLabel) {
        m_algoFillCountLabel->setText(QStringLiteral("Fills: 0"));
    }
}

void PaperTradingDock::onStopAlgoClicked() {
    if (!m_dataSource) return;
    trading::AlgoParams params{};
    m_dataSource->sendAlgoCommand("AvendellaMM", "stop", m_symbol.toStdString(), params);

    if (m_algoStatusLabel) {
        m_algoStatusLabel->setText(QStringLiteral("STOPPED"));
        m_algoStatusLabel->setStyleSheet("QLabel { color: #888; font-family: 'Roboto Mono'; font-size: 11px; }");
    }
    if (m_startBtn) m_startBtn->setEnabled(true);
    if (m_stopBtn) m_stopBtn->setEnabled(false);
}

void PaperTradingDock::sendManualCommand(trading::TradeAction action,
                                         trading::OrderSide side,
                                         trading::OrderType orderType,
                                         bool hasPrice,
                                         double price) {
    if (!m_dataSource || m_symbol.isEmpty()) {
        return;
    }
    trading::TradeCommand cmd;
    cmd.commandId = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    cmd.action = action;
    cmd.symbol = m_symbol.toStdString();
    cmd.side = side;
    cmd.orderType = orderType;
    cmd.qty = m_manualQtySpin ? m_manualQtySpin->value() : 0.01;
    cmd.timestamp = QDateTime::currentMSecsSinceEpoch();
    if (hasPrice) {
        cmd.hasPrice = true;
        cmd.price = price;
    }
    m_dataSource->sendTradeCommand(cmd);
}
