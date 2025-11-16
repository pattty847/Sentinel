#include "HeatmapDock.hpp"
#include <QQuickView>
#include <QSurfaceFormat>
#include <QSGRendererInterface>
#include <QQuickWindow>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFrame>

HeatmapDock::HeatmapDock(QWidget* parent)
    : DockablePanel("HeatmapDock", "Heatmap", parent)
{
    buildUi();
}

void HeatmapDock::buildUi() {
    // Create main vertical layout for heatmap + symbol control
    QVBoxLayout* mainLayout = new QVBoxLayout(m_contentWidget);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);
    
    // Create QQuickView with threaded render loop configuration
    m_qquickView = new QQuickView;
    m_qquickView->setPersistentSceneGraph(true);
    m_qquickView->setResizeMode(QQuickView::SizeRootObjectToView);
    m_qquickView->setColor(Qt::black);
    
    // Configure surface format for optimal GPU performance
    QSurfaceFormat format = QSurfaceFormat::defaultFormat();
    m_qquickView->setFormat(format);
    
    // Create container widget for QML
    m_qmlContainer = QWidget::createWindowContainer(m_qquickView, m_contentWidget);
    m_qmlContainer->setFocusPolicy(Qt::StrongFocus);
    mainLayout->addWidget(m_qmlContainer, 1);  // Takes most space
    
    // Create embedded symbol control bar (seamless, no title bar look)
    QWidget* symbolBar = new QWidget(m_contentWidget);
    symbolBar->setFixedHeight(32);
    symbolBar->setStyleSheet(
        "QWidget { background-color: #1e1e1e; border-top: 1px solid #333; }"
    );
    
    QHBoxLayout* symbolLayout = new QHBoxLayout(symbolBar);
    symbolLayout->setContentsMargins(8, 4, 8, 4);
    symbolLayout->setSpacing(8);
    
    // Symbol label
    QLabel* symbolLabel = new QLabel("Symbol:", symbolBar);
    symbolLabel->setStyleSheet("QLabel { color: #ccc; font-size: 11px; }");
    symbolLayout->addWidget(symbolLabel);
    
    // Symbol input
    m_symbolInput = new QLineEdit("BTC-USD", symbolBar);
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
    symbolLayout->addWidget(m_symbolInput);
    
    // Subscribe button
    m_subscribeButton = new QPushButton("Subscribe", symbolBar);
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
    symbolLayout->addWidget(m_subscribeButton);
    
    // Status label removed - now shown in bottom status bar
    
    // Add symbol bar to main layout (fixed height at bottom)
    mainLayout->addWidget(symbolBar, 0);
    
    m_contentWidget->setLayout(mainLayout);
}

