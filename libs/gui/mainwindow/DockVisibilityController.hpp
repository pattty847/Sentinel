#pragma once

#include <QJsonObject>
#include <QMainWindow>
#include <QMap>
#include <QString>
#include <optional>

class QDockWidget;

// GUI-thread controller for the Agent API's dock visibility, independent of saved layouts.
class DockVisibilityController {
public:
    explicit DockVisibilityController(QMainWindow* window) : m_window(window) {}
    void add(const QString& id, QDockWidget* dock);
    QJsonObject snapshot() const;
    QJsonObject apply(const QJsonObject& changes, const QString& focus, bool persist);
    void restore(); // call after the startup layout has been installed
    void restoreBeforeSessionSave(); // exclude owner-mode transient API changes from _last_session

private:
    void save() const;
    QMainWindow* m_window;
    QMap<QString, QDockWidget*> m_docks;
    std::optional<QJsonObject> m_unpersistedBaseline;
};
