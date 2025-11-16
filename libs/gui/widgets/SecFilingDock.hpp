#pragma once

#include "DockablePanel.hpp"
#include "SecApiClient.hpp"
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
    void onFilingsReady(const QList<SecApiClient::Filing>& filings);
    void onTransactionsReady(const QList<SecApiClient::Transaction>& transactions);
    void onFinancialsReady(const QList<SecApiClient::FinancialMetric>& metrics);
    void onApiError(const QString& error);
    void onStatusUpdate(const QString& message);

private:
    void updateStatus(const QString& message, bool isError = false);
    void displayFilings(const QList<SecApiClient::Filing>& filings);
    void displayTransactions(const QList<SecApiClient::Transaction>& transactions);
    void displayFinancials(const QList<SecApiClient::FinancialMetric>& metrics);
    
    SecApiClient* m_apiClient;
    
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
};

