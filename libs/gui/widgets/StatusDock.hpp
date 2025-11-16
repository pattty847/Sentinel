#pragma once

#include <QWidget>
#include <QLabel>
#include <QVBoxLayout>

/**
 * Status widget for central area.
 * Minimal placeholder widget - status info is now in bottom StatusBar.
 * Can be expanded later with connection dashboard features.
 * 
 * Note: This is a regular QWidget (not a dock) for use as central widget.
 */
class StatusDock : public QWidget {
    Q_OBJECT

public:
    explicit StatusDock(QWidget* parent = nullptr);
    void setupUi();

private:
    // Status labels removed - now shown in bottom StatusBar
};

