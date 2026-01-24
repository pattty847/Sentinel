/*
Sentinel — LabTextItem
Role: QML item to render MSDF text in the Lab dock.
Threading: Atlas built on GUI thread; render on QSG thread.
*/
#pragma once

#include <QColor>
#include <QQuickItem>
#include <QtQml/qqmlregistration.h>
#include <vector>

#include "MsdfAtlas.hpp"
#include "MsdfGlyphNode.hpp"

class LabTextItem : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(QString text READ text WRITE setText NOTIFY textChanged)
    Q_PROPERTY(float scale READ scale WRITE setScale NOTIFY scaleChanged)
    Q_PROPERTY(QColor color READ color WRITE setColor NOTIFY colorChanged)
    Q_PROPERTY(QString fontFamily READ fontFamily WRITE setFontFamily NOTIFY fontFamilyChanged)
    Q_PROPERTY(int fontPixelSize READ fontPixelSize WRITE setFontPixelSize NOTIFY fontPixelSizeChanged)
    Q_PROPERTY(float pixelRange READ pixelRange WRITE setPixelRange NOTIFY pixelRangeChanged)
    Q_PROPERTY(QString charset READ charset WRITE setCharset NOTIFY charsetChanged)

public:
    explicit LabTextItem(QQuickItem* parent = nullptr);

    QString text() const { return m_text; }
    float scale() const { return m_scale; }
    QColor color() const { return m_color; }
    QString fontFamily() const { return m_fontFamily; }
    int fontPixelSize() const { return m_fontPx; }
    float pixelRange() const { return m_pxRange; }
    QString charset() const { return m_charset; }

    void setText(const QString& text);
    void setScale(float scale);
    void setColor(const QColor& color);
    void setFontFamily(const QString& fontFamily);
    void setFontPixelSize(int fontPx);
    void setPixelRange(float pxRange);
    void setCharset(const QString& charset);

signals:
    void textChanged();
    void scaleChanged();
    void colorChanged();
    void fontFamilyChanged();
    void fontPixelSizeChanged();
    void pixelRangeChanged();
    void charsetChanged();

protected:
    void updatePolish() override;
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) override;
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;

private:
    void markAtlasDirty();
    void markGeometryDirty();
    void rebuildAtlas();
    void rebuildGeometry();

    MsdfAtlas m_atlas;
    std::vector<MsdfGlyphNode::GlyphQuad> m_quads;
    bool m_atlasDirty = true;
    bool m_geometryDirty = true;

    QString m_text = "12.948";
    float m_scale = 1.0f;
    QColor m_color = QColor("#e7f1ff");
    QString m_fontFamily = "Roboto Mono";
    int m_fontPx = 96;
    float m_pxRange = 8.0f;
    QString m_charset = "0123456789.kMB$+-";
};
