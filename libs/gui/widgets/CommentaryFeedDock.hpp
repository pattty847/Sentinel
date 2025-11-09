#pragma once

#include "DockablePanel.hpp"
#include <QTextEdit>
#include <QVBoxLayout>
#include <QDateTime>
#include <QScrollBar>

/**
 * Base class for commentary feed widgets.
 * Provides message display, timestamp formatting, and auto-scroll.
 */
class CommentaryFeedDock : public DockablePanel {
    Q_OBJECT

public:
    explicit CommentaryFeedDock(const QString& id, const QString& title, QWidget* parent = nullptr);
    void buildUi() override;
    
    /**
     * Append a message to the feed.
     * @param source Source identifier (e.g., "COPENET", "AI")
     * @param text Message text
     */
    virtual void appendMessage(const QString& source, const QString& text);

protected:
    QTextEdit* m_messageDisplay;
    static constexpr int kMaxMessages = 1000;
    
    void pruneOldMessages();
};

