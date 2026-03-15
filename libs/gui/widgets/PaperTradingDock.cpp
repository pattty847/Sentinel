#include "PaperTradingDock.hpp"
#include "../render/PnlCurveItem.hpp"
#include "../datasources/IGridDataSource.hpp"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QDoubleSpinBox>
#include <QToolButton>
#include <QFrame>
#include <QSplitter>

PaperTradingDock::PaperTradingDock(QWidget* parent)
    : QDockWidget(QStringLiteral("Paper Trading"), parent) {
    setObjectName(QStringLiteral("PaperTradingDock"));
    buildUi();
}

void PaperTradingDock::setDataSource(IGridDataSource* source) {
    m_dataSource = source;
}

void PaperTradingDock::setSymbol(const QString& symbol) {
    m_symbol = symbol;
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

    addRow(0, QStringLiteral("Position:"), m_posLabel);
    addRow(1, QStringLiteral("Avg Price:"), m_avgPriceLabel);
    addRow(2, QStringLiteral("uPnL:"), m_uPnlLabel);
    addRow(3, QStringLiteral("rPnL:"), m_rPnlLabel);
    addRow(4, QStringLiteral("Total PnL:"), m_totalPnlLabel);

    layout->addWidget(posFrame);

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

// ─── Slots ──────────────────────────────────────────────────────────────────

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
    // Highlight algo orders with different color
    const bool isAlgo = !update.algoId.empty();
    const QColor rowColor = isAlgo ? QColor(20, 40, 60) : QColor(25, 25, 30);
    for (int c = 0; c < 6; ++c) {
        if (m_orderLog->item(row, c))
            m_orderLog->item(row, c)->setBackground(rowColor);
    }
    m_orderLog->item(row, 1)->setText(QString::fromUtf8(trading::toString(update.side)));
    m_orderLog->item(row, 2)->setText(QString::number(update.qty, 'f', 4));
    m_orderLog->item(row, 3)->setText(QString::number(update.filledQty, 'f', 4));
    m_orderLog->item(row, 4)->setText(update.limitPrice > 0.0
        ? QString::number(update.limitPrice, 'f', 2)
        : QString::number(update.avgPrice, 'f', 2));
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

    const auto pnlColor = [](double v) -> QString {
        return v >= 0 ? "color: #4caf50;" : "color: #f44336;";
    };

    if (m_uPnlLabel) {
        m_uPnlLabel->setStyleSheet(pnlColor(update.unrealizedPnl));
        m_uPnlLabel->setText(QString("$%1").arg(update.unrealizedPnl, 0, 'f', 2));
    }
    if (m_rPnlLabel) {
        m_rPnlLabel->setStyleSheet(pnlColor(update.realizedPnl));
        m_rPnlLabel->setText(QString("$%1").arg(update.realizedPnl, 0, 'f', 2));
    }
    const double total = update.unrealizedPnl + update.realizedPnl;
    if (m_totalPnlLabel) {
        m_totalPnlLabel->setStyleSheet(pnlColor(total) + "font-weight: bold;");
        m_totalPnlLabel->setText(QString("$%1").arg(total, 0, 'f', 2));
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
        m_algoPnlLabel->setText(QString("PnL: $%1").arg(snap.totalPnl, 0, 'f', 2));
    }
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
