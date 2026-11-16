#pragma once
// Designed empty state for an item view (brief principle 10, B-17): a centred note, and an
// optional action, inside the view's viewport while its model has no rows. An empty view has
// nothing to scroll sideways, so its horizontal scrollbar stays off until rows arrive.
#include "../themes/ThemePalette.hpp"
#include <QAbstractItemView>
#include <QEvent>
#include <QLabel>
#include <QVBoxLayout>

class EmptyState final : public QWidget {
public:
    EmptyState(QAbstractItemView* view, const QString& text, QWidget* action = nullptr)
        : QWidget(view->viewport()), m_view(view) {
        setObjectName(view->objectName() + QStringLiteral("Empty"));
        auto* layout = new QVBoxLayout(this);
        layout->setAlignment(Qt::AlignCenter);
        layout->setSpacing(8);
        m_label = new QLabel(text, this);
        m_label->setObjectName(objectName() + QStringLiteral("Text"));
        m_label->setAlignment(Qt::AlignCenter);
        m_label->setWordWrap(true);
        m_label->setStyleSheet(QStringLiteral("color:%1; background:transparent;").arg(theme::text3().name()));
        layout->addWidget(m_label);
        if (action) {
            action->setParent(this);
            layout->addWidget(action, 0, Qt::AlignHCenter);
        }
        view->viewport()->installEventFilter(this);
        if (auto* model = view->model()) {
            connect(model, &QAbstractItemModel::rowsInserted, this, [this] { sync(); });
            connect(model, &QAbstractItemModel::rowsRemoved, this, [this] { sync(); });
            connect(model, &QAbstractItemModel::modelReset, this, [this] { sync(); });
            connect(model, &QAbstractItemModel::layoutChanged, this, [this] { sync(); });
        }
        sync();
    }
    void setText(const QString& text) { m_label->setText(text); }
    QString text() const { return m_label->text(); }
    void sync() {
        const bool empty = !m_view->model() || m_view->model()->rowCount() == 0;
        setGeometry(m_view->viewport()->rect());
        setVisible(empty);
        m_view->setHorizontalScrollBarPolicy(empty ? Qt::ScrollBarAlwaysOff : Qt::ScrollBarAsNeeded);
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == m_view->viewport() && event->type() == QEvent::Resize)
            setGeometry(m_view->viewport()->rect());
        return QWidget::eventFilter(watched, event);
    }

private:
    QAbstractItemView* m_view;
    QLabel* m_label = nullptr;
};
