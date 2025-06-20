#include "mainwindow.h"
#include "streamcontroller.h"     // 🚀 Our new bridge!
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
    // 🚀 Create our new StreamController bridge!
    // 🐍 Python: self.stream_controller = StreamController()
    // ⚡ C++: m_streamController = new StreamController();
    m_streamController = new StreamController();
    m_statsController = new StatisticsController();
    m_ruleEngine = new RuleEngine(m_statsController->processor());
    
    // Move all worker objects to the worker thread
    // 🐍 Python: These would run in asyncio or threading
    // ⚡ C++: Qt's threading system - moveToThread()
    m_streamController->moveToThread(&m_workerThread);
    m_statsController->moveToThread(&m_workerThread);
    m_ruleEngine->moveToThread(&m_workerThread);

    // --- Connections ---
    
    // 1. Connect UI controls to slots in the main thread
    connect(m_clearButton, &QPushButton::clicked, m_logOutput, &QTextEdit::clear);
    connect(m_submitButton, &QPushButton::clicked, this, &MainWindow::onCommandEntered);
    connect(m_commandInput, &QLineEdit::returnPressed, this, &MainWindow::onCommandEntered);

    // 2. Connect signals from worker objects to slots in the main thread (GUI updates)
    // 🚀 Connect our StreamController bridge signals to the GUI!
    // 🐍 Python: self.stream_controller.on_connected = self.on_connected
    // ⚡ C++: connect(m_streamController, &StreamController::connected, this, &MainWindow::onConnected);
    connect(m_streamController, &StreamController::connected, this, &MainWindow::onConnected);
    connect(m_streamController, &StreamController::disconnected, this, &MainWindow::onDisconnected);
    connect(m_streamController, &StreamController::tradeReceived, this, &MainWindow::onTradeReceived);
    connect(m_statsController, &StatisticsController::cvdUpdated, this, &MainWindow::onCvdUpdated);
    connect(m_ruleEngine, &RuleEngine::alertTriggered, this, &MainWindow::onAlertTriggered);

    // 3. Connect signals and slots between objects that live *inside* the worker thread.
    // 🚀 Start streaming when worker thread starts!
    // 🐍 Python: async def on_thread_start(): await self.stream_controller.start(["BTC-USD"])
    // ⚡ C++: connect(&m_workerThread, &QThread::started, m_streamController, [this](){...});
    connect(&m_workerThread, &QThread::started, m_streamController, [this](){
        std::vector<std::string> symbols = {"BTC-USD", "ETH-USD"};
        m_streamController->start(symbols);
    });
    
    // Connect trade data flow between workers
    // 🐍 Python: self.stream_controller.on_trade_received = self.stats_controller.process_trade
    // ⚡ C++: connect(m_streamController, &StreamController::tradeReceived, ...);
    connect(m_streamController, &StreamController::tradeReceived, m_statsController, &StatisticsController::processTrade);
    connect(m_streamController, &StreamController::tradeReceived, m_ruleEngine, &RuleEngine::onNewTrade);
    
    // 4. Connect thread finishing to the cleanup of worker objects
    connect(&m_workerThread, &QThread::finished, m_streamController, &QObject::deleteLater);
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
    // Politely ask the stream controller to stop streaming.
    // This is a thread-safe way to call a method on an object living in another thread.
    // 🐍 Python: await self.stream_controller.stop()
    // ⚡ C++: QMetaObject::invokeMethod(m_streamController, "stop", Qt::QueuedConnection);
    QMetaObject::invokeMethod(m_streamController, "stop", Qt::QueuedConnection);

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

// 🚀 Slot for when a new trade is received from StreamController!
// 🐍 Python: def on_trade_received(self, trade):
// ⚡ C++: void MainWindow::onTradeReceived(const Trade &trade)
void MainWindow::onTradeReceived(const Trade &trade)
{
    // Convert enum to string (like Python's enum.name)
    // 🐍 Python: side_str = "BUY" if trade.side == Side.Buy else "SELL"
    // ⚡ C++: QString sideStr = (trade.side == Side::Buy) ? "BUY" : "SELL";
    QString sideStr = (trade.side == Side::Buy) ? "BUY" : "SELL";
    if (trade.side == Side::Unknown) {
        sideStr = "UNKNOWN";
    }

    // Format the message (like Python's f-strings)
    // 🐍 Python: message = f"TRADE [{side_str}]: {trade.size} @ ${trade.price}"
    // ⚡ C++: QString formattedMessage = QString("TRADE [%1]: %2 @ $%3").arg(...);
    QString formattedMessage = QString("TRADE [%1]: %2 @ $%3 ID:%4")
                                   .arg(sideStr)
                                   .arg(trade.size, 0, 'f', 8)
                                   .arg(trade.price, 0, 'f', 2)
                                   .arg(QString::fromStdString(trade.trade_id));

    // Add to GUI log (like Python's print to console)
    // 🐍 Python: self.log_output.append(message)
    // ⚡ C++: m_logOutput->append(formattedMessage);
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