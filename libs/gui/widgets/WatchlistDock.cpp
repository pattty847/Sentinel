#include "WatchlistDock.hpp"

#include <QBrush>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QLabel>
#include <QRegularExpression>
#include <QSettings>
#include <QVBoxLayout>
#include <functional>

namespace {
const QRegularExpression kCryptoPair(QStringLiteral("^[A-Z0-9]{2,20}-[A-Z0-9]{2,20}$"));
const QString kSettingsGroup = QStringLiteral("watchRail");

class WatchTreeView final : public QTreeView {
public:
    using QTreeView::QTreeView;
    std::function<void(const QModelIndex&)> keyboardActivate;
protected:
    void keyPressEvent(QKeyEvent* event) override {
        if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter ||
             event->key() == Qt::Key_Space) && currentIndex().isValid()) {
            if (keyboardActivate) keyboardActivate(currentIndex());
            event->accept();
            return;
        }
        QTreeView::keyPressEvent(event);
    }
};
}

WatchlistDock::WatchlistDock(QWidget* parent)
    : DockablePanel("WatchlistDock", "Watchlist", parent)
    , m_tree(new WatchTreeView(m_contentWidget))
    , m_model(new QStandardItemModel(this)) {
    setMaximumWidth(280);
    initPresets();
    QSettings settings(QSettings::defaultFormat(), QSettings::UserScope, "Sentinel", "SentinelTerminal");
    settings.beginGroup(kSettingsGroup);
    for (const QString& value : settings.value("pinnedSymbols").toStringList()) {
        const QString symbol = value.trimmed().toUpper();
        if (kCryptoPair.match(symbol).hasMatch() && !m_pinned.contains(symbol) && m_pinned.size() < 30)
            m_pinned.append(symbol);
    }
    const QString savedPreset = settings.value("preset", "Crypto").toString();
    settings.endGroup();

    buildUi();
    int index = m_presetCombo->findText(savedPreset);
    if (index < 0) index = 0;
    m_presetCombo->setCurrentIndex(index);
    loadPreset(index);
}

void WatchlistDock::initPresets() {
    auto make = [](const QString& name, AssetType type,
                   QVector<QPair<QString, QString>> symbols) -> WatchlistPreset {
        return {name, type, std::move(symbols)};
    };
    m_presets = {
        // Navigation suggestions from configured/captured products. They are not a recording catalog.
        make("Crypto", AssetType::Crypto, {
            {"BTC-USD", "Bitcoin"}, {"ETH-USD", "Ethereum"}, {"SOL-USD", "Solana"},
            {"DOGE-USD", "Dogecoin"}, {"PEPE-USD", "Pepe"},
            {"AVAX-USD", "Avalanche"}, {"FARTCOIN-USD", "Fartcoin"},
        }),
        make("Pinned", AssetType::Crypto, {}),
        make("Major Indices", AssetType::Stock, {
            {"SPY", "S&P 500 ETF"}, {"QQQ", "NASDAQ 100 ETF"}, {"DIA", "Dow Jones ETF"},
            {"IWM", "Russell 2000 ETF"}, {"MDY", "S&P MidCap 400 ETF"},
            {"VTI", "Total Stock Market ETF"}, {"VEA", "Developed Markets ETF"},
            {"VWO", "Emerging Markets ETF"}, {"EFA", "iShares MSCI EAFE ETF"},
            {"EEM", "iShares MSCI EM ETF"},
        }),
        make("XL Sectors", AssetType::Stock, {
            {"XLK", "Technology"}, {"XLF", "Financials"}, {"XLV", "Health Care"},
            {"XLY", "Consumer Discretionary"}, {"XLP", "Consumer Staples"},
            {"XLE", "Energy"}, {"XLI", "Industrials"}, {"XLB", "Materials"},
            {"XLRE", "Real Estate"}, {"XLU", "Utilities"}, {"XLC", "Communication Services"},
        }),
        make("Commodities", AssetType::Stock, {
            {"GLD", "Gold ETF"}, {"SLV", "Silver ETF"}, {"USO", "US Oil Fund"},
            {"UNG", "Natural Gas ETF"}, {"CORN", "Corn ETF"}, {"WEAT", "Wheat ETF"},
            {"SOYB", "Soybeans ETF"}, {"CPER", "Copper ETF"}, {"PALL", "Palladium ETF"},
            {"PPLT", "Platinum ETF"}, {"DBA", "Agri Commodity ETF"},
            {"DJP", "Bloomberg Commodity"},
        }),
        make("Top Stocks", AssetType::Stock, {
            {"AAPL", "Apple"}, {"NVDA", "NVIDIA"}, {"MSFT", "Microsoft"},
            {"AMZN", "Amazon"}, {"GOOGL", "Alphabet"}, {"META", "Meta Platforms"},
            {"TSLA", "Tesla"}, {"AVGO", "Broadcom"}, {"JPM", "JPMorgan Chase"},
            {"V", "Visa"}, {"UNH", "UnitedHealth"}, {"LLY", "Eli Lilly"},
            {"XOM", "ExxonMobil"}, {"JNJ", "Johnson & Johnson"}, {"WMT", "Walmart"},
            {"MA", "Mastercard"}, {"PG", "Procter & Gamble"}, {"HD", "Home Depot"},
            {"COST", "Costco"}, {"ORCL", "Oracle"},
        }),
        make("Fixed Income", AssetType::Stock, {
            {"TLT", "20yr Treasury ETF"}, {"IEF", "7-10yr Treasury ETF"},
            {"SHY", "1-3yr Treasury ETF"}, {"BND", "Vanguard Bond ETF"},
            {"AGG", "US Agg Bond ETF"}, {"HYG", "High Yield Corp ETF"},
            {"LQD", "Invest Grade Corp ETF"}, {"TIP", "TIPS ETF"},
            {"MBB", "Mortgage-Backed ETF"}, {"VCSH", "Short-Term Corp ETF"},
            {"VCIT", "Interm-Term Corp ETF"},
        }),
    };
}

void WatchlistDock::buildUi() {
    auto* layout = new QVBoxLayout(m_contentWidget);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    auto* top = new QHBoxLayout;
    top->setSpacing(4);
    m_presetCombo = new QComboBox(m_contentWidget);
    m_presetCombo->setObjectName("watchPreset");
    for (const auto& preset : m_presets) m_presetCombo->addItem(preset.name);
    m_presetCombo->setToolTip("Choose a crypto rail or a stock research list");
    top->addWidget(m_presetCombo, 1);
    m_pinButton = new QToolButton(m_contentWidget);
    m_pinButton->setObjectName("watchPin");
    m_pinButton->setText("Pin");
    m_pinButton->setToolTip("Pin the selected crypto pair");
    m_pinButton->setEnabled(false);
    top->addWidget(m_pinButton);
    layout->addLayout(top);

    auto* addRow = new QHBoxLayout;
    addRow->setSpacing(4);
    m_symbolEdit = new QLineEdit(m_contentWidget);
    m_symbolEdit->setObjectName("watchSymbolEntry");
    m_symbolEdit->setPlaceholderText("Add pair, e.g. BTC-USD");
    m_symbolEdit->setToolTip("Pin a crypto pair. Availability is checked when selected.");
    addRow->addWidget(m_symbolEdit, 1);
    m_addButton = new QToolButton(m_contentWidget);
    m_addButton->setObjectName("watchAdd");
    m_addButton->setText("+");
    m_addButton->setToolTip("Add pair to Pinned");
    addRow->addWidget(m_addButton);
    layout->addLayout(addRow);

    m_model->setHorizontalHeaderLabels({"Symbol", "Name"});
    m_tree->setObjectName("watchRows");
    m_tree->setModel(m_model);
    m_tree->setRootIsDecorated(false);
    m_tree->setUniformRowHeights(true);
    m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tree->setAlternatingRowColors(true);
    m_tree->setStyleSheet(
        "QTreeView { background:#1a1a1a; alternate-background-color:#202020; color:#e0e0e0; }"
        "QTreeView::item:selected { background:#3a4652; color:#fff; }"
        "QHeaderView::section { background:#252525; color:#aaa; border:none; padding:3px; }");
    m_tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_tree->header()->setStretchLastSection(true);
    layout->addWidget(m_tree, 1);

    m_status = new QLabel(m_contentWidget);
    m_status->setObjectName("watchStatus");
    m_status->setWordWrap(true);
    m_status->setStyleSheet("color:#aaa; font-size:11px;");
    layout->addWidget(m_status);

    connect(m_presetCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &WatchlistDock::onPresetChanged);
    connect(m_tree, &QTreeView::clicked, this, &WatchlistDock::onRowActivated);
    static_cast<WatchTreeView*>(m_tree)->keyboardActivate = [this](const QModelIndex& index) {
        onRowActivated(index);
    };
    connect(m_tree->selectionModel(), &QItemSelectionModel::currentChanged,
            this, [this] { updatePinButton(); });
    connect(m_addButton, &QToolButton::clicked, this, &WatchlistDock::addPinnedSymbol);
    connect(m_symbolEdit, &QLineEdit::returnPressed, this, &WatchlistDock::addPinnedSymbol);
    connect(m_pinButton, &QToolButton::clicked, this, &WatchlistDock::toggleSelectedPin);
}

void WatchlistDock::loadPreset(int index) {
    if (index < 0 || index >= m_presets.size()) return;
    const QString selected = selectedSymbol();
    m_model->removeRows(0, m_model->rowCount());
    const auto& preset = m_presets[index];
    QVector<QPair<QString, QString>> symbols = preset.symbols;
    if (index == 1) {
        for (const QString& symbol : m_pinned) symbols.append({symbol, {}});
    }
    const QString asset = preset.assetType == AssetType::Crypto ? "crypto" : "stock";
    for (const auto& entry : symbols) {
        auto* ticker = new QStandardItem(entry.first);
        ticker->setData(entry.first, TickerRole);
        ticker->setData(asset, AssetTypeRole);
        auto* name = new QStandardItem(entry.second);
        name->setForeground(QBrush(QColor("#aaa")));
        m_model->appendRow({ticker, name});
        if (entry.first == selected) m_tree->setCurrentIndex(m_model->index(m_model->rowCount() - 1, 0));
    }
    refreshRowState();
    updatePinButton();
}

void WatchlistDock::onPresetChanged(int index) {
    loadPreset(index);
    savePreferences();
}

QString WatchlistDock::selectedSymbol() const {
    const QModelIndex index = m_tree->currentIndex();
    return index.isValid() ? m_model->index(index.row(), 0).data(TickerRole).toString() : QString();
}

void WatchlistDock::onRowActivated(const QModelIndex& index) {
    if (!index.isValid()) return;
    const QModelIndex symbolIndex = m_model->index(index.row(), 0);
    const QString symbol = symbolIndex.data(TickerRole).toString();
    const QString asset = symbolIndex.data(AssetTypeRole).toString();
    if (symbol.isEmpty() || asset.isEmpty()) return;
    if (asset == "crypto") {
        if (m_catalogAuthoritative && !m_supported.contains(symbol)) {
            m_status->setText(QStringLiteral("%1 is unavailable in the current market catalog").arg(symbol));
            return;
        }
        if (symbol == m_pendingSymbol) return;
        m_pendingSymbol = symbol;
        m_refusedSymbol.clear();
        m_refusalReason.clear();
        refreshRowState();
    }
    emit symbolSelected(symbol, asset);
}

void WatchlistDock::setChartSwitchState(const QString& activeSymbol, const QString& pendingSymbol,
                                        const QString& refusedSymbol, const QString& refusalReason) {
    m_activeSymbol = activeSymbol.trimmed().toUpper();
    m_pendingSymbol = pendingSymbol.trimmed().toUpper();
    m_refusedSymbol = refusedSymbol.trimmed().toUpper();
    m_refusalReason = refusalReason;
    refreshRowState();
}

void WatchlistDock::setCryptoAvailability(const QStringList& supportedSymbols, bool authoritative,
                                          const QString& source) {
    m_catalogAuthoritative = authoritative;
    m_supported.clear();
    if (authoritative) {
        for (const QString& symbol : supportedSymbols) m_supported.insert(symbol.trimmed().toUpper());
    }
    m_catalogSource = source;
    refreshRowState();
}

void WatchlistDock::refreshRowState() {
    const bool isCrypto = m_presetCombo->currentIndex() < 2;
    for (int row = 0; row < m_model->rowCount(); ++row) {
        auto* item = m_model->item(row, 0);
        const QString symbol = item->data(TickerRole).toString();
        const bool unavailable = isCrypto && m_catalogAuthoritative && !m_supported.contains(symbol);
        const bool active = isCrypto && symbol == m_activeSymbol;
        const bool pending = isCrypto && symbol == m_pendingSymbol;
        const bool refused = isCrypto && symbol == m_refusedSymbol;
        const QString marker = pending ? QStringLiteral("… ") : refused ? QStringLiteral("! ")
                               : active ? QStringLiteral("● ") : QString();
        item->setText(marker + symbol);
        item->setForeground(QBrush(QColor(pending ? "#e6c07b" : refused ? "#ef8178"
                                         : active ? "#84d4a1" : unavailable ? "#777" : "#e0e0e0")));
        item->setToolTip(pending ? QStringLiteral("Waiting for chart confirmation")
                          : refused ? m_refusalReason
                          : unavailable ? QStringLiteral("Not in the current market catalog")
                          : active ? QStringLiteral("Active chart")
                          : isCrypto ? QStringLiteral("Availability unverified until selected") : QString());
    }
    if (m_pendingSymbol.isEmpty() && m_refusedSymbol.isEmpty()) {
        const QString provenance = m_catalogAuthoritative
            ? QStringLiteral("Catalog: %1").arg(m_catalogSource.isEmpty() ? QStringLiteral("server") : m_catalogSource)
            : QStringLiteral("Availability unknown · no quote feed");
        m_status->setText(m_activeSymbol.isEmpty() ? provenance
            : QStringLiteral("Chart: %1 · %2").arg(m_activeSymbol, provenance));
    } else if (!m_refusedSymbol.isEmpty()) {
        m_status->setText(m_refusalReason.isEmpty()
            ? QStringLiteral("%1 unavailable").arg(m_refusedSymbol) : m_refusalReason);
    } else {
        m_status->setText(QStringLiteral("Switching to %1 · waiting for confirmation").arg(m_pendingSymbol));
    }
}

void WatchlistDock::updatePinButton() {
    const QString symbol = selectedSymbol();
    const bool crypto = m_presetCombo->currentIndex() < 2;
    m_pinButton->setEnabled(crypto && !symbol.isEmpty());
    const bool pinned = m_pinned.contains(symbol);
    m_pinButton->setText(pinned ? QStringLiteral("Unpin") : QStringLiteral("Pin"));
    m_pinButton->setToolTip(pinned ? QStringLiteral("Remove selected pair from Pinned")
                                : QStringLiteral("Pin selected pair"));
}

void WatchlistDock::addPinnedSymbol() {
    const QString symbol = m_symbolEdit->text().trimmed().toUpper();
    if (!kCryptoPair.match(symbol).hasMatch()) {
        m_status->setText(QStringLiteral("Enter a pair such as BTC-USD"));
        return;
    }
    if (!m_pinned.contains(symbol)) {
        if (m_pinned.size() >= 30) {
            m_status->setText(QStringLiteral("Pinned list is full (30 pairs)"));
            return;
        }
        m_pinned.append(symbol);
        savePreferences();
    }
    m_symbolEdit->clear();
    m_presetCombo->setCurrentIndex(1);
    loadPreset(1);
    for (int row = 0; row < m_model->rowCount(); ++row) {
        if (m_model->index(row, 0).data(TickerRole).toString() == symbol) {
            m_tree->setCurrentIndex(m_model->index(row, 0));
            break;
        }
    }
    updatePinButton();
}

void WatchlistDock::toggleSelectedPin() {
    const QString symbol = selectedSymbol();
    if (symbol.isEmpty() || m_presetCombo->currentIndex() >= 2) return;
    if (m_pinned.contains(symbol)) m_pinned.removeAll(symbol);
    else if (m_pinned.size() < 30) m_pinned.append(symbol);
    savePreferences();
    if (m_presetCombo->currentIndex() == 1) loadPreset(1);
    else updatePinButton();
}

void WatchlistDock::savePreferences() {
    QSettings settings(QSettings::defaultFormat(), QSettings::UserScope, "Sentinel", "SentinelTerminal");
    settings.beginGroup(kSettingsGroup);
    settings.setValue("pinnedSymbols", m_pinned);
    settings.setValue("preset", m_presetCombo->currentText());
    settings.endGroup();
}
