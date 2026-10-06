// Sentinel — ScreenerDock
#include "ScreenerDock.hpp"

#include "../../core/protocol/SentinelStreamClient.hpp"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDateTime>
#include <QEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QLabel>
#include <QCheckBox>
#include <QComboBox>
#include <QSlider>
#include <QToolButton>
#include <QIcon>
#include <QTableView>
#include <QTimer>
#include <QSet>
#include <QScrollBar>
#include <QShowEvent>
#include <QHideEvent>
#include <QHash>
#include <functional>
#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

static constexpr int kColSymbol    = 0;
[[maybe_unused]] static constexpr int kColName      = 1;
[[maybe_unused]] static constexpr int kColPrice     = 2;
[[maybe_unused]] static constexpr int kColChangePct = 3;
[[maybe_unused]] static constexpr int kColVolume    = 4;
[[maybe_unused]] static constexpr int kColRelVol    = 5;
static constexpr int kColMktCap    = 6;
[[maybe_unused]] static constexpr int kColExtra1    = 7;   // P/E (stocks) | Category (crypto)
[[maybe_unused]] static constexpr int kColExtra2    = 8;   // Div Yield% (stocks) | Sector (crypto)
[[maybe_unused]] static constexpr int kColSector    = 9;   // Sector (stocks) | — (crypto)
[[maybe_unused]] static constexpr int kColExchange  = 10;
static constexpr int kColCount     = 11;

namespace {
class ScreenerHeaderView final : public QHeaderView {
public:
    using QHeaderView::QHeaderView;
    bool userResizeActive() const { return m_mousePressed; }
protected:
    void mousePressEvent(QMouseEvent* event) override {
        m_mousePressed = false;
        const int section = logicalIndexAt(event->pos());
        if (event->button() == Qt::LeftButton && section >= 0) {
            const int x = event->pos().x();
            const int left = sectionViewportPosition(section);
            const int right = left + sectionSize(section);
            m_mousePressed = std::abs(x - left) <= 5 || std::abs(x - right) <= 5;
        }
        QHeaderView::mousePressEvent(event);
    }
    void mouseReleaseEvent(QMouseEvent* event) override {
        QHeaderView::mouseReleaseEvent(event);
        m_mousePressed = false;
    }
    bool event(QEvent* event) override {
        if (event->type() == QEvent::UngrabMouse || event->type() == QEvent::Hide)
            m_mousePressed = false;
        return QHeaderView::event(event);
    }
private:
    bool m_mousePressed = false;
};

class ScreenerTableView final : public QTableView {
public:
    using QTableView::QTableView;
    std::function<void(const QModelIndex&)> keyboardActivate;
    int measuredColumnWidth(int column) const { return sizeHintForColumn(column); }
protected:
    void keyPressEvent(QKeyEvent* event) override {
        if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter ||
             event->key() == Qt::Key_Space) && currentIndex().isValid()) {
            if (keyboardActivate) keyboardActivate(currentIndex());
            event->accept();
            return;
        }
        QTableView::keyPressEvent(event);
    }
};
}

ScreenerDock::ScreenerDock(QWidget* parent)
    : DockablePanel("ScreenerDock", "Screener", parent)
    , m_autoTimer(new QTimer(this))
    , m_fetchTimer(new QTimer(this))
    , m_model(new QStandardItemModel(0, kColCount, this))
{
    m_model->setHorizontalHeaderLabels({"Symbol", "Name", "Price", "Change %", "Volume", "Rel Vol", "Mkt Cap", "Category", "Sector", "—", "Exchange"});
    m_model->setSortRole(Qt::UserRole + 1);

    m_autoTimer->setSingleShot(false);
    m_autoTimer->setObjectName("screenerAutoTimer");
    connect(m_autoTimer, &QTimer::timeout, this, &ScreenerDock::onAutoTimer);
    m_fetchTimer->setObjectName("screenerFetchTimeout");
    m_fetchTimer->setSingleShot(true);
    m_fetchTimer->setInterval(20000);
    connect(m_fetchTimer, &QTimer::timeout, this, &ScreenerDock::onFetchTimeout);

    buildUi();
    connect(this, &QDockWidget::visibilityChanged, this, [this](bool exposed) {
        m_exposed = exposed;
        updateAutoTimer();
    });
    installEventFilter(this);
    connect(this, &QDockWidget::topLevelChanged, this, [this] {
        watchWindow();
        updateAutoTimer();
    });
}

ScreenerDock::~ScreenerDock() = default;

void ScreenerDock::setStreamClient(SentinelStreamClient* client) {
    if (m_client == client) return;
    if (m_client) disconnect(m_client, nullptr, this, nullptr);
    m_client = client;
    m_fetchTimer->stop();
    m_fetchPending = false;
    if (m_client) {
        connect(m_client, &SentinelStreamClient::screenerUpdateReceived,
                this, &ScreenerDock::onScreenerUpdate, Qt::QueuedConnection);
        connect(m_client, &SentinelStreamClient::screenerRequestError,
                this, &ScreenerDock::showServiceError, Qt::QueuedConnection);
        connect(m_client, &SentinelStreamClient::disconnected, this, [this] {
            m_fetchTimer->stop();
            m_fetchPending = false;
            setStatus("Stream disconnected · previous screener rows may be stale", true);
        }, Qt::QueuedConnection);
        connect(m_client, &SentinelStreamClient::connected, this, [this] {
            setStatus("Stream connected · press Refresh for TradingView data");
        }, Qt::QueuedConnection);
        connect(m_client, &SentinelStreamClient::errorOccurred, this, [this](const QString& error) {
            if (m_fetchPending) {
                m_fetchTimer->stop();
                m_fetchPending = false;
                setStatus(QStringLiteral("Stream error: %1 · previous rows retained").arg(error), true);
            }
        }, Qt::QueuedConnection);
        setStatus("Stream state unknown · press Refresh to fetch");
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
    m_assetCombo->setObjectName("screenerAsset");
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
    m_autoCheck->setMinimumWidth(m_autoCheck->sizeHint().width() + 4);
    toolbar->addWidget(m_autoCheck);

    m_runBtn = new QToolButton(m_contentWidget);
    m_runBtn->setObjectName("screenerRefresh");
    m_runBtn->setIcon(QIcon(":/svg/refresh.svg"));
    m_runBtn->setToolTip("Fetch screener data once");
    m_runBtn->setFixedSize(28, 28);
    toolbar->addWidget(m_runBtn);

    toolbar->addStretch();
    layout->addLayout(toolbar);

    // ── Table ────────────────────────────────────────────────────────────────
    m_table = new ScreenerTableView(m_contentWidget);
    m_table->setObjectName("screenerRows");
    m_table->setHorizontalHeader(new ScreenerHeaderView(Qt::Horizontal, m_table));
    m_table->setModel(m_model);
    m_table->viewport()->installEventFilter(this);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setAlternatingRowColors(true);
    m_table->setSortingEnabled(true);
    m_table->sortByColumn(kColSymbol, Qt::AscendingOrder);
    m_table->setTextElideMode(Qt::ElideRight);
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
    m_statusLabel->setObjectName("screenerStatus");
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setStyleSheet("color:#888;");
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
    connect(m_table->horizontalHeader(), &QHeaderView::sectionResized, this,
            [this](int logicalIndex, int, int newSize) {
        const auto* header = static_cast<ScreenerHeaderView*>(m_table->horizontalHeader());
        if (m_adjustingColumnWidths || logicalIndex == kColExchange ||
            !header->userResizeActive() || m_table->isColumnHidden(logicalIndex)) return;
        m_userColumnWidths.insert(logicalIndex, newSize);
    });
    connect(m_table->horizontalHeader(), &QHeaderView::sectionHandleDoubleClicked, this,
            [this](int logicalIndex) {
        // Auto-fit is also an explicit user width. Capture its final size after Qt handles
        // the double-click so an earlier drag cannot be restored on the next font change.
        QTimer::singleShot(0, this, [this, logicalIndex] {
            if (logicalIndex == kColExchange || m_table->isColumnHidden(logicalIndex)) return;
            m_userColumnWidths.insert(logicalIndex, m_table->columnWidth(logicalIndex));
        });
    });
    static_cast<ScreenerTableView*>(m_table)->keyboardActivate = [this](const QModelIndex& index) {
        onRowClicked(index);
    };
    updateColumns();
    m_measuredFont = m_table->font();
}

// ── Fetch ──────────────────────────────────────────────────────────────────────

void ScreenerDock::requestFetch() {
    if (!m_client) {
        setStatus("No stream client — is the server running?", true);
        return;
    }
    m_fetchPending = true;
    m_fetchTimer->start();
    setStatus(QStringLiteral("Fetching %1 from TradingView · previous rows retained").arg(m_currentAsset));
    m_client->requestScreenerData(m_currentAsset.toStdString(), 100, 500000.0);
}

// ── Incoming messages ─────────────────────────────────────────────────────────

void ScreenerDock::onScreenerUpdate(const QString& asset, int /*rowCount*/, const QByteArray& rowsJson) {
    // Only apply if it matches current asset (may lag if user switched mid-fetch)
    if (asset != m_currentAsset) return;

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(rowsJson, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isArray()) {
        m_fetchTimer->stop();
        m_fetchPending = false;
        showServiceError(QStringLiteral("Invalid screener response"));
        return;
    }
    m_fetchTimer->stop();
    m_fetchPending = false;
    const QJsonArray rows = document.array();
    applyRows(rows);
    m_lastReceived = QDateTime::currentDateTime().toString("HH:mm:ss t");
    setStatus(QStringLiteral("TradingView via Sentinel · %1 rows · received %2 · vendor as-of unavailable")
                  .arg(rows.size()).arg(m_lastReceived));
}

void ScreenerDock::showServiceError(const QString& message) {
    const QString provenance = m_lastReceived.isEmpty()
        ? QStringLiteral("no screener rows received")
        : QStringLiteral("previous rows: TradingView via Sentinel · received %1 · vendor as-of unavailable")
              .arg(m_lastReceived);
    setStatus(QStringLiteral("Screener service error (request not identified): %1 · %2")
                  .arg(message, provenance), true);
}

void ScreenerDock::onFetchTimeout() {
    if (!m_fetchPending) return;
    m_fetchPending = false;
    setStatus(QStringLiteral("Screener timed out · previous rows retained · TradingView via Sentinel"), true);
}

// ── Row display ───────────────────────────────────────────────────────────────

static std::optional<double> number(const QJsonObject& row, const char* key) {
    const QJsonValue value = row[QLatin1String(key)];
    if (!value.isDouble()) return std::nullopt;
    const double result = value.toDouble();
    return std::isfinite(result) ? std::optional<double>(result) : std::nullopt;
}

struct CellView {
    QString text;
    QVariant sort;
    QVariant foreground;
    QString tooltip;
    bool numeric = false;
};

static CellView textCell(const QString& text, const QString& tooltip = {}) {
    return {text.isEmpty() ? QStringLiteral("—") : text, text, {}, tooltip, false};
}

static CellView numberCell(const QString& display, std::optional<double> value,
                           const QVariant& foreground = {}) {
    return {value ? display : QStringLiteral("—"),
            value ? QVariant(*value) : QVariant(), foreground, {}, true};
}

static bool updateCell(QStandardItem* item, const CellView& cell) {
    bool changed = false;
    if (item->text() != cell.text) { item->setText(cell.text); changed = true; }
    if (item->data(Qt::UserRole + 1) != cell.sort) {
        item->setData(cell.sort, Qt::UserRole + 1);
        changed = true;
    }
    if (item->data(Qt::ForegroundRole) != cell.foreground) {
        item->setData(cell.foreground, Qt::ForegroundRole);
        changed = true;
    }
    if (item->toolTip() != cell.tooltip) { item->setToolTip(cell.tooltip); changed = true; }
    const auto alignment = cell.numeric ? Qt::AlignRight | Qt::AlignVCenter : Qt::AlignLeft | Qt::AlignVCenter;
    if (item->textAlignment() != alignment) { item->setTextAlignment(alignment); changed = true; }
    return changed;
}

static QString fmtPrice(double v) {
    if (v != 0.0 && std::abs(v) < 0.000001) return QString::number(v, 'g', 6);
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
    const QModelIndex selected = m_table->currentIndex();
    const QString selectedSymbol = selected.isValid()
        ? m_model->index(selected.row(), kColSymbol).data().toString() : QString();
    const QModelIndex top = m_table->indexAt(QPoint(1, 1));
    const QString topSymbol = top.isValid()
        ? m_model->index(top.row(), kColSymbol).data().toString() : QString();
    const int oldScroll = m_table->verticalScrollBar()->value();
    const int oldHorizontalScroll = m_table->horizontalScrollBar()->value();
    QSet<QString> seen;
    QHash<QString, int> existingRows;
    for (int row = 0; row < m_model->rowCount(); ++row)
        existingRows.insert(m_model->item(row, kColSymbol)->text(), row);
    bool changed = false;

    const QColor upColor(47, 221, 122);
    const QColor downColor(239, 92, 85);

    for (const QJsonValue& val : rows) {
        if (!val.isObject()) continue;
        const QJsonObject row = val.toObject();
        const QString symbol = row["symbol"].toString().trimmed();
        if (symbol.isEmpty() || seen.contains(symbol)) continue;
        seen.insert(symbol);
        const auto price = number(row, "Price");
        const auto changePct = number(row, "Change %");
        const auto volume = number(row, "Volume");
        const auto relVol = number(row, "Relative Volume");
        const auto mktCap = number(row, isCrypto ? "Market Cap" : "Market Capitalization");

        const QVariant pctColor = changePct
            ? QVariant::fromValue(QBrush(*changePct >= 0 ? upColor : downColor)) : QVariant();
        CellView extra1, extra2, extra3;
        if (isCrypto) {
            extra1 = textCell(row["Crypto Categories"].toString());
            extra2 = textCell(row["Sector"].toString());
            extra3 = textCell(QString());
        } else {
            const auto pe = number(row, "Price to Earnings Ratio (TTM)");
            const auto div = number(row, "Dividend Yield % (Current)");
            extra1 = numberCell(pe ? QString::number(*pe, 'f', 1) : QString(), pe);
            extra2 = numberCell(div ? QString::number(*div, 'f', 2) + "%" : QString(), div);
            extra3 = textCell(row["Sector"].toString());
        }
        const std::array<CellView, kColCount> cells = {
            textCell(symbol), textCell(row["Name"].toString(), row["Name"].toString()),
            numberCell(price ? fmtPrice(*price) : QString(), price),
            numberCell(changePct ? QString::number(*changePct, 'f', 2) + "%" : QString(), changePct, pctColor),
            numberCell(volume ? fmtVolume(*volume) : QString(), volume),
            numberCell(relVol ? QString::number(*relVol, 'f', 2) : QString(), relVol),
            numberCell(mktCap ? fmtVolume(*mktCap) : QString(), mktCap),
            extra1, extra2, extra3, textCell(row["Exchange"].toString())};
        const int existingRow = existingRows.value(symbol, -1);
        if (existingRow < 0) {
            QList<QStandardItem*> newItems;
            for (const CellView& cell : cells) {
                auto* item = new QStandardItem;
                updateCell(item, cell);
                newItems.append(item);
            }
            newItems[kColSymbol]->setData(m_currentAsset, Qt::UserRole);
            m_model->appendRow(newItems);
            changed = true;
        } else {
            for (int col = 0; col < kColCount; ++col)
                changed = updateCell(m_model->item(existingRow, col), cells[col]) || changed;
        }
    }
    for (int i = m_model->rowCount() - 1; i >= 0; --i) {
        if (!seen.contains(m_model->item(i, kColSymbol)->text())) {
            m_model->removeRow(i);
            changed = true;
        }
    }
    if (!changed) return;
    const auto* header = m_table->horizontalHeader();
    const int sortColumn = header->sortIndicatorSection();
    m_model->sort(sortColumn >= 0 ? sortColumn : kColSymbol, header->sortIndicatorOrder());
    auto rowForSymbol = [this](const QString& symbol) {
        for (int row = 0; row < m_model->rowCount(); ++row)
            if (m_model->item(row, kColSymbol)->text() == symbol) return row;
        return -1;
    };
    if (!selectedSymbol.isEmpty()) {
        const int row = rowForSymbol(selectedSymbol);
        if (row >= 0) m_table->selectRow(row);
    }
    const int topRow = rowForSymbol(topSymbol);
    if (topRow >= 0) m_table->scrollTo(m_model->index(topRow, 0), QAbstractItemView::PositionAtTop);
    else m_table->verticalScrollBar()->setValue(oldScroll);

    if (!m_columnsResized) {
        m_adjustingColumnWidths = true;
        m_table->resizeColumnsToContents();
        for (auto it = m_userColumnWidths.cbegin(); it != m_userColumnWidths.cend(); ++it)
            if (!m_table->isColumnHidden(it.key())) m_table->setColumnWidth(it.key(), it.value());
        m_adjustingColumnWidths = false;
        m_columnsResized = true;
        m_nameColumnPreferredWidth = m_userColumnWidths.value(kColName, m_table->columnWidth(kColName));
    } else if (!m_userColumnWidths.contains(kColName)) {
        // Re-measure only after a changed payload batch, never on each viewport resize.
        m_nameColumnPreferredWidth = static_cast<ScreenerTableView*>(m_table)->measuredColumnWidth(kColName);
    }
    adjustDefaultNameColumnWidth();
    m_table->horizontalScrollBar()->setValue(oldHorizontalScroll);
}

void ScreenerDock::adjustDefaultNameColumnWidth() {
    if (!m_table || !m_columnsResized || m_userColumnWidths.contains(kColName) || m_model->rowCount() == 0) return;
    const int viewportWidth = m_table->viewport()->width();
    if (viewportWidth <= 0) return;

    // Name is secondary context in the compact view. Keep Symbol and Price visible at the
    // left edge, while allowing more room on wider docks. The full provider name remains in
    // DisplayRole and ToolTipRole; QTableView elides only its painted text.
    const int quoteBudget = m_table->columnWidth(kColSymbol) + m_table->columnWidth(kColPrice) +
                            m_table->columnWidth(kColChangePct) + m_table->columnWidth(kColVolume) + 8;
    const int compactNameBudget = viewportWidth * 2 / 5;
    const int availableNameBudget = viewportWidth - quoteBudget;
    const int nameHeaderWidth = m_table->horizontalHeader()->sectionSizeHint(kColName);
    const int desired = std::max(nameHeaderWidth,
        std::min(m_nameColumnPreferredWidth,
                 std::min(compactNameBudget, availableNameBudget)));
    if (desired <= 0 || desired == m_table->columnWidth(kColName)) return;

    const int horizontalOffset = m_table->horizontalScrollBar()->value();
    m_adjustingColumnWidths = true;
    m_table->setColumnWidth(kColName, desired);
    m_adjustingColumnWidths = false;
    m_table->horizontalScrollBar()->setValue(horizontalOffset);
}

void ScreenerDock::remeasureFontColumns() {
    if (!m_table || m_measuredFont == m_table->font()) return;
    m_measuredFont = m_table->font();
    if (!m_columnsResized || m_model->rowCount() == 0) return;

    const int horizontalScroll = m_table->horizontalScrollBar()->value();
    const int verticalScroll = m_table->verticalScrollBar()->value();
    m_adjustingColumnWidths = true;
    m_table->resizeColumnsToContents();
    for (auto it = m_userColumnWidths.cbegin(); it != m_userColumnWidths.cend(); ++it)
        if (!m_table->isColumnHidden(it.key())) m_table->setColumnWidth(it.key(), it.value());
    m_adjustingColumnWidths = false;
    m_nameColumnPreferredWidth = m_userColumnWidths.value(kColName, m_table->columnWidth(kColName));
    adjustDefaultNameColumnWidth();
    m_table->horizontalScrollBar()->setValue(horizontalScroll);
    m_table->verticalScrollBar()->setValue(verticalScroll);
}

void ScreenerDock::updateColumns() {
    const bool crypto = m_currentAsset == "crypto";
    m_model->setHorizontalHeaderLabels(crypto
        ? QStringList{"Symbol", "Name", "Price", "Change %", "Volume", "Rel Vol", "Mkt Cap",
                      "Category", "Sector", "—", "Exchange"}
        : QStringList{"Symbol", "Name", "Price", "Change %", "Volume", "Rel Vol", "Mkt Cap",
                      "P/E", "Div Yield%", "Sector", "Exchange"});
    // The default crypto view keeps just the fields that can identify and compare pairs.
    m_adjustingColumnWidths = true;
    for (int col = 0; col < kColCount; ++col)
        m_table->setColumnHidden(col, crypto && (col == kColRelVol || col == kColMktCap ||
                                col == kColExtra1 || col == kColExtra2 || col == kColSector));
    for (auto it = m_userColumnWidths.cbegin(); it != m_userColumnWidths.cend(); ++it)
        if (!m_table->isColumnHidden(it.key())) m_table->setColumnWidth(it.key(), it.value());
    m_adjustingColumnWidths = false;
}

// ── UI slots ──────────────────────────────────────────────────────────────────

void ScreenerDock::onRunClicked() {
    requestFetch();
}

void ScreenerDock::onAutoToggled(bool checked) {
    m_autoEnabled = checked;
    updateAutoTimer();
}

bool ScreenerDock::automaticRefreshAllowed() const {
    return m_autoEnabled && m_exposed && isVisible() && !window()->isMinimized() &&
           (!m_hostWindow || !m_hostWindow->isMinimized());
}

void ScreenerDock::requestAutomaticFetch() {
    if (automaticRefreshAllowed() && !m_fetchPending) requestFetch();
}

void ScreenerDock::updateAutoTimer() {
    if (!automaticRefreshAllowed()) {
        m_autoTimer->stop();
    } else if (!m_autoTimer->isActive()) {
        m_autoTimer->start(m_intervalSec * 1000);
        requestAutomaticFetch();
    }
}

void ScreenerDock::onAutoTimer() {
    requestAutomaticFetch();
}

void ScreenerDock::watchWindow() {
    QWidget* host = parentWidget() ? parentWidget()->window() : window();
    if (host == m_hostWindow) return;
    if (m_hostWindow && m_hostWindow != this) m_hostWindow->removeEventFilter(this);
    m_hostWindow = host;
    if (host && host != this) host->installEventFilter(this);
}

bool ScreenerDock::eventFilter(QObject* watched, QEvent* event) {
    if (m_table && watched == m_table->viewport() && event->type() == QEvent::Resize)
        adjustDefaultNameColumnWidth();
    if ((watched == this || watched == m_hostWindow) && event->type() == QEvent::WindowStateChange)
        updateAutoTimer();
    return DockablePanel::eventFilter(watched, event);
}

void ScreenerDock::changeEvent(QEvent* event) {
    DockablePanel::changeEvent(event);
    if (event->type() != QEvent::ApplicationFontChange && event->type() != QEvent::FontChange) return;
    if (m_fontRefreshPending) return;
    m_fontRefreshPending = true;
    QTimer::singleShot(0, this, [this] {
        m_fontRefreshPending = false;
        remeasureFontColumns();
    });
}

void ScreenerDock::showEvent(QShowEvent* event) {
    DockablePanel::showEvent(event);
    watchWindow();
    updateAutoTimer();
}

void ScreenerDock::hideEvent(QHideEvent* event) {
    m_exposed = false;
    updateAutoTimer();
    DockablePanel::hideEvent(event);
}

void ScreenerDock::onAssetChanged(int index) {
    m_currentAsset = m_assetCombo->itemData(index).toString();
    m_fetchTimer->stop();
    m_fetchPending = false;
    m_lastReceived.clear();
    m_model->removeRows(0, m_model->rowCount());
    m_table->clearSelection();
    m_columnsResized = false;
    updateColumns();
    if (automaticRefreshAllowed()) requestAutomaticFetch();
    else setStatus(QStringLiteral("%1 · press Refresh for TradingView data")
                       .arg(m_currentAsset == "crypto" ? QStringLiteral("Crypto") : QStringLiteral("Stocks")));
}

void ScreenerDock::onIntervalChanged(int value) {
    m_intervalSec = value;
    m_intervalLabel->setText(QString("%1s").arg(value));
    if (m_autoEnabled) {
        m_autoTimer->setInterval(value * 1000);
    }
}

void ScreenerDock::onRowClicked(const QModelIndex& index) {
    if (!index.isValid()) return;
    const int     row       = index.row();
    const auto* item = m_model->item(row, kColSymbol);
    if (!item) return;
    const QString symbol = item->text();
    const QString assetType = item->data(Qt::UserRole).toString();
    if (!symbol.isEmpty() && !assetType.isEmpty()) emit rowSelected(symbol, assetType);
}

void ScreenerDock::onSymbolChanged(const QString& /*symbol*/) {
    // Screener shows the full market — doesn't filter by heatmap symbol
}

void ScreenerDock::setStatus(const QString& text, bool error) {
    if (!m_statusLabel) return;
    m_statusLabel->setText(text);
    m_statusLabel->setStyleSheet(error
        ? "color:#ef5c55;"
        : "color:#888;");
}
