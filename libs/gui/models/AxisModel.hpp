#pragma once
#include <QAbstractListModel>
#include <QObject>
#include <QPointF>
#include <QQuickItem>
#include <vector>

class GridViewState;
class UnifiedGridRenderer;

class AxisModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QQuickItem* target READ target WRITE setTarget NOTIFY targetChanged)
    Q_PROPERTY(int labelCount READ labelCount NOTIFY labelCountChanged)
    
public:
    enum Role {
        PositionRole = Qt::UserRole + 1,    // Screen position of tick (double)
        LabelRole,                          // Formatted label text (QString)
        IsMajorTickRole                     // Whether this is a major tick (bool)
    };
    
    explicit AxisModel(QObject* parent = nullptr);
    virtual ~AxisModel() = default;
    
    QQuickItem* target() const { return m_target; }
    void setTarget(QQuickItem* target);
    
signals:
    void targetChanged();
    void labelCountChanged();
    
public:
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;
    int labelCount() const { return m_labelCapacity; }
    
    void setGridViewState(GridViewState* viewState);
    void setViewportSize(double width, double height);
    
    Q_INVOKABLE virtual void recalculateTicks() = 0;

protected:
    struct TickInfo {
        double value;           // Raw value (timestamp_ms or price)
        double position;        // Screen position (pixels)
        QString label;          // Formatted label
        bool isMajorTick;      // Major or minor tick
        
        TickInfo()
            : value(0.0), position(0.0), label(), isMajorTick(false) {}
        TickInfo(double v, double pos, const QString& lbl, bool major = true)
            : value(v), position(pos), label(lbl), isMajorTick(major) {}
    };
    
    virtual void calculateTicks() = 0;
    virtual QString formatLabel(double value) const = 0;
    virtual double calculateNiceStep(double range, int targetTicks) const;
    
    virtual double getViewportStart() const;
    virtual double getViewportEnd() const;
    virtual double getViewportWidth() const;
    virtual double getViewportHeight() const;
    virtual bool isViewportValid() const;
    
    void clearTicks();
    void addTick(double value, double position, const QString& label, bool isMajorTick = true);
    virtual double valueToScreenPosition(double value) const;
    void updateTicksAndNotify();
    
private slots:
    void onViewportChanged();
    void onPanVisualOffsetChanged();

protected:
    GridViewState* m_viewState = nullptr;
    std::vector<TickInfo> m_ticks;
    std::vector<TickInfo> m_ticksScratch;  // swap buffer — avoids heap alloc in hot path
    UnifiedGridRenderer* m_renderer = nullptr;
    UnifiedGridRenderer* renderer() const { return m_renderer; }
    int m_labelCapacity = 32;
    int m_tickWriteIndex = 0;
    
    double m_viewportWidth = 800.0;
    double m_viewportHeight = 600.0;
    
private:
    QQuickItem* m_target = nullptr;
};

Q_DECLARE_METATYPE(AxisModel*)
