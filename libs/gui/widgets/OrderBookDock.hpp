#pragma once

#include "DockablePanel.hpp"
#include "marketdata/analytics/LeanGauge.hpp"
#include "../models/DomModel.hpp"
#include "../models/DomGrouping.hpp"
#include <QElapsedTimer>
#include <QImage>
#include <QPointer>
#include <QStyledItemDelegate>
#include <QTimer>
#include <functional>
#include <vector>

class LeanGaugeWidget;
class IGridDataSource;
class MarketHealth;
class QHBoxLayout;
class QVBoxLayout;
class QLabel;
class QMenu;
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
    // Liquidity Pulse: eaten rows flash in the aggressor's side colour (buyers
    // eat asks: bid colour), added segments are bright, pulled outlines grey.
    static QColor eatenColor(bool bidLevel, double alpha);
    static QColor addedColor(bool bid, double alpha);
    static QColor pulledColor(double alpha);
private:
    // Pulse tags are drawn from glyph strips rendered once per font, scale and colour:
    // drawText, setPen(QColor) and setFont allocate in Qt on every call; drawImage does not.
    struct TagGlyphs {
        static constexpr int Colours = 5; // added bid, added ask, pulled, eaten by buyers, eaten by sellers
        QFont font;
        qreal scale = 0;
        int height = 0, ascent = 0, fallback = 0;
        std::array<QImage, Colours> strips;
        std::array<int8_t, 256> index{};
        std::array<int, 32> offset{}, width{};
    };
    const TagGlyphs& tagGlyphs(const QFont& font, qreal scale) const;
    int tagWidth(const TagGlyphs& glyphs, const QString& text) const;
    void paintPulse(QPainter* painter, const QStyleOptionViewItem& opt, int row, bool bid,
                    const QRect& area, double value, bool cumulative, const QString& label) const;
    void paintSize(QPainter* painter, const QStyleOptionViewItem& opt, const QModelIndex& index, const QString& text) const;
    void paintPrice(QPainter* painter, const QStyleOptionViewItem& opt, const QString& text) const;
    static void drawFitted(QPainter* painter, const QStyleOptionViewItem& opt, const QString& text, const QColor& color);
    DomModel* m_model;
    mutable TagGlyphs m_tags;
};

class OrderBookDock : public DockablePanel {
    Q_OBJECT
public:
    explicit OrderBookDock(QWidget* parent = nullptr, IGridDataSource* source = nullptr);
    void buildUi() override;
    void onSymbolChanged(const QString& symbol) override;
    // Test hook: the pulse's monotonic clock in ms (default: a QElapsedTimer).
    void setPulseClock(std::function<qint64()> clock) { m_pulseClockSource = std::move(clock); }
    const sentinel::book::LeanGauge& leanGauge() const { return m_leanGauge; }
    uint64_t leanSteps() const { return m_leanSteps; }
    static constexpr int PulseTradeCapacity = 4096;
    // Row grouping (heading menu): Auto (DomGrouping AutoGroupRule with a hold), a fixed
    // row size in quote units (nearest offered choice), or Link (the heatmap's tick).
    enum class Grouping { Auto, Fixed, Link };
    void setGrouping(Grouping mode, double fixedTick = 0);
    Grouping grouping() const { return m_groupMode; }
    double fixedGroupTick() const { return m_fixedTick; }
    const dom::GroupChoices& groupChoices() const { return m_choices; }
    uint64_t autoGroupChanges() const { return m_autoHold.changes(); }
    // Heatmap tick shown now (Manual: the locked preset; Auto: the drawn tick), in quote units.
    void setHeatmapTick(qint64 units, double priceScale, bool manual);

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    void onOrderBookUpdated(const QString& symbol, const std::vector<BookDelta>& deltas);
    void onTradeReceived(const Trade& trade);
    void refreshDisplay();
    void recenter();
    void fillExposedRows();

private:
    QHBoxLayout* buildHeading();
    void buildLadder(QVBoxLayout* layout);
    void refreshBook();
    void setDisplayActive(bool active);
    void stopFollowing();
    void setCumulative(bool on);
    void setPulse(bool on);
    void buildLean(QHBoxLayout* heading, QVBoxLayout* layout);
    void updateLeanVisibility();
    void fitHeading();
    void applyHeadingStyle(bool compact);
    void buildGrouping(QHBoxLayout* line);
    void fillGroupMenu();
    void updateGrouping(const LiveOrderBook& book);
    void updateGroupButton();
    void refreshLean();
    void stepLean(const LiveOrderBook& book);
    void updatePulseActive();
    qint64 pulseClock(bool snapshot);
    void updatePulseFooter();
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
    QToolButton* m_pulse = nullptr;
    QLabel* m_pulseFooter = nullptr;
    // Liquidity Pulse feed: trades stamped on receipt, supplied once to the next step.
    QElapsedTimer m_pulseElapsed;
    std::function<qint64()> m_pulseClockSource;
    std::vector<sentinel::book::Trade> m_pulseTrades;
    qint64 m_lastPulseMs = 0;
    uint64_t m_footerVersion = UINT64_MAX;
    int m_footerWidth = -1;
    bool m_pulseOn = true, m_pulseActive = false, m_pulseOverflowWarned = false;
    sentinel::book::LeanGauge m_leanGauge;
    LeanGaugeWidget* m_leanStrip = nullptr;
    QToolButton* m_lean = nullptr;
    QHBoxLayout* m_heading = nullptr;
    int m_headingNatural = -1; // unmodified heading width; -1: measure again
    bool m_headingCompact = false;
    QToolButton* m_grouping = nullptr;
    QMenu* m_groupMenu = nullptr;
    Grouping m_groupMode = Grouping::Auto;
    double m_fixedTick = 0, m_linkTick = 0, m_linkScale = 0, m_groupNative = 0, m_shownNative = -1, m_shownLinkTick = -1, m_shownLinkScale = -1;
    qint64 m_linkUnits = 0, m_bandLevels = 0;
    bool m_linkManual = false, m_linkExact = true;
    dom::LinkGroup::Fallback m_linkFallback = dom::LinkGroup::Fallback::None, m_shownFallback = dom::LinkGroup::Fallback::None;
    dom::GroupChoices m_choices;
    dom::GroupHold m_autoHold, m_linkHold;
    int64_t m_shownGroup = 0;
    Grouping m_shownMode = Grouping::Auto;
    bool m_shownExact = true;
    std::vector<std::pair<uint32_t, double>> m_groupBids, m_groupAsks;
    std::vector<std::pair<uint32_t, double>> m_leanBids, m_leanAsks;
    uint64_t m_leanGeneration = 0, m_leanSteps = 0;
    QTimer m_timer;
    bool m_dirty = true, m_symbolDirty = true, m_bookDirty = true;
    bool m_exposed = false;
    bool m_displayActive = false, m_follow = true, m_programmaticScroll = false;
};
