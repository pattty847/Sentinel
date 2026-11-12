#include "WidgetToolTips.hpp"

#include <QAction>
#include <QAbstractItemView>
#include <QApplication>
#include <QEvent>
#include <QFontMetrics>
#include <QHeaderView>
#include <QHelpEvent>
#include <QMenu>
#include <QStringList>
#include <QTabBar>
#include <QToolTip>
#include <QWidget>

WidgetToolTips::WidgetToolTips(QApplication* app) : QObject(app) {}

void WidgetToolTips::install(QApplication* app) {
    app->installEventFilter(new WidgetToolTips(app));
}

QString WidgetToolTips::wrappedText(const QString& text) {
    constexpr int contentWidth = 344;
    QFont font = QApplication::font("QToolTip");
    font.setPixelSize(11);
    const QFontMetrics metrics(font);
    QStringList lines;
    for (const QString& paragraph : text.split('\n')) {
        QString line;
        for (const QString& word : paragraph.split(' ', Qt::SkipEmptyParts)) {
            const QString candidate = line.isEmpty() ? word : line + ' ' + word;
            if (metrics.horizontalAdvance(candidate) <= contentWidth) {
                line = candidate;
                continue;
            }
            if (!line.isEmpty()) lines << line;
            line.clear();
            if (metrics.horizontalAdvance(word) <= contentWidth) {
                line = word;
                continue;
            }
            for (const QChar ch : word) {
                if (!line.isEmpty() && metrics.horizontalAdvance(line + ch) > contentWidth) {
                    lines << line;
                    line.clear();
                }
                line += ch;
            }
        }
        lines << line.trimmed();
    }
    return QStringLiteral("<span style=\"white-space:pre\">%1</span>")
        .arg(lines.join('\n').toHtmlEscaped().replace('\n', QStringLiteral("<br/>")));
}

bool WidgetToolTips::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() != QEvent::ToolTip) return false;
    auto* widget = qobject_cast<QWidget*>(watched);
    if (!widget) return false;
    auto* help = static_cast<QHelpEvent*>(event);
    QString tip = widget->toolTip();
    if (auto* view = qobject_cast<QAbstractItemView*>(widget->parentWidget())) {
        if (view->viewport() == widget) {
            const QModelIndex index = view->indexAt(help->pos());
            const QString itemTip = index.data(Qt::ToolTipRole).toString();
            if (!itemTip.isEmpty()) tip = itemTip;
        }
    }
    if (auto* menu = qobject_cast<QMenu*>(widget)) {
        if (QAction* action = menu->actionAt(help->pos())) tip = action->toolTip();
    }
    if (auto* tabs = qobject_cast<QTabBar*>(widget)) {
        const int index = tabs->tabAt(help->pos());
        if (index >= 0) tip = tabs->tabToolTip(index);
    }
    if (auto* header = qobject_cast<QHeaderView*>(widget)) {
        const int index = header->logicalIndexAt(help->pos());
        if (index >= 0) tip = header->model()->headerData(index, header->orientation(),
                                                          Qt::ToolTipRole).toString();
    }
    if (tip.isEmpty()) return false;
    QToolTip::showText(help->globalPos() + QPoint(12, 16), wrappedText(tip), widget);
    return true;
}
