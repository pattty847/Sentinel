#include "SecBackendServer.hpp"
#include "SentinelLogging.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QProcess>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <QHostAddress>

namespace {

struct SecBackendConfig {
    QString pythonExe;
    QString scriptsDir;
    QString repoRoot;
    int timeoutMs = 30000;
    int maxOutputBytes = 5 * 1024 * 1024;
};

struct HttpRequest {
    QString method;
    QString path;
    QMap<QString, QString> headers;
    QByteArray body;
};

QString findScriptsDir() {
    const QString envOverride = qEnvironmentVariable("SENTINEL_SEC_SCRIPTS_DIR");
    if (!envOverride.isEmpty()) {
        QDir dir(envOverride);
        if (dir.exists("sec")) {
            return dir.absolutePath();
        }
        sLog_Warning("SENTINEL_SEC_SCRIPTS_DIR set but missing sec/ folder: " << envOverride);
    }

    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        QDir(appDir).filePath("scripts"),
        QDir(appDir).filePath("../scripts"),
        QDir(appDir).filePath("../../scripts")
    };

    for (const auto& path : candidates) {
        QDir dir(path);
        if (dir.exists("sec")) {
            return dir.absolutePath();
        }
    }

    return QString();
}

QString findRepoRoot(const QString& scriptsDir) {
    if (scriptsDir.isEmpty()) {
        return QString();
    }
    QDir dir(scriptsDir);
    dir.cdUp();
    return dir.absolutePath();
}

QString findPythonExecutable(const QString& repoRoot) {
    const QString envOverride = qEnvironmentVariable("SENTINEL_SEC_PYTHON");
    if (!envOverride.isEmpty()) {
        return envOverride;
    }

    if (!repoRoot.isEmpty()) {
#ifdef Q_OS_WIN
        QString venvPython = QDir(repoRoot).filePath(".venv/Scripts/python.exe");
#else
        QString venvPython = QDir(repoRoot).filePath(".venv/bin/python");
#endif
        if (QFileInfo::exists(venvPython)) {
            return venvPython;
        }
    }

#ifdef Q_OS_WIN
    return "python";
#else
    return "python3";
#endif
}

SecBackendConfig buildConfig() {
    SecBackendConfig cfg;
    cfg.scriptsDir = findScriptsDir();
    cfg.repoRoot = findRepoRoot(cfg.scriptsDir);
    cfg.pythonExe = findPythonExecutable(cfg.repoRoot);

    const QByteArray timeoutEnv = qgetenv("SENTINEL_SEC_TIMEOUT_MS");
    bool ok = false;
    const int timeoutMs = timeoutEnv.toInt(&ok);
    if (ok && timeoutMs > 0) {
        cfg.timeoutMs = timeoutMs;
    }

    const QByteArray maxEnv = qgetenv("SENTINEL_SEC_MAX_OUTPUT_BYTES");
    const int maxOut = maxEnv.toInt(&ok);
    if (ok && maxOut > 0) {
        cfg.maxOutputBytes = maxOut;
    }

    return cfg;
}

bool parseHttpRequest(const QByteArray& buffer, HttpRequest& request, int& bodyStart, int& contentLength) {
    const int headerEnd = buffer.indexOf("\r\n\r\n");
    if (headerEnd < 0) {
        return false;
    }

    const QByteArray headerBlock = buffer.left(headerEnd);
    const QList<QByteArray> lines = headerBlock.split('\n');
    if (lines.isEmpty()) {
        return false;
    }

    const QList<QByteArray> firstParts = lines.first().trimmed().split(' ');
    if (firstParts.size() < 2) {
        return false;
    }

    request.method = QString::fromUtf8(firstParts[0]);
    request.path = QString::fromUtf8(firstParts[1]);

    for (int i = 1; i < lines.size(); ++i) {
        const QByteArray line = lines[i].trimmed();
        const int colon = line.indexOf(':');
        if (colon <= 0) {
            continue;
        }
        const QString key = QString::fromUtf8(line.left(colon)).toLower();
        const QString value = QString::fromUtf8(line.mid(colon + 1)).trimmed();
        request.headers.insert(key, value);
    }

    bodyStart = headerEnd + 4;
    contentLength = request.headers.value("content-length").toInt();
    return true;
}

class SecRequestHandler : public QObject {
public:
    SecRequestHandler(QTcpSocket* socket, SecBackendConfig config, QObject* parent = nullptr)
        : QObject(parent)
        , m_socket(socket)
        , m_config(std::move(config))
    {
        m_socket->setParent(this);
        connect(m_socket, &QTcpSocket::readyRead, this, &SecRequestHandler::onReadyRead);
        connect(m_socket, &QTcpSocket::disconnected, this, &SecRequestHandler::deleteLater);
    }

private:
    void onReadyRead() {
        if (m_responded) {
            return;
        }

        m_buffer.append(m_socket->readAll());
        if (!m_headersParsed) {
            int bodyStart = 0;
            int contentLength = 0;
            if (!parseHttpRequest(m_buffer, m_request, bodyStart, contentLength)) {
                return;
            }

            m_headersParsed = true;
            m_bodyStart = bodyStart;
            m_contentLength = contentLength;
        }

        if (m_buffer.size() < m_bodyStart + m_contentLength) {
            return;
        }

        if (m_contentLength > 0) {
            m_request.body = m_buffer.mid(m_bodyStart, m_contentLength);
        }

        handleRequest();
    }

    void onProcessFinished(int exitCode, QProcess::ExitStatus status) {
        if (m_timeout) {
            m_timeout->stop();
        }

        if (!m_process) {
            failRequest(500, "SEC process missing");
            return;
        }

        const QByteArray stdoutBytes = m_process->readAllStandardOutput();
        const QByteArray stderrBytes = m_process->readAllStandardError();

        if (stdoutBytes.size() > m_config.maxOutputBytes) {
            failRequest(502, "SEC response too large");
            return;
        }

        if (status != QProcess::NormalExit || exitCode != 0) {
            const QString err = QString::fromUtf8(stderrBytes).trimmed();
            failRequest(502, "SEC script failed", err.isEmpty() ? QString::fromUtf8(stdoutBytes) : err);
            return;
        }

        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(stdoutBytes, &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            failRequest(502, "Invalid SEC JSON response", parseError.errorString());
            return;
        }

        respondJson(200, doc.object());
    }

    void onProcessError(QProcess::ProcessError error) {
        Q_UNUSED(error);
        if (m_timeout) {
            m_timeout->stop();
        }
        failRequest(502, "SEC process error", m_process ? m_process->errorString() : QString());
    }

    void onTimeout() {
        if (!m_process) {
            return;
        }
        m_process->kill();
        failRequest(504, "SEC request timed out");
    }

private:
    void handleRequest() {
        const QUrl url = QUrl::fromEncoded(m_request.path.toUtf8());
        const QString path = url.path();

        if (!path.startsWith("/sec")) {
            failRequest(404, "Not Found");
            return;
        }

        if (path == "/sec/ping" && m_request.method == "GET") {
            QJsonObject payload;
            payload["ok"] = true;
            payload["service"] = "sec";
            payload["ts"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
            respondJson(200, payload);
            return;
        }

        if (m_request.method != "POST") {
            failRequest(405, "Method Not Allowed");
            return;
        }

        QJsonParseError parseError;
        const QJsonDocument bodyDoc = QJsonDocument::fromJson(m_request.body, &parseError);
        if (parseError.error != QJsonParseError::NoError || !bodyDoc.isObject()) {
            failRequest(400, "Invalid JSON body", parseError.errorString());
            return;
        }

        const QJsonObject payload = bodyDoc.object();
        if (!payload.contains("ticker") || payload.value("ticker").toString().trimmed().isEmpty()) {
            failRequest(400, "Missing ticker");
            return;
        }

        if (m_config.scriptsDir.isEmpty()) {
            failRequest(500, "SEC scripts directory not found");
            return;
        }

        QString script;
        if (path == "/sec/filings") {
            script = "sec_fetch_filings.py";
        } else if (path == "/sec/insider" || path == "/sec/transactions") {
            script = "sec_fetch_transactions.py";
        } else if (path == "/sec/financials" || path == "/sec/financial-summary") {
            script = "sec_fetch_financials.py";
        } else {
            failRequest(404, "Unknown SEC endpoint");
            return;
        }

        startProcess(script, payload);
    }

    void startProcess(const QString& scriptName, const QJsonObject& payload) {
        const QString scriptPath = QDir(m_config.scriptsDir).filePath(QString("sec/%1").arg(scriptName));
        if (!QFileInfo::exists(scriptPath)) {
            failRequest(500, "SEC script not found", scriptPath);
            return;
        }

        m_process = new QProcess(this);
        if (!m_config.repoRoot.isEmpty()) {
            m_process->setWorkingDirectory(m_config.repoRoot);
        }

        connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, &SecRequestHandler::onProcessFinished);
        connect(m_process, &QProcess::errorOccurred, this, &SecRequestHandler::onProcessError);

        QStringList args;
        args << scriptPath << "--input-json";

        m_process->start(m_config.pythonExe, args);
        if (!m_process->waitForStarted(2000)) {
            failRequest(502, "Failed to start SEC process", m_process->errorString());
            return;
        }

        const QByteArray payloadBytes = QJsonDocument(payload).toJson(QJsonDocument::Compact);
        m_process->write(payloadBytes);
        m_process->closeWriteChannel();

        m_timeout = new QTimer(this);
        m_timeout->setSingleShot(true);
        connect(m_timeout, &QTimer::timeout, this, &SecRequestHandler::onTimeout);
        m_timeout->start(m_config.timeoutMs);
    }

    void respondJson(int statusCode, const QJsonObject& obj) {
        if (m_responded) {
            return;
        }
        m_responded = true;

        const QByteArray body = QJsonDocument(obj).toJson(QJsonDocument::Compact);
        QByteArray response;
        response += "HTTP/1.1 " + QByteArray::number(statusCode) + " ";
        response += (statusCode == 200 ? "OK" : "ERROR");
        response += "\r\n";
        response += "Content-Type: application/json\r\n";
        response += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
        response += "Connection: close\r\n\r\n";
        response += body;

        m_socket->write(response);
        m_socket->flush();
        m_socket->disconnectFromHost();
    }

    void failRequest(int statusCode, const QString& message, const QString& details = QString()) {
        QJsonObject obj;
        obj["ok"] = false;
        obj["error"] = message;
        if (!details.isEmpty()) {
            obj["details"] = details;
        }
        respondJson(statusCode, obj);
    }

    QTcpSocket* m_socket = nullptr;
    SecBackendConfig m_config;
    QByteArray m_buffer;
    HttpRequest m_request;
    bool m_headersParsed = false;
    int m_bodyStart = 0;
    int m_contentLength = 0;
    bool m_responded = false;

    QProcess* m_process = nullptr;
    QTimer* m_timeout = nullptr;
};

} // namespace

SecBackendServer::SecBackendServer(QObject* parent)
    : QObject(parent)
    , m_server(new QTcpServer(this))
{
    connect(m_server, &QTcpServer::newConnection, this, [this]() {
        SecBackendConfig config = buildConfig();
        while (m_server->hasPendingConnections()) {
            QTcpSocket* socket = m_server->nextPendingConnection();
            new SecRequestHandler(socket, config, this);
        }
    });
}

SecBackendServer::~SecBackendServer() {
    stop();
}

bool SecBackendServer::start(quint16 port) {
    if (m_server->isListening()) {
        return true;
    }

    SecBackendConfig config = buildConfig();
    if (config.scriptsDir.isEmpty()) {
        sLog_Error("SEC backend could not find scripts directory. Set SENTINEL_SEC_SCRIPTS_DIR or deploy scripts/.");
        return false;
    }

    if (!m_server->listen(QHostAddress::LocalHost, port)) {
        sLog_Error("SEC backend failed to bind on 127.0.0.1:" << port);
        return false;
    }

    sLog_App("SEC backend listening on 127.0.0.1:" << port);
    return true;
}

void SecBackendServer::stop() {
    if (m_server->isListening()) {
        m_server->close();
    }
}

bool SecBackendServer::isRunning() const {
    return m_server->isListening();
}
