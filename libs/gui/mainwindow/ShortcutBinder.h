/*
Sentinel — ShortcutBinder
Role: Registers keyboard shortcuts and binds them to callbacks.
Inputs/Outputs: Takes callbacks for layout operations and dock toggles, registers shortcuts.
Threading: Runs on main GUI thread.
Performance: Setup-only, not on hot path.
Integration: Called from MainWindowGPU::setupShortcuts().
Observability: None (shortcuts are silent).
Related: MainWindowGpu.cpp, LayoutManager.hpp.
*/
#pragma once

#include <QWidget>
#include <QShortcut>
#include <functional>

// Forward declarations
class HeatmapDock;
class MarketDataPanel;
class SecFilingDock;

class ShortcutBinder {
public:
    struct Callbacks {
        std::function<void()> saveLayout;
        std::function<void()> restoreLayout;
        std::function<void()> resetLayout;
    };

    struct DockWidgets {
        HeatmapDock* heatmapDock = nullptr;
        MarketDataPanel* marketDataDock = nullptr;
        SecFilingDock* secDock = nullptr;
    };

    explicit ShortcutBinder(QWidget* parent);
    
    void bindShortcuts(const Callbacks& callbacks, const DockWidgets& docks);
    
private:
    QWidget* m_parent;
};

