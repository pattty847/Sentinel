#include "CvdThresholdRule.h"

CvdThresholdRule::CvdThresholdRule(double threshold)
    : m_threshold(threshold), m_hasFired(false)
{
    // The alert message is prepared once in the constructor
    m_alertMessage = QString("CVD has crossed the configured threshold of %1").arg(m_threshold);
}

/**
 * @brief Checks if the CVD has crossed the threshold.
 * 
 * This implementation contains state. It will only return true once.
 * If the CVD later drops below the threshold, this rule would need
 * to be reset (a feature we can add later).
 * @return true if the threshold is crossed for the first time, false otherwise.
 */
bool CvdThresholdRule::check(const Trade &trade, double cvd)
{
    // We don't need the 'trade' object for this specific rule, but other rules might.
    (void)trade; // This prevents a compiler warning about an unused parameter.

    if (!m_hasFired && cvd > m_threshold) {
        m_hasFired = true; // Mark that this alert has fired
        return true;       // Trigger the alert
    }
    
    // Optional: Add logic to reset m_hasFired if CVD drops below a certain level
    // if (cvd < m_threshold) {
    //     m_hasFired = false;
    // }

    return false; // Do not trigger
}

QString CvdThresholdRule::getAlertMessage() const
{
    return m_alertMessage;
} 