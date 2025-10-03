/*
Sentinel — StatisticsController
Role: A controller that processes trades to calculate and emit market statistics like CVD.
Inputs/Outputs: Takes Trade data via a slot; emits a cvdUpdated signal with the new CVD value.
Threading: Lives on the main GUI thread; the processTrade slot receives data from other threads.
Performance: Emits a signal for every trade, which may be very high-frequency.
Integration: Delegates calculations to an internal StatisticsProcessor instance.
Observability: No internal logging.
Related: StatisticsController.cpp, StatisticsProcessor.h, TradeData.h.
Assumptions: The consumer of the cvdUpdated signal can handle high-frequency updates.
*/
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