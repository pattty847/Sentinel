#pragma once
#include "DockablePanel.hpp"
#include <QComboBox>
#include <QLineEdit>
#include <QSet>
#include <QStandardItemModel>
#include <QStringList>
#include <QToolButton>
#include <QTreeView>
#include <QVector>

class QLabel;

class WatchlistDock : public DockablePanel {
    Q_OBJECT
public:
    enum WatchlistItemRole { TickerRole = Qt::UserRole + 1, AssetTypeRole };
    enum class AssetType { Stock, Crypto };

    explicit WatchlistDock(QWidget* parent = nullptr);
    QSize minimumSizeHint() const override { return {220, 260}; }

    // The hub calls this from its acknowledged chart switch path. A request alone is never active.
    void setChartSwitchState(const QString& activeSymbol, const QString& pendingSymbol,
                             const QString& refusedSymbol = {}, const QString& refusalReason = {});
    // Until a catalog is authoritative, suggestions remain explicitly unverified.
    void setCryptoAvailability(const QStringList& supportedSymbols, bool authoritative,
                               const QString& source = {});

signals:
    void symbolSelected(const QString& symbol, const QString& assetType);

private slots:
    void onPresetChanged(int index);
    void onRowActivated(const QModelIndex& index);
    void addPinnedSymbol();
    void toggleSelectedPin();

private:
    void changeEvent(QEvent* event) override;
    struct WatchlistPreset {
        QString name;
        AssetType assetType;
        QVector<QPair<QString, QString>> symbols;
    };
    void buildUi() override;
    void initPresets();
    void loadPreset(int index);
    void refreshRowState();
    void updatePinButton();
    void savePreferences();
    QString selectedSymbol() const;

    QComboBox* m_presetCombo = nullptr;
    QLineEdit* m_symbolEdit = nullptr;
    QToolButton* m_addButton = nullptr;
    QToolButton* m_pinButton = nullptr;
    QTreeView* m_tree = nullptr;
    QLabel* m_status = nullptr;
    QStandardItemModel* m_model = nullptr;
    QVector<WatchlistPreset> m_presets;
    QStringList m_pinned;
    QSet<QString> m_supported;
    QString m_activeSymbol;
    QString m_pendingSymbol;
    QString m_refusedSymbol;
    QString m_refusalReason;
    QString m_catalogSource;
    bool m_catalogAuthoritative = false;
    bool m_fontRefreshPending = false;
};
