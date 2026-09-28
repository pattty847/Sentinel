#pragma once

#include "CommentaryFeedDock.hpp"

/**
 * AI Commentary feed widget.
 * Displays AI-generated market commentary.
 */
class AICommentaryFeedDock : public CommentaryFeedDock {
    Q_OBJECT

public:
    explicit AICommentaryFeedDock(QWidget* parent = nullptr);
    
    // Override to use AI-specific formatting
    void appendMessage(const QString& source, const QString& text) override;
};

