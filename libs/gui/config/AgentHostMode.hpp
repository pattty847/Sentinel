#pragma once

#include <QString>
#include <QStringList>

// --agent-host <dir>: the GUI as run by scripts/dev/gui-host.py for sandboxed agents. The agent can
// reach the GUI's API port and can write its own worktree, so in this mode the GUI
//   - refuses screenshots that grab screen pixels (target=main; FM-120) and every unknown target,
//   - writes screenshots only to <dir>/screenshots (config and SENTINEL_GUI_SCREENSHOT_DIR ignored),
//   - keeps QSettings in the host's persistent profile/settings (INI), never the owner's preferences domain,
//   - sends no trade commands (/api/v1/input can drive the chart's TP/SL controls into the server's
//     trading session) and only switches to allowlisted symbols (a symbol change makes the recorder
//     subscribe upstream).
// The host runs only the main checkout's build (never a build an agent can write), so QML, scripts
// and plugins are the reviewed ones; this mode contains what an agent can do through the API.
// QtCore only: core code and tests can use it without a window.
namespace AgentHostMode {

// Validate `dir` and switch the mode on. Refuses (returns false, mode stays off) when `dir` is not
// absolute, is under /Volumes (the recording drive and the agent worktrees), or resolves into or
// onto one of `forbiddenRoots` (the repo checkout, the build tree) after symlinks are resolved.
bool activate(const QString& dir, const QStringList& forbiddenRoots, QString* error,
              const QString& profileSettingsDir = {});

bool active();
QString screenshotDir();          // <dir>/screenshots (canonical); empty when inactive
bool tradingAllowed();            // false when active: drop every TradeCommand at the data source
// Symbols a GUI in this mode may switch to (set from --agent-host-symbols). Active with an empty
// list means no symbol changes at all. Exact, case-sensitive match. True when inactive.
void setSymbolAllowlist(const QStringList& symbols);
bool symbolAllowed(const QString& symbol);
// The symbol a GUI should start on: `preferred` when inactive or allowed, else the first allowlisted
// symbol, else empty (stay unsubscribed). The GUI's hard-coded BTC-USD and the server's default symbol
// both go through this, and RemoteGridDataSource refuses the rest at the send boundary.
QString startupSymbol(const QString& preferred);
// True when inactive. When active: only chart and widget grabs (heatmap, lab, telemetry, toolbar,
// settings, settings:<Tab>), never screen pixels.
bool screenshotTargetAllowed(const QString& target);

void resetForTests();

}  // namespace AgentHostMode
