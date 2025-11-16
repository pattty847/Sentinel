#include "AICommentaryFeedDock.hpp"
#include <QDateTime>
#include <QColor>
#include <QScrollBar>
#include <QStringList>

AICommentaryFeedDock::AICommentaryFeedDock(QWidget* parent)
    : CommentaryFeedDock("AICommentaryFeedDock", "AI Commentary", parent)
{
    // Set AI-specific styling (purple/cyan scheme)
    m_messageDisplay->setStyleSheet(
        "QTextEdit { background-color: #1a1a1a; color: #ff00ff; font-family: monospace; }"
    );
}

void AICommentaryFeedDock::appendMessage(const QString& source, const QString& text) {
    // Use purple/cyan color scheme for AI commentary
    QString timestamp = QDateTime::currentDateTime().toString("HH:mm");
    
    // Extract symbol if present in text
    QString formatted;
    if (text.contains('-')) {
        // Assume format: "SYMBOL message"
        QStringList parts = text.split(' ', Qt::SkipEmptyParts);
        if (parts.size() > 1) {
            QString symbol = parts.first();
            QString message = parts.mid(1).join(' ');
            formatted = QString("[%1] %2 %3\n").arg(timestamp, symbol, message);
        } else {
            formatted = QString("[%1] %2\n").arg(timestamp, text);
        }
    } else {
        formatted = QString("[%1] %2\n").arg(timestamp, text);
    }
    
    m_messageDisplay->setTextColor(QColor(255, 0, 255));  // Magenta/Purple
    m_messageDisplay->append(formatted);
    
    // Auto-scroll
    QScrollBar* scrollBar = m_messageDisplay->verticalScrollBar();
    scrollBar->setValue(scrollBar->maximum());
    
    pruneOldMessages();
}

