#include "ResearchProcess.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <memory>
#include <QVariant>
#include <cerrno>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <vector>
#endif
#ifdef Q_OS_UNIX
#include <signal.h>
#include <unistd.h>
#endif

namespace {
#ifdef Q_OS_WIN
// Assign the job at creation, before uv can spawn Python. Closing the job also
// reaps descendants if the launcher exits before its children.
struct ResearchJob {
    HANDLE handle = nullptr;
    STARTUPINFOEXW startup{};
    std::vector<unsigned char> attributes;
    bool initialize() {
        handle = CreateJobObjectW(nullptr, nullptr);
        if (!handle) return false;
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(handle, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) return false;
        SIZE_T bytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        attributes.resize(bytes);
        auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
        if (!InitializeProcThreadAttributeList(list, 1, 0, &bytes)) return false;
        startup.lpAttributeList = list;
        return UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST, &handle, sizeof(handle), nullptr, nullptr);
    }
    ~ResearchJob() {
        if (startup.lpAttributeList) DeleteProcThreadAttributeList(startup.lpAttributeList);
        if (handle) CloseHandle(handle);
    }
};
#endif
void terminateTree(QProcess* process) {
#ifdef Q_OS_UNIX
    // uv can have a Python child. Each request owns an isolated process group.
    const auto group = process->property("researchGroup").toLongLong();
    if (group > 0) ::kill(-pid_t(group), SIGKILL);
#endif
#ifdef Q_OS_WIN
    const auto handle = reinterpret_cast<HANDLE>(process->property("researchJob").value<quintptr>());
    if (handle) TerminateJobObject(handle, 1);
#endif
    if (process->state() != QProcess::NotRunning) process->kill();
}
}
ResearchProcess::~ResearchProcess() { cancel(); }
void ResearchProcess::cancel() {
    if (!m_process) return;
    auto* process = m_process.data();
    m_process = nullptr;
    disconnect(process, nullptr, this, nullptr);
    // Process lifetime belongs to the application until it has actually exited;
    // deleting a running QProcess would synchronously wait in its destructor.
    if (process->state() == QProcess::NotRunning) process->deleteLater();
    else {
        connect(process, &QProcess::started, process, [process] { terminateTree(process); });
        terminateTree(process);
    }
}
QString ResearchProcess::scriptsPath() {
#ifdef SENTINEL_SOURCE_DIR
    const QString source = QDir(QString::fromUtf8(SENTINEL_SOURCE_DIR)).filePath("scripts");
    if (QFileInfo::exists(source)) return source;
#endif
    for (const auto& base : {QDir::currentPath(), QCoreApplication::applicationDirPath()}) {
        QDir dir(base);
        for (int i = 0; i < 6; ++i) {
            if (QFileInfo::exists(dir.filePath("scripts/pyproject.toml"))) return dir.filePath("scripts");
            if (!dir.cdUp()) break;
        }
    }
    return QDir::current().filePath("scripts");
}
bool ResearchProcess::isEquityTicker(const QString& ticker) {
    // Equity class suffixes are allowed; market pairs (BTC-USD, ETH/USDT,
    // exchange-qualified products) are never converted to another identifier.
    static const QRegularExpression equity("^[A-Z][A-Z0-9]{0,9}(?:[.-][A-Z])?$");
    return equity.match(ticker.trimmed().toUpper()).hasMatch();
}
void ResearchProcess::run(quint64 request, const QString& script, const QStringList& arguments) {
    cancel();
    auto* process = new QProcess(QCoreApplication::instance());
    m_process = process;
#ifdef Q_OS_UNIX
    connect(process, &QProcess::started, process, [process] {
        process->setProperty("researchGroup", process->processId());
    });
    process->setUnixProcessParameters(QProcess::UnixProcessFlag::CloseFileDescriptors);
    process->setChildProcessModifier([process] {
        if (::setpgid(0, 0) == -1) process->failChildProcessModifier("setpgid", errno);
    });
#endif
#ifdef Q_OS_WIN
    auto job = std::make_shared<ResearchJob>();
    if (!job->initialize()) {
        m_process = nullptr;
        process->deleteLater();
        emit completed(request, {}, "Could not create an isolated research process job");
        return;
    }
    process->setProperty("researchJob", QVariant::fromValue(reinterpret_cast<quintptr>(job->handle)));
    process->setCreateProcessArgumentsModifier([job](QProcess::CreateProcessArguments* args) {
        job->startup.StartupInfo = *args->startupInfo;
        job->startup.StartupInfo.cb = sizeof(STARTUPINFOEXW);
        args->startupInfo = &job->startup.StartupInfo;
        args->flags |= EXTENDED_STARTUPINFO_PRESENT;
    });
#endif
    auto output = std::make_shared<QByteArray>();
    auto errors = std::make_shared<QByteArray>();
    auto* deadline = new QTimer(process);
    deadline->setSingleShot(true);
    deadline->setInterval(90000);
    auto fail = [this, process, request](const QString& message) {
        if (m_process != process) return;
        cancel();
        emit completed(request, {}, message);
    };
    connect(deadline, &QTimer::timeout, this, [fail] { fail("Research request timed out after 90 seconds"); });
    connect(process, &QProcess::readyReadStandardOutput, this, [process, output, fail] {
        *output += process->readAllStandardOutput();
        if (output->size() > 8 * 1024 * 1024) fail("Research response exceeded 8 MiB");
    });
    connect(process, &QProcess::readyReadStandardError, this, [process, errors, fail] {
        *errors += process->readAllStandardError();
        if (errors->size() > 256 * 1024) fail("Research diagnostic output exceeded 256 KiB");
    });
    connect(process, &QProcess::finished, this, [this, process, request, output, errors](int code, QProcess::ExitStatus status) {
        if (m_process != process) return;
        *output += process->readAllStandardOutput();
        *errors += process->readAllStandardError();
        m_process = nullptr;
        if (output->size() > 8 * 1024 * 1024 || errors->size() > 256 * 1024) {
            emit completed(request, {}, "Research response exceeded its output limit");
            return;
        }
        const QString error = status == QProcess::NormalExit && code == 0 ? QString{}
            : QString("Research process failed (%1): %2").arg(code).arg(QString::fromUtf8(*errors).left(500));
        emit completed(request, *output, error);
    });
    connect(process, &QProcess::errorOccurred, this, [fail, process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) fail("Could not start uv/Python: " + process->errorString());
    });
    connect(process, &QProcess::finished, process, [process] {
        terminateTree(process);
        process->deleteLater();
    });
    connect(process, &QProcess::errorOccurred, process, [process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) process->deleteLater();
    });
    process->setWorkingDirectory(scriptsPath());
    process->start("uv", QStringList{"run", "python", script} + arguments);
    deadline->start();
}
