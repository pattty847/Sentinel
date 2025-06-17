#include "statisticscontroller.h"

StatisticsController::StatisticsController(QObject *parent) : QObject(parent)
{
    // Register a callback to the processor to update the CVD
    // This is a lambda function that will be called when the CVD is updated
    // lambda syntax: [capture](parameters) -> return_type { body }
    m_processor.onCvdUpdated([this](double newCvd){
        emit cvdUpdated(newCvd);
    });
}

StatisticsProcessor* StatisticsController::processor()
{
    return &m_processor;
}

void StatisticsController::processTrade(const Trade &trade)
{
    m_processor.processTrade(trade);
} 