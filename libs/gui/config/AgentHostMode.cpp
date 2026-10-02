#include "AgentHostMode.hpp"

#include <QDir>
#include <QFileInfo>
#include <QSettings>

namespace AgentHostMode {
namespace {

bool g_active = false;
QString g_screenshotDir;
QStringList g_symbols;

bool underOrEqual(const QString& path, const QString& root) {
    return path == root || path.startsWith(root.endsWith('/') ? root : root + '/');
}

// Canonical form of a path that may not exist yet: resolve the nearest existing ancestor (symlinks
// included) and append the rest. Empty when nothing on the path can be resolved.
QString resolveMaybeMissing(const QString& path) {
    QString existing = QDir::cleanPath(path), rest;
    while (!QFileInfo::exists(existing)) {
        const int slash = existing.lastIndexOf('/');
        if (slash <= 0) return {};
        rest = existing.mid(slash) + rest;
        existing = existing.left(slash);
    }
    const QString base = QFileInfo(existing).canonicalFilePath();
    return base.isEmpty() ? QString() : base + rest;
}

}  // namespace

bool activate(const QString& dir, const QStringList& forbiddenRoots, QString* error) {
    auto fail = [&](const QString& why) {
        if (error) *error = why;
        return false;
    };
    if (!QDir::isAbsolutePath(dir)) return fail("--agent-host needs an absolute directory: " + dir);
    if (underOrEqual(QDir::cleanPath(dir), "/Volumes"))
        return fail("--agent-host directory must not be under /Volumes (recordings, agent worktrees): " + dir);
    // Resolve symlinks BEFORE the root checks, or a link into the repo would pass, and check BEFORE
    // creating anything, or a refused directory would still be left behind (in the repo). The
    // directory may not exist yet: resolve its nearest existing ancestor and append the rest.
    const QString canonical = resolveMaybeMissing(dir);
    if (canonical.isEmpty()) return fail("cannot resolve --agent-host directory: " + dir);
    if (underOrEqual(canonical, "/Volumes"))
        return fail("--agent-host directory resolves under /Volumes: " + canonical);
    for (const QString& root : forbiddenRoots) {
        if (root.isEmpty()) continue;
        const QString canonicalRoot = resolveMaybeMissing(root);
        if (!canonicalRoot.isEmpty() && underOrEqual(canonical, canonicalRoot))
            return fail("--agent-host directory is inside " + canonicalRoot + " (agent-writable): " + canonical);
    }
    if (!QDir().mkpath(canonical)) return fail("cannot create --agent-host directory: " + canonical);
    const QString shots = canonical + "/screenshots";
    const QString settings = canonical + "/settings";
    if (!QDir().mkpath(shots) || !QDir().mkpath(settings))
        return fail("cannot create screenshots/settings under " + canonical);

    // Every QSettings("org", "app") in the GUI uses the process default format; point it at INI
    // files under the session directory so no run touches the owner's preferences plist.
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings);
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settings);

    g_screenshotDir = shots;
    g_active = true;
    return true;
}

bool active() { return g_active; }
QString screenshotDir() { return g_active ? g_screenshotDir : QString(); }
bool tradingAllowed() { return !g_active; }
void setSymbolAllowlist(const QStringList& symbols) { g_symbols = symbols; }
bool symbolAllowed(const QString& symbol) { return !g_active || g_symbols.contains(symbol); }
QString startupSymbol(const QString& preferred) {
    if (!g_active || g_symbols.contains(preferred)) return preferred;
    return g_symbols.isEmpty() ? QString() : g_symbols.first();
}

bool screenshotTargetAllowed(const QString& target) {
    if (!g_active) return true;
    if (target == "heatmap" || target == "lab" || target == "telemetry" || target == "toolbar" || target == "settings")
        return true;
    // settings:<Tab>, a plain word (the codec validates the tab name; reject path-like text here)
    if (target.startsWith("settings:")) {
        const QString tab = target.mid(9);
        if (tab.isEmpty()) return false;
        for (const QChar c : tab)
            if (!c.isLetter()) return false;
        return true;
    }
    return false;
}

void resetForTests() {
    g_active = false;
    g_screenshotDir.clear();
    g_symbols.clear();
    QSettings::setDefaultFormat(QSettings::NativeFormat);
}

}  // namespace AgentHostMode
