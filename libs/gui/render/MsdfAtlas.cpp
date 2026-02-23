/*
Sentinel — MsdfAtlas
*/
#include "MsdfAtlas.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QVector>
#include <QtGlobal>
#include <algorithm>
#include <cmath>

#include <msdfgen/msdfgen.h>
#include <msdfgen/ext/import-font.h>

namespace {
constexpr const char* kCacheVersion = "msdf_atlas_v1";

float clamp01(float value) {
    if (value < 0.0f) {
        return 0.0f;
    }
    if (value > 1.0f) {
        return 1.0f;
    }
    return value;
}

QStringList candidateFontFileNames(const QString& family) {
    const QString normalized = family.toLower().remove(' ');
    if (normalized.contains("robotomono")) {
        return {
            "RobotoMono-Regular.ttf",
            "RobotoMono-Regular.otf",
            "RobotoMono-Medium.ttf",
            "RobotoMono-Light.ttf"
        };
    }
    return {};
}

QString findFontFileInDir(const QString& dirPath, const QStringList& names) {
    for (const QString& name : names) {
        const QString candidate = QDir(dirPath).filePath(name);
        if (QFile::exists(candidate)) {
            return candidate;
        }
    }
    return {};
}
} // namespace

bool MsdfAtlas::build(const BuildParams& params) {
    m_glyphs.clear();
    m_image = QImage();
    m_fontPx = params.fontPx;
    m_pxRange = params.pxRange;
    m_fontFamily = params.fontFamily;
    m_paddingPx = static_cast<int>(std::ceil(m_pxRange)) + 2;

    if (params.charset.isEmpty() || params.fontPx <= 0) {
        return false;
    }

    const QString fontPath = resolveFontPath(params);
    if (fontPath.isEmpty()) {
        qWarning("MsdfAtlas: font path not found for family '%s'", qPrintable(params.fontFamily));
        return false;
    }

    const QString key = cacheKey(fontPath, params.fontPx, params.charset, params.pxRange);
    const QString cacheBase = QDir(cacheDirPath()).filePath(key);
    if (loadFromCache(cacheBase, key)) {
        return true;
    }

    msdfgen::FreetypeHandle* ft = msdfgen::initializeFreetype();
    if (!ft) {
        return false;
    }
    msdfgen::FontHandle* font = msdfgen::loadFont(ft, fontPath.toUtf8().constData());
    if (!font) {
        msdfgen::deinitializeFreetype(ft);
        return false;
    }

    msdfgen::FontMetrics metrics;
    if (!msdfgen::getFontMetrics(metrics, font, msdfgen::FONT_SCALING_EM_NORMALIZED)) {
        msdfgen::destroyFont(font);
        msdfgen::deinitializeFreetype(ft);
        return false;
    }

    const double scale = static_cast<double>(params.fontPx) / metrics.emSize;
    double maxAdvance = 0.0;
    double maxBoundsW = 0.0;
    double maxBoundsH = 0.0;

    struct GlyphPrep {
        QChar c;
        int index = 0;
        msdfgen::Shape shape;
        msdfgen::Shape::Bounds bounds;
        double advance = 0.0;
    };
    QVector<GlyphPrep> prepared;
    prepared.reserve(params.charset.size());

    for (int idx = 0; idx < params.charset.size(); ++idx) {
        const QChar c = params.charset.at(idx);
        msdfgen::Shape shape;
        double advance = 0.0;
        if (!msdfgen::loadGlyph(shape, font, static_cast<msdfgen::unicode_t>(c.unicode()),
                                msdfgen::FONT_SCALING_EM_NORMALIZED, &advance)) {
            continue;
        }
        shape.setYAxisOrientation(msdfgen::Y_DOWNWARD);
        shape.normalize();
        msdfgen::edgeColoringSimple(shape, 3.0);
        const auto bounds = shape.getBounds();

        const double width = (bounds.r - bounds.l) * scale;
        const double height = (bounds.t - bounds.b) * scale;
        maxAdvance = std::max(maxAdvance, advance * scale);
        maxBoundsW = std::max(maxBoundsW, width);
        maxBoundsH = std::max(maxBoundsH, height);

        prepared.push_back({c, idx, std::move(shape), bounds, advance});
    }

    const int cellW = static_cast<int>(std::ceil(std::max(maxAdvance, maxBoundsW))) + m_paddingPx * 2;
    const int cellH = static_cast<int>(std::ceil(std::max(maxBoundsH, metrics.lineHeight * scale))) + m_paddingPx * 2;
    if (cellW <= 0 || cellH <= 0) {
        msdfgen::destroyFont(font);
        msdfgen::deinitializeFreetype(ft);
        return false;
    }

    const int atlasW = cellW * params.charset.size();
    const int atlasH = cellH;
    QImage atlas(atlasW, atlasH, QImage::Format_RGB888);
    atlas.fill(Qt::black);

    msdfgen::MSDFGeneratorConfig config;
    for (int i = 0; i < prepared.size(); ++i) {
        const GlyphPrep& prep = prepared[i];
        const int cellX = prep.index * cellW;

        msdfgen::Bitmap<float, 3> glyphBitmap(cellW, cellH, msdfgen::Y_DOWNWARD);
        const double translateX = static_cast<double>(m_paddingPx) - prep.bounds.l * scale;
        const double translateY = static_cast<double>(m_paddingPx) - prep.bounds.b * scale;
        const msdfgen::Vector2 scaleVec(scale, scale);
        const msdfgen::Vector2 translateVec(translateX, translateY);
        const msdfgen::Range range(m_pxRange);
        msdfgen::generateMSDF(glyphBitmap, prep.shape, range, scaleVec, translateVec, config.errorCorrection, true);

        for (int y = 0; y < cellH; ++y) {
            uchar* dst = atlas.scanLine(y) + cellX * 3;
            for (int x = 0; x < cellW; ++x) {
                const float* src = glyphBitmap(x, y);
                const int r = static_cast<int>(clamp01(src[0]) * 255.0f);
                const int g = static_cast<int>(clamp01(src[1]) * 255.0f);
                const int b = static_cast<int>(clamp01(src[2]) * 255.0f);
                dst[0] = static_cast<uchar>(r);
                dst[1] = static_cast<uchar>(g);
                dst[2] = static_cast<uchar>(b);
                dst += 3;
            }
        }

        Glyph glyph;
        glyph.bounds = QRectF(prep.bounds.l * scale,
                              prep.bounds.b * scale,
                              (prep.bounds.r - prep.bounds.l) * scale,
                              (prep.bounds.t - prep.bounds.b) * scale);
        glyph.advance = static_cast<float>(prep.advance * scale);
        const QRectF pixelRect(cellX, 0, cellW, cellH);
        glyph.uv = QRectF(pixelRect.x() / atlasW,
                          pixelRect.y() / atlasH,
                          pixelRect.width() / atlasW,
                          pixelRect.height() / atlasH);
        m_glyphs.insert(prep.c, glyph);
    }

    msdfgen::destroyFont(font);
    msdfgen::deinitializeFreetype(ft);

    m_image = atlas;
    saveCache(cacheBase, key);
    return true;
}

const MsdfAtlas::Glyph& MsdfAtlas::glyph(QChar c) const {
    static const Glyph empty;
    auto it = m_glyphs.constFind(c);
    if (it == m_glyphs.constEnd()) {
        return empty;
    }
    return it.value();
}

QString MsdfAtlas::resolveFontPath(const BuildParams& params) const {
    if (!params.fontPath.isEmpty()) {
        return params.fontPath;
    }
    const QStringList candidates = candidateFontFileNames(params.fontFamily);
    if (candidates.isEmpty()) {
        return {};
    }

    const QStringList fontDirs = QStandardPaths::standardLocations(QStandardPaths::FontsLocation);
    for (const QString& dirPath : fontDirs) {
        const QString direct = findFontFileInDir(dirPath, candidates);
        if (!direct.isEmpty()) {
            return direct;
        }
        QDirIterator it(dirPath, candidates, QDir::Files, QDirIterator::Subdirectories);
        if (it.hasNext()) {
            return it.next();
        }
    }
    return {};
}

QString MsdfAtlas::cacheDirPath() const {
    const QString base = QDir::current().filePath("data/glyph-cache/msdf");
    QDir().mkpath(base);
    return base;
}

QString MsdfAtlas::cacheKey(const QString& fontPath, int fontPx, const QString& charset, float pxRange) const {
    const QString payload = QString("%1|%2|%3|%4|%5")
        .arg(fontPath, QString::number(fontPx), charset, QString::number(pxRange, 'f', 3), kCacheVersion);
    const QByteArray hash = QCryptographicHash::hash(payload.toUtf8(), QCryptographicHash::Sha1);
    return QString::fromLatin1(hash.toHex());
}

bool MsdfAtlas::loadFromCache(const QString& cacheBasePath, const QString& key) {
    const QString metaPath = cacheBasePath + ".json";
    const QString imagePath = cacheBasePath + ".png";
    if (!QFile::exists(metaPath) || !QFile::exists(imagePath)) {
        return false;
    }

    QFile metaFile(metaPath);
    if (!metaFile.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(metaFile.readAll());
    if (!doc.isObject()) {
        return false;
    }
    const QJsonObject root = doc.object();
    if (root.value("key").toString() != key) {
        return false;
    }

    QImage image(imagePath);
    if (image.isNull()) {
        return false;
    }

    m_fontFamily = root.value("fontFamily").toString();
    m_fontPx = root.value("fontPx").toInt();
    m_pxRange = static_cast<float>(root.value("pxRange").toDouble());
    m_paddingPx = root.value("paddingPx").toInt();

    const QJsonArray glyphs = root.value("glyphs").toArray();
    for (const QJsonValue& value : glyphs) {
        const QJsonObject obj = value.toObject();
        const QChar c(static_cast<ushort>(obj.value("code").toInt()));
        Glyph glyph;
        const QJsonArray bounds = obj.value("bounds").toArray();
        const QJsonArray uv = obj.value("uv").toArray();
        if (bounds.size() == 4 && uv.size() == 4) {
            glyph.bounds = QRectF(bounds[0].toDouble(), bounds[1].toDouble(),
                                  bounds[2].toDouble(), bounds[3].toDouble());
            glyph.uv = QRectF(uv[0].toDouble(), uv[1].toDouble(),
                              uv[2].toDouble(), uv[3].toDouble());
        }
        glyph.advance = static_cast<float>(obj.value("advance").toDouble());
        m_glyphs.insert(c, glyph);
    }

    m_image = image;
    return true;
}

void MsdfAtlas::saveCache(const QString& cacheBasePath, const QString& key) const {
    if (m_image.isNull()) {
        return;
    }
    m_image.save(cacheBasePath + ".png");

    QJsonObject root;
    root["key"] = key;
    root["fontFamily"] = m_fontFamily;
    root["fontPx"] = m_fontPx;
    root["pxRange"] = m_pxRange;
    root["paddingPx"] = m_paddingPx;

    QJsonArray glyphs;
    for (auto it = m_glyphs.constBegin(); it != m_glyphs.constEnd(); ++it) {
        QJsonObject obj;
        obj["code"] = static_cast<int>(it.key().unicode());
        obj["advance"] = it.value().advance;
        obj["bounds"] = QJsonArray{it.value().bounds.x(), it.value().bounds.y(),
                                   it.value().bounds.width(), it.value().bounds.height()};
        obj["uv"] = QJsonArray{it.value().uv.x(), it.value().uv.y(),
                               it.value().uv.width(), it.value().uv.height()};
        glyphs.append(obj);
    }
    root["glyphs"] = glyphs;

    const QJsonDocument doc(root);
    QFile metaFile(cacheBasePath + ".json");
    if (metaFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        metaFile.write(doc.toJson(QJsonDocument::Compact));
    }
}
