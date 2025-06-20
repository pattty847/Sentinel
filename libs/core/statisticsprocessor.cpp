#include "statisticsprocessor.h"

StatisticsProcessor::StatisticsProcessor()
    : m_cvd(0.0)
{
    // The m_cvdUpdateCallback is default-initialized (to an empty function)
}

double StatisticsProcessor::getCvd() const
{
    return m_cvd;
}

void StatisticsProcessor::onCvdUpdated(CvdUpdateCallback callback)
{
    // Store the provided callback function to be called later.
    m_cvdUpdateCallback = callback;
}

void StatisticsProcessor::processTrade(const Trade &trade)
{
    // Simple CVD calculation logic
    if (trade.side == Side::Buy) {
        m_cvd += trade.size;
    } else if (trade.side == Side::Sell) {
        m_cvd -= trade.size;
    }

    // If a callback has been registered, call it with the new value.
    if (m_cvdUpdateCallback) {
        m_cvdUpdateCallback(m_cvd);
    }
}