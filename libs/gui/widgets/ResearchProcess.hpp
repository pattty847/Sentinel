#pragma once
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QTimer>

// One bounded, asynchronous research request. Results carry the caller's identity;
// cancellation never waits on the GUI thread. Virtual seam supports fixture providers.
class ResearchProcess : public QObject {
    Q_OBJECT
public:
    explicit ResearchProcess(QObject* parent = nullptr) : QObject(parent) {}
    ~ResearchProcess() override;
    virtual void run(quint64 request, const QString& script, const QStringList& arguments);
    virtual void cancel();
    static QString scriptsPath();
    static bool isEquityTicker(const QString& ticker);
signals:
    void completed(quint64 request, const QByteArray& output, const QString& error);
private:
    QPointer<QProcess> m_process;
};
