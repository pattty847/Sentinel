#include "AxisModel.hpp"
#include "../render/GridViewState.hpp"
#include <QDebug>
#include <cmath>
#include <algorithm>

AxisModel::AxisModel(QObject* parent)
    : QAbstractListModel(parent) {
}

void AxisModel::setGridViewState(GridViewState* viewState) {
    if (m_viewState == viewState) return;
    
    // Disconnect from old view state
    if (m_viewState) {
        disconnect(m_viewState, &GridViewState::viewportChanged, 
                  this, &AxisModel::onViewportChanged);
    }
    
    m_viewState = viewState;
    
    // Connect to new view state
    if (m_viewState) {
        connect(m_viewState, &GridViewState::viewportChanged,
               this, &AxisModel::onViewportChanged);
        
        // Trigger initial calculation
        onViewportChanged();
    }
}

void AxisModel::setViewportSize(double width, double height) {
    if (width > 0 && height > 0) {
        bool changed = (m_viewportWidth != width || m_viewportHeight != height);
        m_viewportWidth = width;
        m_viewportHeight = height;
        
        if (changed && m_viewState) {
            onViewportChanged();
        }
    }
}

int AxisModel::rowCount(const QModelIndex& parent) const {
    Q_UNUSED(parent)
    return static_cast<int>(m_ticks.size());
}

QVariant AxisModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= static_cast<int>(m_ticks.size())) {
        return QVariant();
    }
    
    const TickInfo& tick = m_ticks[index.row()];
    
    switch (role) {
        case PositionRole:
            return tick.position;
        case LabelRole:
            return tick.label;
        case IsMajorTickRole:
            return tick.isMajorTick;
        default:
            return QVariant();
    }
}

QHash<int, QByteArray> AxisModel::roleNames() const {
    QHash<int, QByteArray> roles;
    roles[PositionRole] = "position";
    roles[LabelRole] = "label";
    roles[IsMajorTickRole] = "isMajorTick";
    return roles;
}

void AxisModel::onViewportChanged() {
    if (!m_viewState || !isViewportValid()) return;
    
    // Recalculate ticks
    beginResetModel();
    calculateTicks();
    endResetModel();
}

double AxisModel::calculateNiceStep(double range, int targetTicks) const {
    if (range <= 0 || targetTicks <= 0) return 1.0;
    
    double rawStep = range / targetTicks;
    double magnitude = std::pow(10.0, std::floor(std::log10(rawStep)));
    double normalizedStep = rawStep / magnitude;
    
    // Choose nice step sizes
    double niceStep;
    if (normalizedStep <= 1.0) {
        niceStep = 1.0;
    } else if (normalizedStep <= 2.0) {
        niceStep = 2.0;
    } else if (normalizedStep <= 5.0) {
        niceStep = 5.0;
    } else {
        niceStep = 10.0;
    }
    
    return niceStep * magnitude;
}

double AxisModel::getViewportStart() const {
    // To be implemented by subclasses based on their axis type
    return 0.0;
}

double AxisModel::getViewportEnd() const {
    // To be implemented by subclasses based on their axis type
    return 1.0;
}

double AxisModel::getViewportWidth() const {
    return m_viewportWidth;
}

double AxisModel::getViewportHeight() const {
    return m_viewportHeight;
}

bool AxisModel::isViewportValid() const {
    return m_viewState && m_viewState->isTimeWindowValid() && 
           m_viewportWidth > 0 && m_viewportHeight > 0;
}

void AxisModel::clearTicks() {
    m_ticks.clear();
}

void AxisModel::addTick(double value, double position, const QString& label, bool isMajorTick) {
    m_ticks.emplace_back(value, position, label, isMajorTick);
}

double AxisModel::valueToScreenPosition(double value) const {
    // Default implementation - to be overridden by subclasses
    double start = getViewportStart();
    double end = getViewportEnd();
    
    if (end <= start) return 0.0;
    
    double normalized = (value - start) / (end - start);
    return normalized * getViewportWidth();
}