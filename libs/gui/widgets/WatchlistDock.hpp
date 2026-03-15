#pragma once
#include "DockablePanel.hpp"
#include <QTreeView>
#include <QStandardItemModel>
#include <QComboBox>
#include <QString>
#include <QVector>
#include <QPair>

class WatchlistDock : public DockablePanel {
    Q_OBJECT
public:
    // Named roles for QStandardItem custom data — replaces magic Qt::UserRole + N numbers.
    enum WatchlistItemRole {
        IsSectionRole = Qt::UserRole + 1,
        TickerRole,
        AssetTypeRole,
    };

    // Strongly-typed asset class — prevents typo-prone "crypto"/"stock" string comparisons
    // inside WatchlistDock. The public signal still uses QString for interface consistency
    // with ScreenerDock::rowSelected so callers share a single routing slot.
    enum class AssetType { Stock, Crypto };

    explicit WatchlistDock(QWidget* parent = nullptr);
    QSize minimumSizeHint() const override;

signals:
    // Emitted when the user clicks a watchlist row.
    // assetType is "crypto" or "stock" — callers route accordingly.
    void symbolSelected(const QString& symbol, const QString& assetType);

private slots:
    void onPresetChanged(int index);
    void onRowClicked(const QModelIndex& index);

private:
    struct WatchlistPreset {
        QString    name;
        AssetType  assetType;
        QVector<QPair<QString, QString>> symbols; // (ticker, description)
    };

    static QString assetTypeString(AssetType t) {
        return (t == AssetType::Crypto) ? QStringLiteral("crypto") : QStringLiteral("stock");
    }

    void buildUi() override;
    void initPresets();
    void loadPreset(int index);

    QComboBox*          m_presetCombo = nullptr;
    QTreeView*          m_tree        = nullptr;
    QStandardItemModel* m_model       = nullptr;
    QVector<WatchlistPreset> m_presets;
};
