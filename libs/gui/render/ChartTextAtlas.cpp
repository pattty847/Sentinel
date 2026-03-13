#include "ChartTextAtlas.hpp"

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
        return params.fontPath;
    }
    if (params.resourceFont.isEmpty()) {
        return {};
    }

    QFile src(params.resourceFont);
    if (!src.open(QIODevice::ReadOnly)) {
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
        return {};
    }
    if (out.write(bytes) != bytes.size()) {
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
