#include "CommentaryFeedDock.hpp"
#include <QVBoxLayout>
#include <QScrollBar>
#include <QDateTime>

CommentaryFeedDock::CommentaryFeedDock(const QString& id, const QString& title, QWidget* parent)
    : DockablePanel(id, title, parent)
{
    buildUi();
}

void CommentaryFeedDock::buildUi() {
    QVBoxLayout* layout = new QVBoxLayout(m_contentWidget);
    
    m_messageDisplay = new QTextEdit(m_contentWidget);
    m_messageDisplay->setReadOnly(true);
    m_messageDisplay->setStyleSheet("QTextEdit { background-color: #1a1a1a; color: #e0e0e0; font-family: monospace; }");
    
    layout->addWidget(m_messageDisplay);
    m_contentWidget->setLayout(layout);
}

void CommentaryFeedDock::appendMessage(const QString& source, const QString& text) {
    QString timestamp = QDateTime::currentDateTime().toString("HH:mm:ss");
    QString formatted = QString("[%1] %2: %3\n").arg(timestamp, source, text);
    
    m_messageDisplay->append(formatted);
    
    // Auto-scroll to bottom
    QScrollBar* scrollBar = m_messageDisplay->verticalScrollBar();
    scrollBar->setValue(scrollBar->maximum());
    
    pruneOldMessages();
}

void CommentaryFeedDock::pruneOldMessages() {
    QString content = m_messageDisplay->toPlainText();
    QStringList lines = content.split('\n', Qt::SkipEmptyParts);
    
    if (lines.size() > kMaxMessages) {
        // Keep only the most recent messages
        lines = lines.mid(lines.size() - kMaxMessages);
        m_messageDisplay->setPlainText(lines.join('\n') + '\n');
    }
}

