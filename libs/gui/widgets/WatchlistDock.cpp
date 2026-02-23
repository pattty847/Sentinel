#include "WatchlistDock.hpp"
#include <QVBoxLayout>
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
    buildUi();
    populatePlaceholderData();
}

QSize WatchlistDock::minimumSizeHint() const {
    return QSize(320, 360);
}

void WatchlistDock::buildUi() {
    QVBoxLayout* layout = new QVBoxLayout(m_contentWidget);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    QLabel* header = new QLabel("Main Watchlist", m_contentWidget);
    header->setStyleSheet("QLabel { color: #D0D6DD; font-weight: 600; letter-spacing: 0.2px; }");
    layout->addWidget(header);

    m_model->setHorizontalHeaderLabels({"Symbol", "Last", "Chg", "Chg%", "Vol"});
    m_tree->setModel(m_model);
    m_tree->setRootIsDecorated(false);
    m_tree->setAlternatingRowColors(true);
    m_tree->setItemsExpandable(false);
    m_tree->setUniformRowHeights(true);
    m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->setSelectionBehavior(QAbstractItemView::SelectRows);

    auto* headerView = m_tree->header();
    headerView->setStretchLastSection(false);
    headerView->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int i = 1; i < m_model->columnCount(); ++i) {
        headerView->setSectionResizeMode(i, QHeaderView::ResizeToContents);
    }

    layout->addWidget(m_tree, 1);
    m_contentWidget->setLayout(layout);
}

void WatchlistDock::populatePlaceholderData() {
    m_model->removeRows(0, m_model->rowCount());

    auto addSection = [this](const QString& name, const QList<QStringList>& rows) {
        QList<QStandardItem*> sectionRow;
        auto* sectionItem = new QStandardItem(name);
        sectionItem->setData(true, Qt::UserRole + 1);
        sectionItem->setForeground(QBrush(QColor("#8FA3B8")));
        sectionItem->setFont(QFont("", -1, QFont::DemiBold));
        sectionItem->setFlags(Qt::ItemIsEnabled);
        sectionRow << sectionItem;
        for (int i = 1; i < m_model->columnCount(); ++i) {
            sectionRow << new QStandardItem("");
        }
        m_model->appendRow(sectionRow);

        for (const auto& row : rows) {
            QList<QStandardItem*> items;
            for (int i = 0; i < row.size(); ++i) {
                auto* item = new QStandardItem(row[i]);
                items << item;
            }
            while (items.size() < m_model->columnCount()) {
                items << new QStandardItem("");
            }
            m_model->appendRow(items);
        }
    };

    addSection("CRYPTO", {
        {"BTC-USD", "88,493", "+243", "+0.28%", "9.8K"},
        {"ETH-USD", "3,091", "+10", "+0.33%", "8.1K"},
        {"SOL-USD", "243", "-1.2", "-0.49%", "1.9K"}
    });

    addSection("US MARKETS", {
        {"AAPL", "193.8", "+1.2", "+0.62%", "42.1M"},
        {"NVDA", "637.1", "+8.4", "+1.34%", "36.7M"},
        {"SPY", "467.0", "+0.9", "+0.20%", "58.3M"}
    });
}
