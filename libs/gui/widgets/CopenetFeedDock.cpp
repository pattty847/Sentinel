#include "CopenetFeedDock.hpp"
#include <QDateTime>
#include <QColor>
#include <QScrollBar>

CopenetFeedDock::CopenetFeedDock(QWidget* parent)
    : CommentaryFeedDock("CopenetFeedDock", "COPENET", parent)
{
    // Set COPENET-specific styling
    m_messageDisplay->setStyleSheet(
        "QTextEdit { background-color: #1a1a1a; color: #00ffff; font-family: monospace; }"
    );
}

void CopenetFeedDock::appendMessage(const QString& source, const QString& text) {
    // Use teal/orange color scheme for COPENET
    QString timestamp = QDateTime::currentDateTime().toString("HH:mm");
    QString formatted = QString("[%1] %2\n").arg(timestamp, text);
    
    m_messageDisplay->setTextColor(QColor(0, 255, 255));  // Teal
    m_messageDisplay->append(formatted);
    
    // Auto-scroll
    QScrollBar* scrollBar = m_messageDisplay->verticalScrollBar();
    scrollBar->setValue(scrollBar->maximum());
    
    pruneOldMessages();
}

