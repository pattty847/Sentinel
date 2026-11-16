#pragma once
// Shared dense table geometry; opt in at construction so paper tables retain their own spec.
#include "../themes/ThemePalette.hpp"
#include <QHeaderView>
#include <QTableView>

namespace tableChrome {
inline void header(QHeaderView* view) {
    QFont font = view->font();
    font.setPixelSize(11);
    font.setWeight(QFont::Normal);
    view->setFont(font);
    view->setHighlightSections(false);
    view->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    view->setStyleSheet(QStringLiteral(
        "QHeaderView::section { background:%1; color:%2; font-size:11px; font-weight:400; "
        "border:none; border-bottom:1px solid %3; padding:0px 4px; }")
        .arg(theme::bg2().name(), theme::text3().name(), theme::line().name()));
}
inline void apply(QTableView* table) {
    auto* rows = table->verticalHeader();
    rows->setMinimumSectionSize(22);
    rows->setMaximumSectionSize(22);
    rows->setDefaultSectionSize(22);
    rows->setSectionResizeMode(QHeaderView::Fixed);
    header(table->horizontalHeader());
}
inline void align(QAbstractItemModel* model, int column, bool numeric) {
    model->setHeaderData(column, Qt::Horizontal,
        int((numeric ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter), Qt::TextAlignmentRole);
}
} // namespace tableChrome
