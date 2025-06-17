#include "mainwindow.h"
#include "websocketclient.h"
#include "statisticscontroller.h"
#include "ruleengine.h"
#include "cvdthresholdrule.h"

#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtCore/QUrl>
#include <QtCore/QMetaObject>

// Constructor
MainWindow::MainWindow(QWidget *parent)
    : QWidget(parent)
{
    // --- Create UI Elements ---
    m_logOutput = new QTextEdit(this);
    m_logOutput->setReadOnly(true);
    m_cvdLabel = new QLabel("CVD: 0.00", this);
    m_alertLabel = new QLabel("Alerts: ---", this);
    m_alertLabel->setStyleSheet("color: red;");
    m_commandInput = new QLineEdit(this);
    m_commandInput->setPlaceholderText("Enter command... (try 'help')");
    m_submitButton = new QPushButton("Submit", this);
    m_clearButton = new QPushButton("Clear Log", this);

    // --- Layout ---
    // Main vertical layout
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->addWidget(m_cvdLabel);
    layout->addWidget(m_alertLabel);
    layout->addWidget(m_logOutput);

    // Horizontal layout for command input
    QHBoxLayout *commandLayout = new QHBoxLayout();
    commandLayout->addWidget(m_commandInput);
    commandLayout->addWidget(m_submitButton);
    
    // Horizontal layout for buttons
    QHBoxLayout *buttonLayout = new QHBoxLayout();
    buttonLayout->addStretch(); // Add spacer to push button to the right
    buttonLayout->addWidget(m_clearButton);

    // Add the nested layouts to the main layout
    layout->addLayout(commandLayout);
    layout->addLayout(buttonLayout);

    // --- Create Worker Objects ---
    m_client = new WebSocketClient();
    m_statsController = new StatisticsController();
    m_ruleEngine = new RuleEngine(m_statsController->processor());
    
    m_client->moveToThread(&m_workerThread);
    m_statsController->moveToThread(&m_workerThread);
    m_ruleEngine->moveToThread(&m_workerThread);

    // --- Connections ---
    
    // 1. Connect UI controls to slots in the main thread
    connect(m_clearButton, &QPushButton::clicked, m_logOutput, &QTextEdit::clear);
    connect(m_submitButton, &QPushButton::clicked, this, &MainWindow::onCommandEntered);
    connect(m_commandInput, &QLineEdit::returnPressed, this, &MainWindow::onCommandEntered);

    // 2. Connect signals from worker objects to slots in the main thread (GUI updates)
    connect(m_client, &WebSocketClient::connected, this, &MainWindow::onConnected);
    connect(m_client, &WebSocketClient::disconnected, this, &MainWindow::onDisconnected);
    connect(m_client, &WebSocketClient::tradeReady, this, &MainWindow::onTradeReceived);
    connect(m_statsController, &StatisticsController::cvdUpdated, this, &MainWindow::onCvdUpdated);
    connect(m_ruleEngine, &RuleEngine::alertTriggered, this, &MainWindow::onAlertTriggered);

    // 3. Connect signals and slots between objects that live *inside* the worker thread.
    connect(&m_workerThread, &QThread::started, m_client, [this](){ 
        m_client->connectToServer(QUrl("wss://ws-feed.exchange.coinbase.com"));
    });
    connect(m_client, &WebSocketClient::tradeReady, m_statsController, &StatisticsController::processTrade);
    connect(m_client, &WebSocketClient::tradeReady, m_ruleEngine, &RuleEngine::onNewTrade);
    
    // 4. Connect thread finishing to the cleanup of worker objects
    connect(&m_workerThread, &QThread::finished, m_client, &QObject::deleteLater);
    connect(&m_workerThread, &QThread::finished, m_statsController, &QObject::deleteLater);
    connect(&m_workerThread, &QThread::finished, m_ruleEngine, &QObject::deleteLater);

    // --- Add Rules to Engine ---
    m_ruleEngine->addRule(std::make_unique<CvdThresholdRule>(2000.0));

    // --- Start the Worker Thread ---
    m_workerThread.start();
}

// Destructor
MainWindow::~MainWindow()
{
    // Politely ask the client to close its connection.
    // This is a thread-safe way to call a method on an object living in another thread.
    QMetaObject::invokeMethod(m_client, "closeConnection", Qt::QueuedConnection);

    // Initiate a clean shutdown of the worker thread.
    // This will cause the thread's event loop to exit.
    m_workerThread.quit();
    
    // Wait for the thread to fully finish its execution before we exit.
    // This is crucial to prevent crashes on close.
    m_workerThread.wait();
}

// Slot for when the connection is established
void MainWindow::onConnected()
{
    m_logOutput->append("Connection established! Subscribing to BTC-USD ticker...");
}

// Slot for when the connection is closed
void MainWindow::onDisconnected()
{
    m_logOutput->append("Connection closed.");
}

// Slot for when a new trade is received
// Why reference of the trade?
// Because we don't want to copy the trade object, we want to use the same object
// in the main thread and the worker thread.
// If we pass the trade object by value, it will be copied to the main thread,
// and the worker thread will not be able to use the same object.
// If we pass the trade object by reference, it will be the same object in both threads.
void MainWindow::onTradeReceived(const Trade &trade)
{
    QString sideStr = (trade.side == Side::Buy) ? "BUY" : "SELL";
    if (trade.side == Side::Unknown) {
        sideStr = "UNKNOWN";
    }

    QString formattedMessage = QString("TRADE [%1]: %2 @ $%3")
                                   .arg(sideStr)
                                   .arg(trade.size, 0, 'f', 8)
                                   .arg(trade.price, 0, 'f', 2);

    m_logOutput->append(formattedMessage);
}

void MainWindow::onCvdUpdated(double newCvd)
{
    m_cvdLabel->setText(QString("CVD: %1").arg(newCvd, 0, 'f', 4));
}

void MainWindow::onAlertTriggered(const QString &alertMessage)
{
    m_alertLabel->setText(alertMessage);
}

void MainWindow::onCommandEntered()
{
    // Get the command from the input field
    QString command = m_commandInput->text().trimmed();
    if (command.isEmpty()) {
        return;
    }

    // Echo the command to the log
    m_logOutput->append(QString("> %1").arg(command));

    // Simple command parsing
    if (command.compare("help", Qt::CaseInsensitive) == 0) {
        m_logOutput->append("Available commands:\n- help: Show this message\n- clear: Clear the log\n- status: Show connection status");
    } else if (command.compare("clear", Qt::CaseInsensitive) == 0) {
        m_logOutput->clear();
    } else if (command.compare("status", Qt::CaseInsensitive) == 0) {
        if (m_workerThread.isRunning()) {
             m_logOutput->append("Status: Worker thread running. Connection active.");
        } else {
            m_logOutput->append("Status: Worker thread stopped. Disconnected.");
        }
    }
    else {
        m_logOutput->append("Unknown command. Type 'help' for a list of commands.");
    }

    // Clear the input field for the next command
    m_commandInput->clear();
}