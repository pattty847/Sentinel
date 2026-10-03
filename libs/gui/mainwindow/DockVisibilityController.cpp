#include "DockVisibilityController.hpp"
#include "config/AgentHostMode.hpp"

#include <QDockWidget>
#include <QObject>
#include <QSettings>

namespace {
constexpr auto kSettingsKey = "agentApi/docks/visible";
}

DockVisibilityController::~DockVisibilityController() {
    for (const auto& connection : m_connections) QObject::disconnect(connection);
}

void DockVisibilityController::add(const QString& id, QDockWidget* dock) {
    if (!dock) return;
    m_docks.insert(id, dock);
    m_connections.append(QObject::connect(dock, &QDockWidget::visibilityChanged, m_window, [this, id, dock] {
        // A tab switch may emit visibilityChanged while its toggle action stays checked.
        // Only a real owner visibility change takes this dock out of the API baseline.
        if (!m_applying && m_apiState.contains(id) &&
            dock->toggleViewAction()->isChecked() != m_apiState.value(id).toBool()) {
            m_apiState.remove(id);
            if (m_unpersistedBaseline) {
                m_unpersistedBaseline->remove(id);
                if (m_unpersistedBaseline->isEmpty()) m_unpersistedBaseline.reset();
            }
        }
    }));
}

QJsonObject DockVisibilityController::snapshot() const {
    QJsonObject visible;
    for (auto it = m_docks.cbegin(); it != m_docks.cend(); ++it)
        visible.insert(it.key(), it.value()->toggleViewAction()->isChecked());
    return visible;
}

QJsonObject DockVisibilityController::apply(const QJsonObject& changes, const QString& focus, bool persist) {
    QJsonObject desired = snapshot();
    const QJsonObject before = desired;
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
    if (!persist && !m_applying) {
        if (!m_unpersistedBaseline) m_unpersistedBaseline = QJsonObject{};
        for (auto it = desired.constBegin(); it != desired.constEnd(); ++it) {
            if (it.value() == before.value(it.key())) continue;
            if (!m_unpersistedBaseline->contains(it.key()))
                m_unpersistedBaseline->insert(it.key(), before.value(it.key()));
            m_apiState.insert(it.key(), it.value());
        }
        if (m_unpersistedBaseline->isEmpty()) m_unpersistedBaseline.reset();
    }
    m_applying = true;
    // Hide first so a focused dock can claim the freed space before it is raised.
    for (auto it = m_docks.cbegin(); it != m_docks.cend(); ++it)
        if (!desired.value(it.key()).toBool()) it.value()->hide();
    for (auto it = m_docks.cbegin(); it != m_docks.cend(); ++it)
        if (desired.value(it.key()).toBool()) it.value()->show();
    if (!focus.isEmpty()) {
        auto* target = m_docks.value(focus);
        if (target->isFloating()) {
            target->setFloating(false);
            if (m_window->dockWidgetArea(target) == Qt::NoDockWidgetArea)
                m_window->addDockWidget(Qt::LeftDockWidgetArea, target);
        }
        target->raise();
    }
    m_window->updateGeometry();
    m_applying = false;
    if (persist) {
        save();
        m_unpersistedBaseline.reset();
        m_apiState = {};
    }
    return snapshot();
}

void DockVisibilityController::save() const {
    if (AgentHostMode::active()) {
        QSettings settings(AgentHostMode::dockProfileFile(), QSettings::IniFormat);
        settings.setValue(kSettingsKey, snapshot().toVariantMap());
        settings.sync();
    } else {
        QSettings settings(QSettings::defaultFormat(), QSettings::UserScope, "Sentinel", "SentinelTerminal");
        settings.setValue(kSettingsKey, snapshot().toVariantMap());
        settings.sync();
    }
}

void DockVisibilityController::restore() {
    QVariant storedValue;
    if (AgentHostMode::active()) {
        QSettings settings(AgentHostMode::dockProfileFile(), QSettings::IniFormat);
        storedValue = settings.value(kSettingsKey);
    } else {
        QSettings settings(QSettings::defaultFormat(), QSettings::UserScope, "Sentinel", "SentinelTerminal");
        storedValue = settings.value(kSettingsKey);
    }
    if (!storedValue.isValid()) return;
    const QJsonObject stored = QJsonObject::fromVariantMap(storedValue.toMap());
    QJsonObject valid;
    for (auto it = stored.constBegin(); it != stored.constEnd(); ++it)
        if (m_docks.contains(it.key()) && it.value().isBool()) valid.insert(it.key(), it.value());
    apply(valid, {}, false);
    m_unpersistedBaseline.reset();
    m_apiState = {};
}

void DockVisibilityController::restoreBeforeSessionSave() {
    if (m_unpersistedBaseline) {
        const QJsonObject original = *m_unpersistedBaseline;
        m_unpersistedBaseline.reset();
        m_applying = true;
        apply(original, {}, false);
        m_applying = false;
        m_unpersistedBaseline.reset();
        m_apiState = {};
    }
}
