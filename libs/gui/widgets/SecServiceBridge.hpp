#pragma once

#include <QObject>
#include <QProcess>
#include <QString>
#include <QUrl>

/**
 * Bridge to Python SEC service.
 * Spawns Python process and manages HTTP communication.
 */
class SecServiceBridge : public QObject {
    Q_OBJECT

public:
    explicit SecServiceBridge(QObject* parent = nullptr);
    ~SecServiceBridge();
    
    bool startService();
    void stopService();
    bool isRunning() const;
    
    QUrl serviceUrl() const { return m_serviceUrl; }

signals:
    void serviceStarted();
    void serviceStopped();
    void serviceError(const QString& error);

private slots:
    void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onProcessError(QProcess::ProcessError error);

private:
    QProcess* m_process;
    QUrl m_serviceUrl;
    static constexpr int kDefaultPort = 8765;
};

