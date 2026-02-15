// Sentinel — ScreenerDock
#include "ScreenerDock.hpp"

#include "../../core/protocol/SentinelStreamClient.hpp"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QLabel>
#include <QCheckBox>
#include <QComboBox>
#include <QSlider>
#include <QToolButton>
#include <QIcon>
#include <QTableView>
#include <QTimer>

static constexpr int kColSymbol    = 0;
static constexpr int kColName      = 1;
static constexpr int kColPrice     = 2;
static constexpr int kColChangePct = 3;
static constexpr int kColVolume    = 4;
static constexpr int kColRelVol    = 5;
static constexpr int kColMktCap    = 6;
static constexpr int kColExtra1    = 7;   // P/E (stocks) | Category (crypto)
static constexpr int kColExtra2    = 8;   // Div Yield% (stocks) | Sector (crypto)
static constexpr int kColSector    = 9;   // Sector (stocks) | — (crypto)
static constexpr int kColExchange  = 10;
static constexpr int kColCount     = 11;

ScreenerDock::ScreenerDock(QWidget* parent)
    : DockablePanel("ScreenerDock", "Screener", parent)
    , m_autoTimer(new QTimer(this))
    , m_model(new QStandardItemModel(0, kColCount, this))
{
    m_model->setHorizontalHeaderLabels({"Symbol", "Name", "Price", "Change %", "Volume", "Rel Vol", "Mkt Cap", "Category", "Sector", "—", "Exchange"});
    m_model->setSortRole(Qt::UserRole + 1);

    m_autoTimer->setSingleShot(false);
    connect(m_autoTimer, &QTimer::timeout, this, &ScreenerDock::onAutoTimer);

    buildUi();
}

ScreenerDock::~ScreenerDock() = default;

void ScreenerDock::setStreamClient(SentinelStreamClient* client) {
    if (m_client == client) return;
    if (m_client) {
        disconnect(m_client, &SentinelStreamClient::screenerUpdateReceived,
                   this, &ScreenerDock::onScreenerUpdate);
    }
    m_client = client;
    if (m_client) {
        connect(m_client, &SentinelStreamClient::screenerUpdateReceived,
                this, &ScreenerDock::onScreenerUpdate, Qt::QueuedConnection);
        setStatus("Connected — press Run or enable Auto");
    } else {
        setStatus("No stream client", true);
    }
}

void ScreenerDock::buildUi() {
    auto* layout = new QVBoxLayout(m_contentWidget);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    // ── Top toolbar ──────────────────────────────────────────────────────────
    auto* toolbar = new QHBoxLayout;
    toolbar->setSpacing(6);

    m_assetCombo = new QComboBox(m_contentWidget);
    m_assetCombo->addItem("Crypto", "crypto");
    m_assetCombo->addItem("Stocks", "stock");
    m_assetCombo->setFixedWidth(80);
    toolbar->addWidget(m_assetCombo);

    toolbar->addWidget(new QLabel("Interval:", m_contentWidget));

    m_intervalSlider = new QSlider(Qt::Horizontal, m_contentWidget);
    m_intervalSlider->setRange(30, 300);
    m_intervalSlider->setSingleStep(30);
    m_intervalSlider->setPageStep(60);
    m_intervalSlider->setValue(m_intervalSec);
    m_intervalSlider->setFixedWidth(100);
    toolbar->addWidget(m_intervalSlider);

    m_intervalLabel = new QLabel(QString("%1s").arg(m_intervalSec), m_contentWidget);
    m_intervalLabel->setFixedWidth(36);
    toolbar->addWidget(m_intervalLabel);

    m_autoCheck = new QCheckBox("Auto", m_contentWidget);
    m_autoCheck->setChecked(false);
    m_autoCheck->setIcon(QIcon(":/svg/auto-refresh.svg"));
    m_autoCheck->setToolTip("Automatically refresh at the set interval");
    toolbar->addWidget(m_autoCheck);

    m_runBtn = new QToolButton(m_contentWidget);
    m_runBtn->setIcon(QIcon(":/svg/refresh.svg"));
    m_runBtn->setToolTip("Fetch screener data once");
    m_runBtn->setFixedSize(28, 28);
    toolbar->addWidget(m_runBtn);

    toolbar->addStretch();
    layout->addLayout(toolbar);

    // ── Table ────────────────────────────────────────────────────────────────
    m_table = new QTableView(m_contentWidget);
    m_table->setModel(m_model);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setAlternatingRowColors(true);
    m_table->setSortingEnabled(true);
    m_table->verticalHeader()->hide();
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setStyleSheet(
        "QTableView { background:#1a1a1a; color:#e0e0e0; gridline-color:#2a2a2a; }"
        "QTableView::item:selected { background:#2d5a8e; }"
        "QHeaderView::section { background:#252525; color:#aaa; border:none; padding:3px; }"
    );
    layout->addWidget(m_table, 1);

    // ── Status bar ───────────────────────────────────────────────────────────
    m_statusLabel = new QLabel("Waiting for stream client...", m_contentWidget);
    m_statusLabel->setStyleSheet("color:#888; font-size:11px;");
    layout->addWidget(m_statusLabel);

    m_contentWidget->setLayout(layout);

    // Signals
    connect(m_assetCombo,     QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &ScreenerDock::onAssetChanged);
    connect(m_intervalSlider, &QSlider::valueChanged,
            this, &ScreenerDock::onIntervalChanged);
    connect(m_autoCheck,      &QCheckBox::toggled,
            this, &ScreenerDock::onAutoToggled);
    connect(m_runBtn,         &QToolButton::clicked,
            this, &ScreenerDock::onRunClicked);
    connect(m_table,          &QTableView::clicked,
            this, &ScreenerDock::onRowClicked);
}

// ── Fetch ──────────────────────────────────────────────────────────────────────

void ScreenerDock::requestFetch() {
    if (!m_client) {
        setStatus("No stream client — is the server running?", true);
        return;
    }
    setStatus(QString("Fetching %1...").arg(m_currentAsset));
    m_client->requestScreenerData(m_currentAsset.toStdString(), 100, 500000.0);
}

// ── Incoming messages ─────────────────────────────────────────────────────────

void ScreenerDock::onScreenerUpdate(const QString& asset, int rowCount, const QByteArray& rowsJson) {
    // Only apply if it matches current asset (may lag if user switched mid-fetch)
    if (asset != m_currentAsset) return;

    const QJsonArray rows = QJsonDocument::fromJson(rowsJson).array();
    applyRows(rows);
    setStatus(QString("Updated — %1 rows (%2)").arg(rowCount).arg(asset));
}

// ── Row display ───────────────────────────────────────────────────────────────

static QStandardItem* numItem(const QString& display, double sortVal) {
    auto* item = new QStandardItem(display);
    item->setData(sortVal, Qt::UserRole + 1);
    item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    return item;
}

static QStandardItem* strItem(const QString& text) {
    auto* item = new QStandardItem(text);
    item->setData(text, Qt::UserRole + 1);
    return item;
}

static QString fmtPrice(double v) {
    if (v >= 1000.0) return QString::number(v, 'f', 2);
    if (v >= 1.0)    return QString::number(v, 'f', 4);
    return QString::number(v, 'f', 6);
}

static QString fmtVolume(double v) {
    if (v >= 1e9) return QString::number(v / 1e9, 'f', 2) + "B";
    if (v >= 1e6) return QString::number(v / 1e6, 'f', 2) + "M";
    if (v >= 1e3) return QString::number(v / 1e3, 'f', 1) + "K";
    return QString::number(static_cast<qint64>(v));
}

void ScreenerDock::applyRows(const QJsonArray& rows) {
    const bool isCrypto = (m_currentAsset == "crypto");

    QStringList headers = {"Symbol", "Name", "Price", "Change %", "Volume", "Rel Vol", "Mkt Cap"};
    if (isCrypto)
        headers << "Category" << "Sector" << "—" << "Exchange";
    else
        headers << "P/E" << "Div Yield%" << "Sector" << "Exchange";
    m_model->setHorizontalHeaderLabels(headers);

    m_table->setSortingEnabled(false);
    m_model->removeRows(0, m_model->rowCount());

    const QColor upColor(47, 221, 122);
    const QColor downColor(239, 92, 85);

    for (const QJsonValue& val : rows) {
        const QJsonObject row = val.toObject();

        const double price     = row["Price"].toDouble();
        const double changePct = row["Change %"].toDouble();
        const double volume    = row["Volume"].toDouble();
        const double relVol    = row["Relative Volume"].toDouble();
        const double mktCap    = isCrypto
                                   ? row["Market Cap"].toDouble()
                                   : row["Market Capitalization"].toDouble();

        auto* symItem  = strItem(row["symbol"].toString());
        auto* nameItem = strItem(row["Name"].toString());
        symItem->setData(m_currentAsset, Qt::UserRole);

        auto* priceItem  = numItem(fmtPrice(price),                       price);
        auto* pctItem    = numItem(QString::number(changePct, 'f', 2)+"%", changePct);
        auto* volItem    = numItem(fmtVolume(volume),                      volume);
        auto* relVolItem = numItem(QString::number(relVol, 'f', 2),        relVol);
        auto* mktCapItem = numItem(fmtVolume(mktCap),                      mktCap);

        pctItem->setForeground(changePct >= 0 ? upColor : downColor);

        QStandardItem *extra1, *extra2, *extra3;
        if (isCrypto) {
            extra1 = strItem(row["Crypto Categories"].toString());
            extra2 = strItem(row["Sector"].toString());
            extra3 = strItem(QString());
        } else {
            const double pe  = row["Price to Earnings Ratio (TTM)"].toDouble();
            const double div = row["Dividend Yield % (Current)"].toDouble();
            extra1 = numItem(pe  > 0 ? QString::number(pe,  'f', 1)       : "—", pe);
            extra2 = numItem(div > 0 ? QString::number(div, 'f', 2) + "%" : "—", div);
            extra3 = strItem(row["Sector"].toString());
        }

        auto* exchItem = strItem(row["Exchange"].toString());

        m_model->appendRow({symItem, nameItem, priceItem, pctItem, volItem,
                            relVolItem, mktCapItem, extra1, extra2, extra3, exchItem});
    }

    m_table->setSortingEnabled(true);
    if (m_table->horizontalHeader()->sortIndicatorSection() < 0)
        m_table->sortByColumn(kColMktCap, Qt::DescendingOrder);

    if (!m_columnsResized) {
        m_table->resizeColumnsToContents();
        m_columnsResized = true;
    }
}

// ── UI slots ──────────────────────────────────────────────────────────────────

void ScreenerDock::onRunClicked() {
    requestFetch();
}

void ScreenerDock::onAutoToggled(bool checked) {
    m_autoEnabled = checked;
    if (checked) {
        m_autoTimer->setInterval(m_intervalSec * 1000);
        m_autoTimer->start();
        requestFetch();  // immediate fetch on enable
    } else {
        m_autoTimer->stop();
    }
}

void ScreenerDock::onAutoTimer() {
    requestFetch();
}

void ScreenerDock::onAssetChanged(int index) {
    m_currentAsset = m_assetCombo->itemData(index).toString();
    m_columnsResized = false;
    if (m_autoEnabled) requestFetch();
}

void ScreenerDock::onIntervalChanged(int value) {
    m_intervalSec = value;
    m_intervalLabel->setText(QString("%1s").arg(value));
    if (m_autoEnabled) {
        m_autoTimer->setInterval(value * 1000);
    }
}

void ScreenerDock::onRowClicked(const QModelIndex& index) {
    const int     row       = index.row();
    const QString symbol    = m_model->item(row, kColSymbol)->text();
    const QString assetType = m_model->item(row, kColSymbol)->data(Qt::UserRole).toString();
    emit rowSelected(symbol, assetType);
}

void ScreenerDock::onSymbolChanged(const QString& /*symbol*/) {
    // Screener shows the full market — doesn't filter by heatmap symbol
}

void ScreenerDock::setStatus(const QString& text, bool error) {
    if (!m_statusLabel) return;
    m_statusLabel->setText(text);
    m_statusLabel->setStyleSheet(error
        ? "color:#ef5c55; font-size:11px;"
        : "color:#888; font-size:11px;");
}
