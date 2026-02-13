// Sentinel — ScreenerDock
#include "ScreenerDock.hpp"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QLabel>
#include <QComboBox>
#include <QSlider>
#include <QPushButton>
#include <QTableView>
#include <QTimer>
#include <QUrl>
#include <QAbstractSocket>

static constexpr int kColSymbol   = 0;
static constexpr int kColName     = 1;
static constexpr int kColPrice    = 2;
static constexpr int kColChange   = 3;
static constexpr int kColChangePct= 4;
static constexpr int kColVolume   = 5;
static constexpr int kColExchange = 6;
static constexpr int kColCount    = 7;

ScreenerDock::ScreenerDock(QWidget* parent)
    : DockablePanel("ScreenerDock", "Screener", parent)
    , m_ws(new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this))
    , m_reconnectTimer(new QTimer(this))
    , m_model(new QStandardItemModel(0, kColCount, this))
{
    m_model->setHorizontalHeaderLabels({"Symbol", "Name", "Price", "Change", "Change %", "Volume", "Exchange"});

    m_reconnectTimer->setInterval(kReconnectMs);
    m_reconnectTimer->setSingleShot(true);
    connect(m_reconnectTimer, &QTimer::timeout, this, &ScreenerDock::connectToServer);

    connect(m_ws, &QWebSocket::connected,    this, &ScreenerDock::onConnected);
    connect(m_ws, &QWebSocket::disconnected, this, &ScreenerDock::onDisconnected);
    connect(m_ws, &QWebSocket::textMessageReceived, this, &ScreenerDock::onMessageReceived);
    connect(m_ws, QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::errorOccurred),
            this, &ScreenerDock::onSocketError);

    buildUi();
    connectToServer();
}

ScreenerDock::~ScreenerDock() {
    m_ws->close();
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

    m_refreshBtn = new QPushButton("Refresh", m_contentWidget);
    m_refreshBtn->setFixedWidth(64);
    toolbar->addWidget(m_refreshBtn);

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
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->horizontalHeader()->setSectionResizeMode(kColName,     QHeaderView::Interactive);
    m_table->horizontalHeader()->setSectionResizeMode(kColSymbol,   QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(kColPrice,    QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(kColChange,   QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(kColChangePct,QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(kColVolume,   QHeaderView::ResizeToContents);
    m_table->setStyleSheet(
        "QTableView { background:#1a1a1a; color:#e0e0e0; gridline-color:#2a2a2a; }"
        "QTableView::item:selected { background:#2d5a8e; }"
        "QHeaderView::section { background:#252525; color:#aaa; border:none; padding:3px; }"
    );
    layout->addWidget(m_table, 1);

    // ── Status bar ───────────────────────────────────────────────────────────
    m_statusLabel = new QLabel("Connecting...", m_contentWidget);
    m_statusLabel->setStyleSheet("color:#888; font-size:11px;");
    layout->addWidget(m_statusLabel);

    m_contentWidget->setLayout(layout);

    // Signals
    connect(m_assetCombo,    QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &ScreenerDock::onAssetChanged);
    connect(m_intervalSlider, &QSlider::valueChanged,
            this, &ScreenerDock::onIntervalChanged);
    connect(m_refreshBtn, &QPushButton::clicked,
            this, &ScreenerDock::onRefreshClicked);
    connect(m_table, &QTableView::clicked,
            this, &ScreenerDock::onRowClicked);
}

// ── Connection ────────────────────────────────────────────────────────────────

void ScreenerDock::connectToServer() {
    if (m_ws->state() != QAbstractSocket::UnconnectedState)
        return;
    const QUrl url(QString("ws://127.0.0.1:%1/screener").arg(kDefaultPort));
    m_ws->open(url);
}

void ScreenerDock::onConnected() {
    m_connected = true;
    setStatus("Connected");
    sendConfig();
}

void ScreenerDock::onDisconnected() {
    m_connected = false;
    setStatus("Disconnected — retrying...", true);
    m_reconnectTimer->start();
}

void ScreenerDock::onSocketError(QAbstractSocket::SocketError /*error*/) {
    setStatus(QString("Server unavailable — start screener_server.py"), true);
    if (!m_reconnectTimer->isActive())
        m_reconnectTimer->start();
}

void ScreenerDock::sendConfig() {
    if (!m_connected) return;
    QJsonObject msg;
    msg["type"]         = "set_config";
    msg["asset"]        = m_currentAsset;
    msg["interval_sec"] = m_intervalSec;
    msg["limit"]        = 100;
    msg["min_volume"]   = 500000.0;
    m_ws->sendTextMessage(QJsonDocument(msg).toJson(QJsonDocument::Compact));
}

// ── Incoming messages ─────────────────────────────────────────────────────────

void ScreenerDock::onMessageReceived(const QString& message) {
    const QJsonObject obj = QJsonDocument::fromJson(message.toUtf8()).object();
    const QString type = obj["type"].toString();

    if (type == "screener_update") {
        applyRows(obj["rows"].toArray());
        setStatus(QString("Updated — %1 rows (%2)")
                  .arg(obj["row_count"].toInt())
                  .arg(obj["asset"].toString()));
    } else if (type == "status") {
        setStatus(obj["message"].toString());
    } else if (type == "error") {
        setStatus(obj["message"].toString(), true);
    }
}

void ScreenerDock::applyRows(const QJsonArray& rows) {
    // Rebuild model — rows count typically ~100-150, not a hot path
    m_model->removeRows(0, m_model->rowCount());

    for (const QJsonValue& val : rows) {
        const QJsonObject row = val.toObject();

        auto* symItem  = new QStandardItem(row["symbol"].toString());
        auto* nameItem = new QStandardItem(row["Name"].toString());

        const double price     = row["Price"].toDouble();
        const double change    = row["Change"].toDouble();
        const double changePct = row["Change %"].toDouble();
        const double volume    = row["Volume"].toDouble();
        const QString exchange = row["Exchange"].toString();

        auto* priceItem  = new QStandardItem(QString::number(price, 'f', 4));
        auto* changeItem = new QStandardItem(QString::number(change, 'f', 4));
        auto* pctItem    = new QStandardItem(QString::number(changePct, 'f', 2) + "%");
        auto* volItem    = new QStandardItem(QString::number(static_cast<qint64>(volume)));
        auto* exchItem   = new QStandardItem(exchange);

        // Color-code change columns
        const QColor upColor(47, 221, 122);    // #2fdd7a
        const QColor downColor(239, 92, 85);   // #ef5c55
        const QColor changeColor = changePct >= 0 ? upColor : downColor;
        changeItem->setForeground(changeColor);
        pctItem->setForeground(changeColor);

        // Store asset type in symbol item for routing on click
        symItem->setData(m_currentAsset, Qt::UserRole);

        // Right-align numeric columns
        for (auto* item : {priceItem, changeItem, pctItem, volItem}) {
            item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        }

        m_model->appendRow({symItem, nameItem, priceItem, changeItem, pctItem, volItem, exchItem});
    }
}

// ── UI slots ──────────────────────────────────────────────────────────────────

void ScreenerDock::onRefreshClicked() {
    if (!m_connected) {
        connectToServer();
        return;
    }
    QJsonObject msg;
    msg["type"] = "refresh";
    m_ws->sendTextMessage(QJsonDocument(msg).toJson(QJsonDocument::Compact));
}

void ScreenerDock::onAssetChanged(int index) {
    m_currentAsset = m_assetCombo->itemData(index).toString();
    if (m_connected) sendConfig();
}

void ScreenerDock::onIntervalChanged(int value) {
    m_intervalSec = value;
    m_intervalLabel->setText(QString("%1s").arg(value));
    if (m_connected) sendConfig();
}

void ScreenerDock::onRowClicked(const QModelIndex& index) {
    const int row = index.row();
    const QString symbol    = m_model->item(row, kColSymbol)->text();
    const QString assetType = m_model->item(row, kColSymbol)->data(Qt::UserRole).toString();
    emit rowSelected(symbol, assetType);
}

void ScreenerDock::onSymbolChanged(const QString& /*symbol*/) {
    // Screener doesn't filter by heatmap symbol — it shows the full market
}

void ScreenerDock::setStatus(const QString& text, bool error) {
    if (!m_statusLabel) return;
    m_statusLabel->setText(text);
    m_statusLabel->setStyleSheet(error
        ? "color:#ef5c55; font-size:11px;"
        : "color:#888; font-size:11px;");
}
