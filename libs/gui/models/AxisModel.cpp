#include "AxisModel.hpp"
#include "../render/GridViewState.hpp"
#include "../UnifiedGridRenderer.h"
#include <QDebug>
#include <cmath>
#include <algorithm>
AxisModel::AxisModel(QObject* parent)
    : QAbstractListModel(parent) {
    m_ticks.assign(static_cast<size_t>(m_labelCapacity), TickInfo());
}

void AxisModel::setTarget(QQuickItem* target) {
    if (m_target == target) return;
    
    m_target = target;
    emit targetChanged();
    m_renderer = qobject_cast<UnifiedGridRenderer*>(target);
    if (!m_renderer) {
        setGridViewState(nullptr);
        return;
    }

    GridViewState* viewState = m_renderer->getViewState();
    if (viewState) {
        setGridViewState(viewState);
        connect(m_renderer, &QQuickItem::widthChanged, this, [this]() {
            setViewportSize(m_renderer->width(), m_renderer->height());
        });
        connect(m_renderer, &QQuickItem::heightChanged, this, [this]() {
            setViewportSize(m_renderer->width(), m_renderer->height());
        });
        setViewportSize(m_renderer->width(), m_renderer->height());
    }
}

void AxisModel::setGridViewState(GridViewState* viewState) {
    if (m_viewState == viewState) return;
    
    // Disconnect from old view state
    if (m_viewState) {
        disconnect(m_viewState, &GridViewState::viewportChanged, 
                  this, &AxisModel::onViewportChanged);
        disconnect(m_viewState, &GridViewState::panVisualOffsetChanged,
                  this, &AxisModel::onPanVisualOffsetChanged);
    }
    
    m_viewState = viewState;

    if (m_viewState) {
        connect(m_viewState, &GridViewState::viewportChanged,
               this, &AxisModel::onViewportChanged);
        connect(m_viewState, &GridViewState::panVisualOffsetChanged,
               this, &AxisModel::onPanVisualOffsetChanged);
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
    return m_labelCapacity;
}

QVariant AxisModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_labelCapacity ||
        index.row() >= static_cast<int>(m_ticks.size())) {
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
    updateTicksAndNotify();
}

void AxisModel::onPanVisualOffsetChanged() {
    Q_UNUSED(m_viewState);
}

double AxisModel::calculateNiceStep(double range, int targetTicks) const {
    if (range <= 0 || targetTicks <= 0) return 1.0;
    
    double rawStep = range / targetTicks;
    double magnitude = std::pow(10.0, std::floor(std::log10(rawStep)));
    double normalizedStep = rawStep / magnitude;
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
    if (static_cast<int>(m_ticks.size()) != m_labelCapacity) {
        m_ticks.assign(static_cast<size_t>(m_labelCapacity), TickInfo());
        emit labelCountChanged();
    } else {
        for (auto& tick : m_ticks) {
            tick = TickInfo();
        }
    }
    m_tickWriteIndex = 0;
}

void AxisModel::addTick(double value, double position, const QString& label, bool isMajorTick) {
    if (m_tickWriteIndex >= m_labelCapacity) {
        return;
    }
    m_ticks[static_cast<size_t>(m_tickWriteIndex++)] = TickInfo(value, position, label, isMajorTick);
}

void AxisModel::updateTicksAndNotify() {
    if (!m_viewState || !isViewportValid()) return;

    const std::vector<TickInfo> previousTicks = m_ticks;
    calculateTicks();

    if (m_labelCapacity <= 0) {
        return;
    }

    bool positionChanged = false;
    bool labelChanged = false;
    bool majorChanged = false;
    const double kPosEps = 0.01;

    const size_t count = std::min(previousTicks.size(), m_ticks.size());
    for (size_t i = 0; i < count; ++i) {
        const TickInfo& before = previousTicks[i];
        const TickInfo& after = m_ticks[i];
        if (std::abs(before.position - after.position) > kPosEps) {
            positionChanged = true;
        }
        if (before.label != after.label) {
            labelChanged = true;
        }
        if (before.isMajorTick != after.isMajorTick) {
            majorChanged = true;
        }
    }

    if (!positionChanged && !labelChanged && !majorChanged) {
        return;
    }

    QVector<int> roles;
    if (positionChanged) {
        roles << PositionRole;
    }
    if (labelChanged) {
        roles << LabelRole;
    }
    if (majorChanged) {
        roles << IsMajorTickRole;
    }

    emit dataChanged(index(0, 0), index(m_labelCapacity - 1, 0), roles);
}

double AxisModel::valueToScreenPosition(double value) const {
    double start = getViewportStart();
    double end = getViewportEnd();
    
    if (end <= start) return 0.0;
    
    double normalized = (value - start) / (end - start);
    return normalized * getViewportWidth();
}
