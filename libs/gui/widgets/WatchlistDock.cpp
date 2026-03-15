#include "WatchlistDock.hpp"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QBrush>
#include <QColor>
#include <QFont>

// Named constants for section-header badge rendering
namespace {
constexpr const char* kCryptoBadgeText  = "● CRYPTO";
constexpr const char* kStockBadgeText   = "● STOCKS";
constexpr const char* kCryptoBadgeColor = "#F7931A";
constexpr const char* kStockBadgeColor  = "#4CAF50";
} // namespace

WatchlistDock::WatchlistDock(QWidget* parent)
    : DockablePanel("WatchlistDock", "Watchlist", parent)
    , m_tree(new QTreeView(m_contentWidget))
    , m_model(new QStandardItemModel(this))
{
    initPresets();
    buildUi();
    if (!m_presets.isEmpty()) {
        loadPreset(0);
    }
}

QSize WatchlistDock::minimumSizeHint() const {
    return QSize(320, 400);
}

void WatchlistDock::initPresets() {
    // Helper: build a preset inline without a named local variable
    auto make = [](const QString& name, AssetType type,
                   QVector<QPair<QString, QString>> symbols) -> WatchlistPreset {
        return {name, type, std::move(symbols)};
    };

    m_presets = {
        // ── Major Indices ────────────────────────────────────────────────────
        make("Major Indices", AssetType::Stock, {
            {"SPY",  "S&P 500 ETF"},
            {"QQQ",  "NASDAQ 100 ETF"},
            {"DIA",  "Dow Jones ETF"},
            {"IWM",  "Russell 2000 ETF"},
            {"MDY",  "S&P MidCap 400 ETF"},
            {"VTI",  "Total Stock Market ETF"},
            {"VEA",  "Developed Markets ETF"},
            {"VWO",  "Emerging Markets ETF"},
            {"EFA",  "iShares MSCI EAFE ETF"},
            {"EEM",  "iShares MSCI EM ETF"},
        }),

        // ── XL Sectors (SPDR Select Sector ETFs) ────────────────────────────
        make("XL Sectors", AssetType::Stock, {
            {"XLK",  "Technology"},
            {"XLF",  "Financials"},
            {"XLV",  "Health Care"},
            {"XLY",  "Consumer Discretionary"},
            {"XLP",  "Consumer Staples"},
            {"XLE",  "Energy"},
            {"XLI",  "Industrials"},
            {"XLB",  "Materials"},
            {"XLRE", "Real Estate"},
            {"XLU",  "Utilities"},
            {"XLC",  "Communication Services"},
        }),

        // ── Commodities (ETF proxies) ────────────────────────────────────────
        make("Commodities", AssetType::Stock, {
            {"GLD",  "Gold ETF"},
            {"SLV",  "Silver ETF"},
            {"USO",  "US Oil Fund"},
            {"UNG",  "Natural Gas ETF"},
            {"CORN", "Corn ETF"},
            {"WEAT", "Wheat ETF"},
            {"SOYB", "Soybeans ETF"},
            {"CPER", "Copper ETF"},
            {"PALL", "Palladium ETF"},
            {"PPLT", "Platinum ETF"},
            {"DBA",  "Agri Commodity ETF"},
            {"DJP",  "Bloomberg Commodity"},
        }),

        // ── Top Stocks ───────────────────────────────────────────────────────
        make("Top Stocks", AssetType::Stock, {
            {"AAPL",  "Apple"},
            {"NVDA",  "NVIDIA"},
            {"MSFT",  "Microsoft"},
            {"AMZN",  "Amazon"},
            {"GOOGL", "Alphabet"},
            {"META",  "Meta Platforms"},
            {"TSLA",  "Tesla"},
            {"AVGO",  "Broadcom"},
            {"JPM",   "JPMorgan Chase"},
            {"V",     "Visa"},
            {"UNH",   "UnitedHealth"},
            {"LLY",   "Eli Lilly"},
            {"XOM",   "ExxonMobil"},
            {"JNJ",   "Johnson & Johnson"},
            {"WMT",   "Walmart"},
            {"MA",    "Mastercard"},
            {"PG",    "Procter & Gamble"},
            {"HD",    "Home Depot"},
            {"COST",  "Costco"},
            {"ORCL",  "Oracle"},
        }),

        // ── Fixed Income / Bonds ─────────────────────────────────────────────
        make("Fixed Income", AssetType::Stock, {
            {"TLT",  "20yr Treasury ETF"},
            {"IEF",  "7-10yr Treasury ETF"},
            {"SHY",  "1-3yr Treasury ETF"},
            {"BND",  "Vanguard Bond ETF"},
            {"AGG",  "US Agg Bond ETF"},
            {"HYG",  "High Yield Corp ETF"},
            {"LQD",  "Invest Grade Corp ETF"},
            {"TIP",  "TIPS ETF"},
            {"MBB",  "Mortgage-Backed ETF"},
            {"VCSH", "Short-Term Corp ETF"},
            {"VCIT", "Interm-Term Corp ETF"},
        }),

        // ── Crypto (Coinbase pairs — routed to heatmap) ──────────────────────
        make("Crypto", AssetType::Crypto, {
            {"BTC-USD",  "Bitcoin"},
            {"ETH-USD",  "Ethereum"},
            {"SOL-USD",  "Solana"},
            {"XRP-USD",  "Ripple"},
            {"DOGE-USD", "Dogecoin"},
            {"ADA-USD",  "Cardano"},
            {"AVAX-USD", "Avalanche"},
            {"LINK-USD", "Chainlink"},
            {"DOT-USD",  "Polkadot"},
            {"LTC-USD",  "Litecoin"},
            {"UNI-USD",  "Uniswap"},
            {"ATOM-USD", "Cosmos"},
            {"NEAR-USD", "NEAR Protocol"},
            {"BCH-USD",  "Bitcoin Cash"},
            {"MATIC-USD","Polygon"},
        }),
    };
}

void WatchlistDock::buildUi() {
    QVBoxLayout* layout = new QVBoxLayout(m_contentWidget);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    // ── Preset selector row ──────────────────────────────────────────────────
    QHBoxLayout* topRow = new QHBoxLayout();
    topRow->setSpacing(6);

    QLabel* listLabel = new QLabel("Watchlist:", m_contentWidget);
    listLabel->setStyleSheet("QLabel { color: #8FA3B8; font-size: 11px; }");
    topRow->addWidget(listLabel);

    m_presetCombo = new QComboBox(m_contentWidget);
    for (const auto& preset : m_presets) {
        m_presetCombo->addItem(preset.name);
    }
    m_presetCombo->setStyleSheet(
        "QComboBox {"
        "  background-color: #2A2F38;"
        "  color: #D0D6DD;"
        "  border: 1px solid #3C4450;"
        "  border-radius: 4px;"
        "  padding: 3px 8px;"
        "  font-weight: 600;"
        "}"
        "QComboBox::drop-down { border: none; width: 20px; }"
        "QComboBox QAbstractItemView {"
        "  background-color: #1E2530;"
        "  color: #D0D6DD;"
        "  selection-background-color: #2E5FA3;"
        "}"
    );
    topRow->addWidget(m_presetCombo, 1);
    layout->addLayout(topRow);

    // ── Tree view ────────────────────────────────────────────────────────────
    m_model->setHorizontalHeaderLabels({"Symbol", "Name"});
    m_tree->setModel(m_model);
    m_tree->setRootIsDecorated(false);
    m_tree->setAlternatingRowColors(true);
    m_tree->setItemsExpandable(false);
    m_tree->setUniformRowHeights(true);
    m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tree->setStyleSheet(
        "QTreeView { background-color: #1A1F28; alternate-background-color: #1E2530; color: #D0D6DD; }"
        "QTreeView::item:selected { background-color: #2E5FA3; color: #FFFFFF; }"
        "QTreeView::item:hover { background-color: #253040; }"
        "QHeaderView::section { background-color: #252B36; color: #8FA3B8; border: none; padding: 4px; }"
    );

    auto* headerView = m_tree->header();
    headerView->setStretchLastSection(true);
    headerView->setSectionResizeMode(0, QHeaderView::ResizeToContents);

    layout->addWidget(m_tree, 1);
    m_contentWidget->setLayout(layout);

    connect(m_presetCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &WatchlistDock::onPresetChanged);
    connect(m_tree, &QTreeView::clicked, this, &WatchlistDock::onRowClicked);
}

void WatchlistDock::loadPreset(int index) {
    if (index < 0 || index >= m_presets.size()) {
        return;
    }
    m_model->removeRows(0, m_model->rowCount());

    const WatchlistPreset& preset = m_presets[index];
    const bool isCrypto = (preset.assetType == AssetType::Crypto);

    // Section header row — uses named badge constants
    {
        const char* badgeText  = isCrypto ? kCryptoBadgeText  : kStockBadgeText;
        const char* badgeColor = isCrypto ? kCryptoBadgeColor : kStockBadgeColor;

        auto* headerItem = new QStandardItem(QLatin1String(badgeText));
        headerItem->setData(true, IsSectionRole);
        headerItem->setForeground(QBrush(QColor(QLatin1String(badgeColor))));
        QFont f;
        f.setBold(true);
        f.setPointSize(9);
        headerItem->setFont(f);
        headerItem->setFlags(Qt::ItemIsEnabled);

        auto* headerDesc = new QStandardItem(preset.name);
        headerDesc->setForeground(QBrush(QColor("#6B7A8D")));
        headerDesc->setFlags(Qt::ItemIsEnabled);

        m_model->appendRow({headerItem, headerDesc});
    }

    const QString assetTypeStr = assetTypeString(preset.assetType);
    for (const auto& sym : preset.symbols) {
        auto* tickerItem = new QStandardItem(sym.first);
        tickerItem->setData(sym.first,    TickerRole);
        tickerItem->setData(assetTypeStr, AssetTypeRole);

        auto* nameItem = new QStandardItem(sym.second);
        nameItem->setForeground(QBrush(QColor("#8FA3B8")));

        m_model->appendRow({tickerItem, nameItem});
    }
}

void WatchlistDock::onPresetChanged(int index) {
    loadPreset(index);
}

void WatchlistDock::onRowClicked(const QModelIndex& index) {
    if (!index.isValid()) {
        return;
    }
    QStandardItem* item = m_model->itemFromIndex(m_model->index(index.row(), 0));
    if (!item || item->data(IsSectionRole).toBool()) {
        return;
    }
    const QString ticker    = item->data(TickerRole).toString();
    const QString assetType = item->data(AssetTypeRole).toString();
    if (!ticker.isEmpty() && !assetType.isEmpty()) {
        emit symbolSelected(ticker, assetType);
    }
}
