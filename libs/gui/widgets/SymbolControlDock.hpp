#pragma once

#include "DockablePanel.hpp"
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QHBoxLayout>

/**
 * Minimal symbol control dock - seamless, compact controls below heatmap.
 * Matches the style of SEC ticker input for consistency.
 */
class SymbolControlDock : public DockablePanel {
    Q_OBJECT

public:
    explicit SymbolControlDock(QWidget* parent = nullptr);
    ~SymbolControlDock() override = default;
    
    void buildUi() override;
    
    QLineEdit* symbolInput() const { return m_symbolInput; }
    QPushButton* subscribeButton() const { return m_subscribeButton; }
    QLabel* statusLabel() const { return m_statusLabel; }

private:
    QLineEdit* m_symbolInput;
    QPushButton* m_subscribeButton;
    QLabel* m_statusLabel;
};

