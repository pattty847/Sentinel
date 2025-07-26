#ifndef CVDTHRESHOLDDRULE_H
#define CVDTHRESHOLDDRULE_H

#include "Rule.h" // Include the base class interface

/**
 * @class CvdThresholdRule
 * @brief A concrete rule that triggers an alert if CVD crosses a specific threshold.
 *
 * This class inherits from our abstract Rule class and provides a concrete
 * implementation for the check() and getAlertMessage() functions.
 */
class CvdThresholdRule : public Rule
{
public:
    // Constructor to set the threshold for this rule instance
    explicit CvdThresholdRule(double threshold);

    // Override the pure virtual functions from the base class
    bool check(const Trade &trade, double cvd) override;
    QString getAlertMessage() const override;

private:
    double m_threshold;       // The CVD value that triggers this rule
    bool m_hasFired;          // State to prevent the alert from firing repeatedly
    QString m_alertMessage;   // The message to send when the rule fires
};

#endif // CVDTHRESHOLDDRULE_H 