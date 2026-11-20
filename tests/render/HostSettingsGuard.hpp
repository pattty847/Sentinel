// Scope guard for a test that calls AgentHostMode::activate.
//
// activate() points QSettings' default format at INI files under the host directory, and
// resetForTests() sets the default format back to NATIVE (the owner's preferences plist), whatever it was
// before. A binary that isolates its settings at start-up (INI under a temporary directory) would then
// read and write the real plist for every later test. Declare this guard BEFORE the activate call:
// it records the default format and the INI user and system paths, and puts all three back after
// resetForTests().
#pragma once
#include "config/AgentHostMode.hpp"
#include <QFileInfo>
#include <QSettings>
#include <QString>

class HostSettingsGuard {
public:
    HostSettingsGuard()
        : format_(QSettings::defaultFormat()), userDir_(iniDir(QSettings::UserScope)),
          systemDir_(iniDir(QSettings::SystemScope)) {}
    ~HostSettingsGuard() {
        AgentHostMode::resetForTests();
        QSettings::setDefaultFormat(format_);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, userDir_);
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, systemDir_);
    }
    HostSettingsGuard(const HostSettingsGuard&) = delete;
    HostSettingsGuard& operator=(const HostSettingsGuard&) = delete;
    // The directory QSettings uses for INI files in `scope` (QSettings has no getter; the probe
    // names an organization only, so the file is <dir>/<organization>.ini and nothing is created).
    static QString iniDir(QSettings::Scope scope) {
        return QFileInfo(QSettings(QSettings::IniFormat, scope, "HostSettingsGuardProbe").fileName()).absolutePath();
    }
private:
    QSettings::Format format_;
    QString userDir_, systemDir_;
};
