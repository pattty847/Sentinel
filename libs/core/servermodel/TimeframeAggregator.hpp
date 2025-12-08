#pragma once
#include <QObject>

class TimeframeAggregator : public QObject {
    Q_OBJECT
public:
    explicit TimeframeAggregator(QObject* parent = nullptr);
};

