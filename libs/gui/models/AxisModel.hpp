#pragma once
#include <QAbstractListModel>
#include <QObject>
#include <QPointF>
#include <vector>

class GridViewState;

/**
 * AxisModel - Base class for axis tick calculation and presentation
 * 
 * This abstract base class provides the foundation for calculating "nice" tick marks
 * for price and time axes. It inherits from QAbstractListModel to expose tick data
 * to QML components efficiently.
 * 
 * The model automatically updates when the viewport changes by connecting to
 * GridViewState::viewportChanged() signal.
 */
class AxisModel : public QAbstractListModel {
    Q_OBJECT
    
public:
    enum Role {
        PositionRole = Qt::UserRole + 1,    // Screen position of tick (double)
        LabelRole,                          // Formatted label text (QString)
        IsMajorTickRole                     // Whether this is a major tick (bool)
    };
    
    explicit AxisModel(QObject* parent = nullptr);
    virtual ~AxisModel() = default;
    
    // QAbstractListModel interface
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;
    
    // Configuration
    void setGridViewState(GridViewState* viewState);
    void setViewportSize(double width, double height);
    
    // Public interface for subclasses
    Q_INVOKABLE virtual void recalculateTicks() = 0;

protected:
    struct TickInfo {
        double value;           // Raw value (timestamp_ms or price)
        double position;        // Screen position (pixels)
        QString label;          // Formatted label
        bool isMajorTick;      // Major or minor tick
        
        TickInfo(double v, double pos, const QString& lbl, bool major = true)
            : value(v), position(pos), label(lbl), isMajorTick(major) {}
    };
    
    // Protected interface for subclasses
    virtual void calculateTicks() = 0;
    virtual QString formatLabel(double value) const = 0;
    virtual double calculateNiceStep(double range, int targetTicks) const;
    
    // Viewport access for subclasses
    double getViewportStart() const;
    double getViewportEnd() const;
    double getViewportWidth() const;
    double getViewportHeight() const;
    bool isViewportValid() const;
    
    // Utility functions
    void clearTicks();
    void addTick(double value, double position, const QString& label, bool isMajorTick = true);
    double valueToScreenPosition(double value) const;
    
private slots:
    void onViewportChanged();

protected:
    GridViewState* m_viewState = nullptr;
    std::vector<TickInfo> m_ticks;
    
    // Viewport dimensions
    double m_viewportWidth = 800.0;
    double m_viewportHeight = 600.0;
};

Q_DECLARE_METATYPE(AxisModel*)