#include "WatchlistDock.hpp"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QBrush>
#include <QColor>
#include <QFont>

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
    // ── Major Indices ────────────────────────────────────────────────────────
    {
        WatchlistPreset p;
        p.name      = "Major Indices";
        p.assetType = "stock";
        p.symbols   = {
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
        };
        m_presets.append(p);
    }

    // ── XL Sectors (SPDR Select Sector ETFs) ────────────────────────────────
    {
        WatchlistPreset p;
        p.name      = "XL Sectors";
        p.assetType = "stock";
        p.symbols   = {
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
        };
        m_presets.append(p);
    }

    // ── Commodities (ETF proxies) ────────────────────────────────────────────
    {
        WatchlistPreset p;
        p.name      = "Commodities";
        p.assetType = "stock";
        p.symbols   = {
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
        };
        m_presets.append(p);
    }

    // ── Top Stocks ───────────────────────────────────────────────────────────
    {
        WatchlistPreset p;
        p.name      = "Top Stocks";
        p.assetType = "stock";
        p.symbols   = {
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
        };
        m_presets.append(p);
    }

    // ── Fixed Income / Bonds ─────────────────────────────────────────────────
    {
        WatchlistPreset p;
        p.name      = "Fixed Income";
        p.assetType = "stock";
        p.symbols   = {
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
        };
        m_presets.append(p);
    }

    // ── Crypto (Coinbase pairs — routed to heatmap) ──────────────────────────
    {
        WatchlistPreset p;
        p.name      = "Crypto";
        p.assetType = "crypto";
        p.symbols   = {
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
        };
        m_presets.append(p);
    }
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

    // ── Asset type badge ─────────────────────────────────────────────────────
    // (shown inline in the tree as a section header per preset)

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

    // Section header row showing asset type
    {
        const QString badge = (preset.assetType == "crypto") ? "● CRYPTO" : "● STOCKS";
        const QString badgeColor = (preset.assetType == "crypto") ? "#F7931A" : "#4CAF50";
        auto* headerItem = new QStandardItem(badge);
        headerItem->setData(true, Qt::UserRole + 1); // mark as section
        headerItem->setForeground(QBrush(QColor(badgeColor)));
        QFont f;
        f.setBold(true);
        f.setPointSize(9);
        headerItem->setFont(f);
        headerItem->setFlags(Qt::ItemIsEnabled); // not selectable
        auto* headerDesc = new QStandardItem(preset.name);
        headerDesc->setForeground(QBrush(QColor("#6B7A8D")));
        headerDesc->setFlags(Qt::ItemIsEnabled);
        m_model->appendRow({headerItem, headerDesc});
    }

    for (const auto& sym : preset.symbols) {
        auto* tickerItem = new QStandardItem(sym.first);
        tickerItem->setData(sym.first, Qt::UserRole + 2);    // ticker
        tickerItem->setData(preset.assetType, Qt::UserRole + 3); // assetType
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
    // Get the first-column item
    QModelIndex firstCol = m_model->index(index.row(), 0);
    QStandardItem* item  = m_model->itemFromIndex(firstCol);
    if (!item) {
        return;
    }
    // Skip section-header rows
    if (item->data(Qt::UserRole + 1).toBool()) {
        return;
    }
    const QString ticker    = item->data(Qt::UserRole + 2).toString();
    const QString assetType = item->data(Qt::UserRole + 3).toString();
    if (!ticker.isEmpty() && !assetType.isEmpty()) {
        emit symbolSelected(ticker, assetType);
    }
}
