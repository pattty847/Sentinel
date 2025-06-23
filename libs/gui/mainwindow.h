#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QtWidgets/QWidget>
#include <QThread> // Include QThread for multithreading

// Forward declarations to avoid including full headers
// Note: We cannot forward-declare a struct, so we must include the header
#include "tradedata.h"
class QTextEdit;
class QPushButton;
class StreamController;      // 🚀 Our new bridge!
class StatisticsController; // Forward-declare our new controller
class RuleEngine; // Forward-declare our new engine
class QLabel; // Forward-declare the QLabel for our new display
class QLineEdit;
class TradeChartWidget; // Our new custom chart widget!

/**
 * MainWindow class: The main user interface for our Sentinel application
 * 
 * Inherits from QWidget, making it a Qt widget that can be displayed on screen.
 * This class is responsible for:
 * - Creating and managing the user interface elements (text box, button)
 * - Creating and managing a worker thread for all networking and data processing
 * - Receiving processed data back from the worker thread via signals and slots
 * - Handling user interactions (like clearing the log)
 */
class MainWindow : public QWidget
{
    Q_OBJECT

public:
    /**
     * Constructor: Creates a new MainWindow
     * @param parent - Pointer to the parent widget (can be nullptr for top-level windows)
     *                 If provided, this widget becomes a child of the parent and will be
     *                 automatically destroyed when the parent is destroyed
     */
    MainWindow(QWidget *parent = nullptr);
    
    /**
     * Destructor: Called when the MainWindow is being destroyed
     * Ensures clean shutdown of the worker thread before the application exits.
     */
    ~MainWindow();

private slots:
    /**
     * Slot: Called when WebSocketClient successfully connects to the server
     */
    void onConnected();
    
    /**
     * Slot: Called when WebSocketClient disconnects from the server
     */
    void onDisconnected();

    /**
     * Slot: Called when CVD is updated
     * @param newCvd - The new CVD value
     */
    void onCvdUpdated(double newCvd); // Slot to receive CVD updates

    /**
     * Slot: Called when a new alert is triggered
     * @param alertMessage - The message of the triggered alert
     */
    void onAlertTriggered(const QString &alertMessage); // Slot for alerts

    /**
     * @brief Slot to handle commands entered by the user.
     */
    void onCommandEntered();

private:
    // --- UI Elements (Live in the Main/GUI thread) ---
    // Why use m as a prefix for variables?
    // It's a convention in C++ to use m_ as a prefix for member variables.
    // It's not required, but it's a good practice to make it clear that the variable is a member of the class.
    // It's also a good practice to use m_ as a prefix for member variables that are pointers.
    TradeChartWidget *m_chart;   // The new custom chart widget
    QPushButton *m_clearButton;  // Button to clear the log display
    QLabel *m_cvdLabel;          // Label to display the live CVD
    QLabel *m_alertLabel;        // Label to display the latest alert
    QLineEdit *m_commandInput;   // Input box for user commands
    QPushButton *m_submitButton; // Button to submit commands

    // --- Worker Objects (Will be moved to a separate thread) ---
    StreamController *m_streamController;    // 🚀 Our new bridge to CoinbaseStreamClient!
    StatisticsController *m_statsController; // Qt wrapper for our stats processor
    RuleEngine *m_ruleEngine; // Our new rule engine

    // --- Threading ---
    QThread m_workerThread;      // The thread that our worker objects will live in
};
#endif // MAINWINDOW_H 