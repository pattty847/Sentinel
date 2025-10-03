/*
Sentinel — StatisticsController
Role: Implements the logic for the statistics controller.
Inputs/Outputs: Forwards each trade to the processor and immediately emits the new CVD value.
Threading: All code is executed on the main GUI thread.
Performance: The lack of batching or throttling is a key implementation detail.
Integration: The concrete implementation of the statistics controller.
Observability: No internal logging.
Related: StatisticsController.h, StatisticsProcessor.h.
Assumptions: Immediate signal emission after each trade is the desired behavior.
*/
#include "StatisticsController.h"

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