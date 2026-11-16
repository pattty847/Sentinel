#pragma once
// Heatmap telemetry dock (S6c): the sentinel-lab telemetry panel (apps/sentinel-lab
// Main.qml: tick, GPU node, live edge, data controller, render) for the main
// chart. A QTimer polls the provider at 4 Hz on the GUI thread while the dock is
// visible; nothing runs per frame, and nothing runs while it is hidden. The
// provider returns nullopt while the chart draws with the legacy renderer (the
// dock then shows its disabled state). Values are widgets, not QML: one more
// QQuickWidget scene would add a second QRhi per dock for a table of numbers.
#include "DockablePanel.hpp"
#include <QHash>
#include <QPointer>
#include "../models/MarketHealth.hpp"
#include <QIcon>
#include <QVariantMap>
#include <array>
#include <functional>
#include <optional>
#include <vector>

class QLabel;
class QTimer;

class HeatmapTelemetryDock : public DockablePanel {
    Q_OBJECT
public:
    using Provider = std::function<std::optional<QVariantMap>()>;
    static constexpr int kRefreshMs = 250; // 4 Hz, as the lab panel
    static constexpr int kMinimumWidth = 440;

    explicit HeatmapTelemetryDock(QWidget *parent = nullptr);
    void buildUi() override;
    void setProvider(Provider provider);
    void setMarketHealth(MarketHealth* health);
    // One poll now (the timer calls this).
    void refresh();
    uint64_t refreshCount() const { return m_refreshes; }
    QTimer *timer() const { return m_timer; }
    bool disabledShown() const;
    // On screen now: shown and, when tabified, the current tab (visibilityChanged).
    // Distinct from the persisted showTelemetry preference.
    bool exposed() const { return m_exposed; }
    // A row's shown value by its metric key (e.g. "tick", "residentBytes").
    QString valueText(const QString &key) const;
    const std::vector<double> &frameHistory() const { return m_frameHistory; }

signals:
    // The user closed the dock (title-bar close button), as opposed to a layout
    // change hiding it.
    void closedByUser();

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    struct Row {
        QString key;
        QLabel *value = nullptr;
        std::function<QString(const QVariantMap &)> format;
    };
    // Collapsed health facts, in grid order.
    enum class HealthFact { Connection, Subscription, BookAge, HeatmapAge, History, Loading, OldPicture, Count };
    enum class Tone { Normal, Unknown, Warn, Error };
    struct HealthRow {
        QLabel *key = nullptr;
        QLabel *value = nullptr;
        Tone tone = Tone::Normal;
        bool styled = false;
    };
    void setHealthFact(HealthFact id, const QString &value, Tone tone);
    QString healthDetailText() const;
    void addSection(const QString &title);
    void addRow(const QString &name, const QString &key, std::function<QString(const QVariantMap &)> format);

    Provider m_provider;
    QPointer<MarketHealth> m_health;
    QLabel* m_healthSummary = nullptr;
    class QGridLayout* m_healthGrid = nullptr;
    std::array<HealthRow, size_t(HealthFact::Count)> m_healthFacts{};
    QLabel* m_healthReason = nullptr;
    class QToolButton* m_engineeringToggle = nullptr;
    QIcon m_chevronClosed, m_chevronOpen;
    QTimer *m_timer = nullptr;
    QWidget *m_table = nullptr;
    QLabel *m_disabled = nullptr;
    QLabel *m_indicator = nullptr;
    class QGridLayout *m_grid = nullptr;
    class HeatmapFrameGraph *m_graph = nullptr;
    std::vector<Row> m_rows;
    std::vector<double> m_frameHistory;
    uint64_t m_refreshes = 0;
    bool m_exposed = false;
    void setExposed(bool exposed);
};
