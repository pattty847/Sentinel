#pragma once

#include <QString>
#include <QStringList>

// --agent-host <dir>: the GUI as run by scripts/dev/gui-host.py for sandboxed agents. The agent can
// reach the GUI's API port and can write its own worktree, so in this mode the GUI
//   - refuses screenshots that grab screen pixels (target=main; FM-120) and every unknown target,
//   - writes screenshots only to <dir>/screenshots (config and SENTINEL_GUI_SCREENSHOT_DIR ignored),
//   - keeps QSettings in <dir>/settings (INI), never the owner's preferences domain,
//   - loads QML only from the binary's embedded resources (QML runs JavaScript; the build-time
//     source directory and the import path next to the build are agent-writable in a worktree).
// QtCore only: core code and tests can use it without a window.
namespace AgentHostMode {

// Validate `dir` and switch the mode on. Refuses (returns false, mode stays off) when `dir` is not
// absolute, is under /Volumes (the recording drive and the agent worktrees), or resolves into or
// onto one of `forbiddenRoots` (the repo checkout, the build tree) after symlinks are resolved.
bool activate(const QString& dir, const QStringList& forbiddenRoots, QString* error);

bool active();
QString screenshotDir();          // <dir>/screenshots (canonical); empty when inactive
bool embeddedQmlOnly();           // true when active
// True when inactive. When active: only chart and widget grabs (heatmap, lab, telemetry, toolbar,
// settings, settings:<Tab>), never screen pixels.
bool screenshotTargetAllowed(const QString& target);

// URL of a Sentinel.Charts QML file inside the binary. qt6_add_qml_module keeps the `qml/` directory
// of QML_FILES in the resource path; the loaders' older "qrc:/Sentinel/Charts/X.qml" and
// ":/qt/qml/Sentinel/Charts/X.qml" never existed, which is why every normal run loads QML from disk.
QString embeddedQmlUrl(const QString& file);

void resetForTests();

}  // namespace AgentHostMode
