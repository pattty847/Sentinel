#pragma once

#include <QObject>
#include <QString>

class QApplication;

class WidgetToolTips final : public QObject {
public:
    static void install(QApplication* app);
    static QString wrappedText(const QString& text);

private:
    explicit WidgetToolTips(QApplication* app);
    bool eventFilter(QObject* watched, QEvent* event) override;
};
