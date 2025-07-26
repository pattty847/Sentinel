#ifndef STATISTICSCONTROLLER_H
#define STATISTICSCONTROLLER_H

#include <QObject>
#include "StatisticsProcessor.h"
#include "TradeData.h"

class StatisticsController : public QObject
{
    Q_OBJECT

public:
    explicit StatisticsController(QObject *parent = nullptr);
    StatisticsProcessor* processor();

public slots:
    void processTrade(const Trade &trade);

signals:
    void cvdUpdated(double newCvd);

private:
    StatisticsProcessor m_processor;
};

#endif // STATISTICSCONTROLLER_H 