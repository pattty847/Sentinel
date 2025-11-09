#pragma once

#include "DockablePanel.hpp"
#include "SecServiceBridge.hpp"
#include <QLineEdit>
#include <QComboBox>
#include <QPushButton>
#include <QTableView>
#include <QTextEdit>
#include <QLabel>
#include <QStandardItemModel>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QHash>

/**
 * SEC Filing Viewer dock widget.
 * Displays SEC filings, insider transactions, and financial summaries.
 */
class SecFilingDock : public DockablePanel {
    Q_OBJECT

public:
    explicit SecFilingDock(QWidget* parent = nullptr);
    void buildUi() override;
    void onSymbolChanged(const QString& symbol) override;

private slots:
    void fetchFilings();
    void fetchInsiderTransactions();
    void fetchFinancialSummary();
    void onFilingsReply();
    void onTransactionsReply();
    void onFinancialsReply();
    void onServiceError(const QString& error);

private:
    void updateStatus(const QString& message, bool isError = false);
    void parseFilings(const QJsonDocument& doc);
    void parseTransactions(const QJsonDocument& doc);
    void parseFinancials(const QJsonDocument& doc);
    
    SecServiceBridge* m_serviceBridge;
    QNetworkAccessManager* m_networkManager;
    
    QLineEdit* m_tickerInput;
    QComboBox* m_formTypeCombo;
    QPushButton* m_fetchFilingsBtn;
    QPushButton* m_fetchInsiderBtn;
    QPushButton* m_fetchFinancialsBtn;
    
    QTableView* m_filingsTable;
    QStandardItemModel* m_filingsModel;
    
    QTableView* m_transactionsTable;
    QStandardItemModel* m_transactionsModel;
    
    QTextEdit* m_financialsDisplay;
    QLabel* m_statusLabel;
    
    QHash<QString, QJsonDocument> m_cache;
};

