#include "DockVisibilityController.hpp"

#include <QDockWidget>
#include <QSettings>

namespace {
constexpr auto kSettingsKey = "agentApi/docks/visible";
}

void DockVisibilityController::add(const QString& id, QDockWidget* dock) {
    if (dock) m_docks.insert(id, dock);
}

QJsonObject DockVisibilityController::snapshot() const {
    QJsonObject visible;
    for (auto it = m_docks.cbegin(); it != m_docks.cend(); ++it)
        visible.insert(it.key(), it.value()->toggleViewAction()->isChecked());
    return visible;
}

QJsonObject DockVisibilityController::apply(const QJsonObject& changes, const QString& focus, bool persist) {
    QJsonObject desired = snapshot();
    if (!persist && !m_unpersistedBaseline) m_unpersistedBaseline = desired;
    if (!focus.isEmpty()) {
        for (auto it = desired.begin(); it != desired.end(); ++it)
            it.value() = it.key() == focus;
    } else {
        for (auto it = changes.begin(); it != changes.end(); ++it)
            desired.insert(it.key(), it.value());
    }
    // The codec validates IDs and hide-all before this point. Keep a local guard for restores.
    bool any = false;
    for (auto it = desired.constBegin(); it != desired.constEnd(); ++it) any |= it.value().toBool();
    if (!any) return snapshot();
    // Hide first so a focused dock can claim the freed space before it is raised.
    for (auto it = m_docks.cbegin(); it != m_docks.cend(); ++it)
        if (!desired.value(it.key()).toBool()) it.value()->hide();
    for (auto it = m_docks.cbegin(); it != m_docks.cend(); ++it)
        if (desired.value(it.key()).toBool()) it.value()->show();
    if (!focus.isEmpty()) m_docks.value(focus)->raise();
    m_window->updateGeometry();
    if (persist) {
        save();
        m_unpersistedBaseline.reset();
    }
    return snapshot();
}

void DockVisibilityController::save() const {
    QSettings settings(QSettings::defaultFormat(), QSettings::UserScope, "Sentinel", "SentinelTerminal");
    settings.setValue(kSettingsKey, snapshot().toVariantMap());
    settings.sync();
}

void DockVisibilityController::restore() {
    QSettings settings(QSettings::defaultFormat(), QSettings::UserScope, "Sentinel", "SentinelTerminal");
    if (!settings.contains(kSettingsKey)) return;
    const QJsonObject stored = QJsonObject::fromVariantMap(settings.value(kSettingsKey).toMap());
    QJsonObject valid;
    for (auto it = stored.constBegin(); it != stored.constEnd(); ++it)
        if (m_docks.contains(it.key()) && it.value().isBool()) valid.insert(it.key(), it.value());
    apply(valid, {}, false);
    m_unpersistedBaseline.reset();
}

void DockVisibilityController::restoreBeforeSessionSave() {
    if (m_unpersistedBaseline) {
        const QJsonObject original = *m_unpersistedBaseline;
        m_unpersistedBaseline.reset();
        apply(original, {}, false);
        m_unpersistedBaseline.reset();
    }
}
