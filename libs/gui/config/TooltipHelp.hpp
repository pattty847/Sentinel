#pragma once
#include <QObject>
#include <QString>

class QWidget;

// GUI-thread app preference. Models select text on demand; widget bindings retain both
// forms so a mode change refreshes even hidden/idle docks without waiting for market data.
class TooltipHelp : public QObject {
    Q_OBJECT
public:
    explicit TooltipHelp(QObject* parent = nullptr);
    static TooltipHelp& instance();
    bool explanatory() const { return m_explanatory; }
    void setExplanatory(bool enabled);
    QString text(const QString& shortForm, const QString& longForm) const;
    void setToolTip(QWidget* widget, const QString& shortForm, const QString& longForm);
signals:
    void changed(bool enabled);
private:
    bool m_explanatory = false;
};
