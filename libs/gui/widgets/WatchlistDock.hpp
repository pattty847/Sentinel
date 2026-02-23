#pragma once
#include "DockablePanel.hpp"
#include <QTreeView>
#include <QStandardItemModel>

class WatchlistDock : public DockablePanel {
    Q_OBJECT
public:
    explicit WatchlistDock(QWidget* parent = nullptr);
    QSize minimumSizeHint() const override;

private:
    void buildUi() override;
    void populatePlaceholderData();

    QTreeView* m_tree = nullptr;
    QStandardItemModel* m_model = nullptr;
};
