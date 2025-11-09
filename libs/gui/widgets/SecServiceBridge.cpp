#include "SecServiceBridge.hpp"
#include <QProcess>
#include <QDir>
#include <QStandardPaths>
#include <QFile>
#include <QThread>
#include <QDebug>

SecServiceBridge::SecServiceBridge(QObject* parent)
    : QObject(parent)
    , m_process(new QProcess(this))
    , m_serviceUrl(QString("http://localhost:%1").arg(kDefaultPort))
{
    connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &SecServiceBridge::onProcessFinished);
    connect(m_process, &QProcess::errorOccurred, this, &SecServiceBridge::onProcessError);
}

SecServiceBridge::~SecServiceBridge() {
    stopService();
}

bool SecServiceBridge::startService() {
    if (m_process->state() != QProcess::NotRunning) {
        return true;  // Already running
    }
    
    // Find Python executable
    QString pythonExe = "python3";
    #ifdef Q_OS_WIN
    pythonExe = "python";
    #endif
    
    // Find service script (should be in scripts/ directory)
    QDir appDir = QDir::current();
    QString scriptPath = appDir.absoluteFilePath("scripts/sec_service.py");
    
    if (!QFile::exists(scriptPath)) {
        emit serviceError(QString("SEC service script not found: %1").arg(scriptPath));
        return false;
    }
    
    QStringList args;
    args << scriptPath;
    
    m_process->start(pythonExe, args);
    
    if (!m_process->waitForStarted(3000)) {
        emit serviceError("Failed to start SEC service process");
        return false;
    }
    
    // Wait a bit for service to start
    QThread::msleep(500);
    emit serviceStarted();
    return true;
}

void SecServiceBridge::stopService() {
    if (m_process->state() != QProcess::NotRunning) {
        m_process->terminate();
        if (!m_process->waitForFinished(3000)) {
            m_process->kill();
        }
        emit serviceStopped();
    }
}

bool SecServiceBridge::isRunning() const {
    return m_process->state() == QProcess::Running;
}

void SecServiceBridge::onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus) {
    Q_UNUSED(exitCode);
    if (exitStatus == QProcess::CrashExit) {
        emit serviceError("SEC service crashed");
    } else {
        emit serviceStopped();
    }
}

void SecServiceBridge::onProcessError(QProcess::ProcessError error) {
    QString errorMsg;
    switch (error) {
        case QProcess::FailedToStart:
            errorMsg = "SEC service failed to start";
            break;
        case QProcess::Crashed:
            errorMsg = "SEC service crashed";
            break;
        default:
            errorMsg = "SEC service error occurred";
    }
    emit serviceError(errorMsg);
}

