#include "SymbolControlDock.hpp"
#include <QHBoxLayout>
#include <QFrame>

SymbolControlDock::SymbolControlDock(QWidget* parent)
    : DockablePanel("SymbolControlDock", "", parent)  // Empty title for seamless look
{
    // Hide title bar for seamless integration
    setTitleBarWidget(new QWidget());
    
    // Make it non-resizable and non-movable (pinned to heatmap)
    // Keep it floatable in case user wants to detach, but prevent resizing
    setFeatures(QDockWidget::DockWidgetFloatable);  // Can float but not resize
    
    buildUi();
}

void SymbolControlDock::buildUi() {
    QHBoxLayout* layout = new QHBoxLayout(m_contentWidget);
    layout->setContentsMargins(8, 4, 8, 4);  // Minimal padding
    layout->setSpacing(8);
    
    // Symbol label (minimal)
    QLabel* symbolLabel = new QLabel("Symbol:", m_contentWidget);
    symbolLabel->setStyleSheet("QLabel { color: #ccc; font-size: 11px; }");
    layout->addWidget(symbolLabel);
    
    // Symbol input (compact, like SEC ticker)
    m_symbolInput = new QLineEdit("BTC-USD", m_contentWidget);
    m_symbolInput->setStyleSheet(
        "QLineEdit { "
        "  padding: 4px 8px; "
        "  font-size: 11px; "
        "  background-color: #2a2a2a; "
        "  border: 1px solid #444; "
        "  border-radius: 3px; "
        "  color: white; "
        "} "
        "QLineEdit:focus { border: 1px solid #00aaff; }"
    );
    m_symbolInput->setMaximumWidth(120);
    layout->addWidget(m_symbolInput);
    
    // Subscribe button (compact)
    m_subscribeButton = new QPushButton("Subscribe", m_contentWidget);
    m_subscribeButton->setStyleSheet(
        "QPushButton { "
        "  padding: 4px 12px; "
        "  font-size: 11px; "
        "  background-color: #00aaff; "
        "  color: white; "
        "  border: none; "
        "  border-radius: 3px; "
        "} "
        "QPushButton:hover { background-color: #0088cc; } "
        "QPushButton:pressed { background-color: #006699; }"
    );
    layout->addWidget(m_subscribeButton);
    
    layout->addStretch();
    
    // Status label (minimal, right-aligned)
    m_statusLabel = new QLabel("Disconnected", m_contentWidget);
    m_statusLabel->setStyleSheet("QLabel { color: #ff4444; font-size: 10px; }");
    layout->addWidget(m_statusLabel);
    
    m_contentWidget->setLayout(layout);
    
    // Set fixed height for compact appearance
    setMaximumHeight(32);
    setMinimumHeight(32);
}

