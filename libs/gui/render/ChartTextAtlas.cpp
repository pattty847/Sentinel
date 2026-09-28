#include "ChartTextAtlas.hpp"
#include "SentinelLogging.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

bool ChartTextAtlas::build(const BuildParams& params) {
    MsdfAtlas::BuildParams atlasParams;
    atlasParams.fontFamily = params.fontFamily;
    atlasParams.fontPath = ensureFontFile(params);
    atlasParams.charset = params.charset;
    atlasParams.fontPx = params.fontPx;
    atlasParams.pxRange = params.pxRange;
    return m_atlas.build(atlasParams);
}

QString ChartTextAtlas::ensureFontFile(const BuildParams& params) const {
    if (!params.fontPath.isEmpty()) {
        QFileInfo fi(params.fontPath);
        if (fi.exists() && fi.isReadable()) {
            return fi.absoluteFilePath();
        }
        // Portable bundles often omit repo-relative resources/fonts — fall back to embedded Qt resource font.
        sLog_Warning("Chart text atlas: MSDF font path not found, using embedded font: path="
                     << params.fontPath << " resource="
                     << (params.resourceFont.isEmpty() ? QStringLiteral("(none)") : params.resourceFont));
    }
    if (params.resourceFont.isEmpty()) {
        return {};
    }

    QFile src(params.resourceFont);
    if (!src.open(QIODevice::ReadOnly)) {
        sLog_Warning("Chart text atlas: embedded font open failed resource=" << params.resourceFont);
        return {};
    }
    const QByteArray bytes = src.readAll();
    const QByteArray hash = QCryptographicHash::hash(bytes, QCryptographicHash::Sha1).toHex();
    const QString fileName = QStringLiteral("%1_%2")
        .arg(QString::fromLatin1(hash.left(12)), QFileInfo(params.resourceFont).fileName());
    const QString dir = runtimeDir();
    QDir().mkpath(dir);
    const QString outPath = QDir(dir).filePath(fileName);

    QFileInfo outInfo(outPath);
    if (outInfo.exists() && outInfo.size() == bytes.size()) {
        return outPath;
    }

    QFile out(outPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        sLog_Warning("Chart text atlas: font extract open failed path=" << outPath << " error=" << out.errorString());
        return {};
    }
    if (out.write(bytes) != bytes.size()) {
        sLog_Warning("Chart text atlas: font extract write failed path=" << outPath << " error=" << out.errorString());
        out.remove();
        return {};
    }
    return outPath;
}

QString ChartTextAtlas::runtimeDir() const {
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (base.isEmpty()) {
        base = QDir::current().filePath(QStringLiteral("data"));
    }
    return QDir(base).filePath(QStringLiteral("fonts/msdf"));
}
