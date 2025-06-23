#include "mainwindow.h"
#include "streamcontroller.h"
#include "statisticscontroller.h"
#include "ruleengine.h"
#include "cvdthresholdrule.h"
#include "tradechartwidget.h"
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QGroupBox>
#include <QDebug>
#include <vector>
#include <string>

MainWindow::MainWindow(QWidget *parent)
    : QWidget(parent)
{
    // --- Create UI Elements ---
    m_chart = new TradeChartWidget(this);
    m_chart->setSymbol("BTC-USD"); // Set a default symbol for the chart on startup
    m_cvdLabel = new QLabel("CVD: N/A", this);
    m_alertLabel = new QLabel("Alerts: ---", this);
    m_commandInput = new QLineEdit(this);
    m_commandInput->setPlaceholderText("e.g., BTC-USD,ETH-USD");
    m_submitButton = new QPushButton("Subscribe", this);

    // Style the labels
    m_cvdLabel->setStyleSheet("QLabel { color : white; font-size: 16px; }");
    m_alertLabel->setStyleSheet("QLabel { color : yellow; font-size: 16px; }");

    // --- Layout ---
    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->addWidget(m_chart, 1); // Chart takes up most space

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

    // --- Worker Thread Setup ---
    m_streamController = new StreamController();
    m_statsController = new StatisticsController();
    m_ruleEngine = new RuleEngine(m_statsController->processor());
    
    // Add a simple rule
    m_ruleEngine->addRule(std::make_unique<CvdThresholdRule>(1000.0));

    // Move worker objects to the thread
    m_streamController->moveToThread(&m_workerThread);
    m_statsController->moveToThread(&m_workerThread);
    m_ruleEngine->moveToThread(&m_workerThread);

    // --- Connections ---
    
    // Command connections
    connect(m_submitButton, &QPushButton::clicked, this, &MainWindow::onCommandEntered);
    connect(m_commandInput, &QLineEdit::returnPressed, this, &MainWindow::onCommandEntered);

    // Worker thread management
    connect(&m_workerThread, &QThread::finished, m_streamController, &QObject::deleteLater);
    connect(&m_workerThread, &QThread::finished, m_statsController, &QObject::deleteLater);
    connect(&m_workerThread, &QThread::finished, m_ruleEngine, &QObject::deleteLater);
    
    // StreamController -> MainWindow UI
    connect(m_streamController, &StreamController::connected, this, &MainWindow::onConnected);
    connect(m_streamController, &StreamController::disconnected, this, &MainWindow::onDisconnected);
    
    // StreamController -> Data Processors
    connect(m_streamController, &StreamController::tradeReceived, m_statsController, &StatisticsController::processTrade);
    connect(m_streamController, &StreamController::tradeReceived, m_ruleEngine, &RuleEngine::onNewTrade);

    // StreamController -> Chart
    connect(m_streamController, &StreamController::tradeReceived, m_chart, &TradeChartWidget::addTrade);
    connect(m_streamController, &StreamController::orderBookUpdated, m_chart, &TradeChartWidget::updateOrderBook);

    // Data Processors -> MainWindow UI
    connect(m_statsController, &StatisticsController::cvdUpdated, this, &MainWindow::onCvdUpdated);
    connect(m_ruleEngine, &RuleEngine::alertTriggered, this, &MainWindow::onAlertTriggered);

    // --- Start the Worker Thread ---
    m_workerThread.start();
    
    // Set initial size
    resize(1200, 800);
    
    // Automatically start the stream with the default symbol
    onCommandEntered();
}

MainWindow::~MainWindow()
{
    m_workerThread.quit();
    m_workerThread.wait();
}

void MainWindow::onConnected() {
    qDebug() << "MainWindow received connected signal";
    m_commandInput->setEnabled(false);
    m_submitButton->setText("Connected");
    m_submitButton->setEnabled(false);
}

void MainWindow::onDisconnected() {
    qDebug() << "MainWindow received disconnected signal";
    m_commandInput->setEnabled(true);
    m_submitButton->setText("Subscribe");
    m_submitButton->setEnabled(true);
}

void MainWindow::onCvdUpdated(double newCvd) {
    m_cvdLabel->setText(QString("CVD: %1").arg(newCvd, 0, 'f', 2));
}

void MainWindow::onAlertTriggered(const QString &alertMessage) {
    m_alertLabel->setText(QString("Alert: %1").arg(alertMessage));
}

void MainWindow::onCommandEntered() {
    QString command = m_commandInput->text().toUpper().trimmed();
    if (command.isEmpty()) {
        command = "BTC-USD"; // Default to BTC-USD if empty
    }
    
    QStringList symbols = command.split(',', Qt::SkipEmptyParts);
    std::vector<std::string> symbolVec;
    for (const auto& s : symbols) {
        symbolVec.push_back(s.trimmed().toStdString());
    }

    if (!symbolVec.empty()) {
        m_chart->setSymbol(symbolVec[0]); // Ensure chart is always aware of the symbol
        QMetaObject::invokeMethod(m_streamController, "start", Qt::QueuedConnection, Q_ARG(std::vector<std::string>, symbolVec));
    }
}
