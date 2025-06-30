#include "mainwindow.h"
#include "streamcontroller.h"
#include "statisticscontroller.h"
#include "ruleengine.h"
#include "cvdthresholdrule.h"
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QGroupBox>
#include "Log.hpp"
#include <QQmlContext>
#include <vector>
#include <string>

static constexpr auto CAT = "MainWindow";

MainWindow::MainWindow(QWidget *parent)
    : QWidget(parent)
{
    LOG_I(CAT, "🚀 MAINWINDOW CREATING GPU CHART BEAST!");
    
    setupUI();
    setupConnections();
    
    LOG_I(CAT, "✅ MainWindow initialized with GPU chart");
}

MainWindow::~MainWindow() = default;

void MainWindow::setupUI() {
    // 🔥 CREATE GPU CHART WIDGET (QML)
    m_gpuChart = new QQuickWidget(this);
    m_gpuChart->setResizeMode(QQuickWidget::SizeRootObjectToView);
    m_gpuChart->setSource(QUrl("qrc:/qml/DepthChartView.qml"));
    
    // Set BTC-USD as default symbol
    QQmlContext* context = m_gpuChart->rootContext();
    context->setContextProperty("symbol", "BTC-USD");
    
    // Other UI elements
    m_cvdLabel = new QLabel("CVD: N/A", this);
    m_alertLabel = new QLabel("Alerts: ---", this);
    m_commandInput = new QLineEdit(this);
    m_commandInput->setPlaceholderText("e.g., BTC-USD,ETH-USD");
    m_submitButton = new QPushButton("Subscribe", this);

    // Style the labels
    m_cvdLabel->setStyleSheet("QLabel { color : white; font-size: 16px; }");
    m_alertLabel->setStyleSheet("QLabel { color : yellow; font-size: 16px; }");

    // Layout
    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->addWidget(m_gpuChart, 1); // GPU chart takes up most space

    QGroupBox *statsGroup = new QGroupBox("Live Stats");
    QHBoxLayout *statsLayout = new QHBoxLayout();
    statsLayout->addWidget(m_cvdLabel);
    statsLayout->addWidget(m_alertLabel);
    statsGroup->setLayout(statsLayout);
    
    QGroupBox *controlGroup = new QGroupBox("Controls");
    QHBoxLayout *controlLayout = new QHBoxLayout();
    controlLayout->addWidget(m_commandInput);
    controlLayout->addWidget(m_submitButton);
    controlGroup->setLayout(controlLayout);

    mainLayout->addWidget(statsGroup);
    mainLayout->addWidget(controlGroup);
    setLayout(mainLayout);
}

void MainWindow::setupConnections() {
    // Worker Thread Setup
    m_streamController = new StreamController();
    m_statsController = new StatisticsController();
    
    // Connect UI signals
    connect(m_submitButton, &QPushButton::clicked, this, &MainWindow::onSubscribe);
    
    // Connect worker signals
    connect(m_streamController, &StreamController::connected, this, []() {
        LOG_I(CAT, "✅ StreamController connected");
    });
    
    connect(m_streamController, &StreamController::disconnected, this, []() {
        LOG_W(CAT, "❌ StreamController disconnected");
    });
    
    // 🚀 TODO: Connect to GPU chart (Phase 1)
    // For now, GPU chart generates its own test data
    // connect(m_streamController, &StreamController::tradeReceived, 
    //         m_gpuChart, &GPUChartWidget::addTrade);
    
    connect(m_statsController, &StatisticsController::cvdUpdated, 
            this, &MainWindow::onCVDUpdated);
}

void MainWindow::onSubscribe() {
    QString input = m_commandInput->text().trimmed();
    if (input.isEmpty()) {
        LOG_W(CAT, "❌ Empty input");
        return;
    }
    
    // Parse symbols
    QStringList symbols = input.split(',');
    std::vector<std::string> symbolsVec;
    for (const QString& symbol : symbols) {
        symbolsVec.push_back(symbol.trimmed().toStdString());
    }
    
    LOG_I(CAT, "🚀 Subscribing to symbols: {}", input.toStdString());
    m_streamController->start(symbolsVec);
}

void MainWindow::onCVDUpdated(double cvd) {
    m_cvdLabel->setText(QString("CVD: %1").arg(cvd, 0, 'f', 2));
}

void MainWindow::onAlert(const QString& message) {
    m_alertLabel->setText(QString("Alert: %1").arg(message));
}
