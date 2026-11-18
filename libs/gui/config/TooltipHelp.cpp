#include "TooltipHelp.hpp"
#include <QSettings>
#include <QStringList>
#include <QWidget>

namespace {
constexpr auto Key = "ui/showExplanatoryTooltips";
constexpr auto Forms = "sentinelTooltipForms";
}

TooltipHelp::TooltipHelp(QObject* parent) : QObject(parent) {
    QSettings settings(QSettings::defaultFormat(), QSettings::UserScope, "Sentinel", "SentinelGUI");
    m_explanatory = settings.value(Key, false).toBool();
}

TooltipHelp& TooltipHelp::instance() {
    static TooltipHelp help;
    return help;
}

void TooltipHelp::setExplanatory(bool enabled) {
    if (m_explanatory == enabled) return;
    m_explanatory = enabled;
    QSettings settings(QSettings::defaultFormat(), QSettings::UserScope, "Sentinel", "SentinelGUI");
    settings.setValue(Key, enabled);
    emit changed(enabled);
}

QString TooltipHelp::text(const QString& shortForm, const QString& longForm) const {
    return m_explanatory ? longForm : shortForm;
}

void TooltipHelp::setToolTip(QWidget* widget, const QString& shortForm, const QString& longForm) {
    if (!widget->property(Forms).isValid()) {
        connect(this, &TooltipHelp::changed, widget, [this, widget] {
            const auto forms = widget->property(Forms).toStringList();
            widget->setToolTip(text(forms.at(0), forms.at(1)));
        });
    }
    widget->setProperty(Forms, QStringList{shortForm, longForm});
    widget->setToolTip(text(shortForm, longForm));
}
