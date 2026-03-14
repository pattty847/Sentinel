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
        QString name;
        QString assetType;           // "stock" or "crypto"
        QVector<QPair<QString, QString>> symbols; // (ticker, description)
    };

    void buildUi() override;
    void initPresets();
    void loadPreset(int index);

    QComboBox*          m_presetCombo = nullptr;
    QTreeView*          m_tree        = nullptr;
    QStandardItemModel* m_model       = nullptr;
    QVector<WatchlistPreset> m_presets;
};
