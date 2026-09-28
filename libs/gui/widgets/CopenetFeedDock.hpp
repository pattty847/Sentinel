#pragma once

#include "CommentaryFeedDock.hpp"

/**
 * COPENET commentary feed widget.
 * Displays market commentary and social sentiment.
 */
class CopenetFeedDock : public CommentaryFeedDock {
    Q_OBJECT

public:
    explicit CopenetFeedDock(QWidget* parent = nullptr);
    
    // Override to use COPENET-specific formatting
    void appendMessage(const QString& source, const QString& text) override;
};

