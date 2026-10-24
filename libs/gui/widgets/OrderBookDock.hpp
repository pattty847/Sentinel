#pragma once

#include "DockablePanel.hpp"
#include "../models/DomModel.hpp"
#include <QPointer>
#include <QStyledItemDelegate>
#include <QTimer>

class IGridDataSource;
class MarketHealth;
class QLabel;
class QPushButton;
class QToolButton;
class QTableView;

class DomBarDelegate : public QStyledItemDelegate {
public:
    explicit DomBarDelegate(DomModel* model, QObject* parent = nullptr);
    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
    // Heatmap default-palette ramp (bids navy -> blue -> cyan, asks dark -> orange)
    // sampled by bar fraction in [0, 1].
    static QColor barColor(bool bid, double fraction);
    // A light tint of the bar colour whose opacity rises with the fraction.
    static QColor sizeTextColor(bool bid, double fraction);
    // Fill of the best bid / best ask size cell.
    static QColor bestFill(bool bid);
    // Characters of a price text drawn dim: everything before its last three digits.
    static int dimPrefixLength(const QString& price);
private:
    void paintSize(QPainter* painter, const QStyleOptionViewItem& opt, const QModelIndex& index, const QString& text) const;
    void paintPrice(QPainter* painter, const QStyleOptionViewItem& opt, const QString& text) const;
    static void drawFitted(QPainter* painter, const QStyleOptionViewItem& opt, const QString& text, const QColor& color);
    DomModel* m_model;
};

class OrderBookDock : public DockablePanel {
    Q_OBJECT
public:
    explicit OrderBookDock(QWidget* parent = nullptr, IGridDataSource* source = nullptr);
    void buildUi() override;
    void onSymbolChanged(const QString& symbol) override;

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    void onOrderBookUpdated(const QString& symbol, const std::vector<BookDelta>& deltas);
    void onTradeReceived(const Trade& trade);
    void refreshDisplay();
    void recenter();

private:
    void setDisplayActive(bool active);
    void stopFollowing();
    void setCumulative(bool on);
    void updateVisibleRows();
    void watchWindow();
    void updateDisplayTimer();
    bool minimized() const;
    void fitColumns();
    void updateSummary();
    QPointer<IGridDataSource> m_source;
    QPointer<MarketHealth> m_health;
    QString m_symbol;
    std::string m_symbolId;
    QPointer<QWidget> m_hostWindow;
    DomTradeWindow m_trades;
    DomFreshness m_freshness;
    DomModel* m_model = nullptr;
    QTableView* m_table = nullptr;
    QLabel* m_symbolLabel = nullptr;
    QLabel* m_aggregation = nullptr;
    QLabel* m_summary = nullptr;
    QLabel* m_status = nullptr;
    QPushButton* m_recenter = nullptr;
    QToolButton* m_cumulative = nullptr;
    QTimer m_timer;
    bool m_dirty = true, m_symbolDirty = true, m_bookDirty = true;
    bool m_exposed = false;
    bool m_displayActive = false, m_follow = true, m_programmaticScroll = false;
};
