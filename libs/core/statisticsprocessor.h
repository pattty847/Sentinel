#ifndef STATISTICSPROCESSOR_H
#define STATISTICSPROCESSOR_H

#include "TradeData.h"
#include <functional> // Include for std::function

/**
 * StatisticsProcessor class: A pure C++ data processor for calculating statistics.
 *
 * This class has no knowledge of Qt or any UI framework. Its only job is to
 * receive raw trade data and produce calculated statistics. It uses a C++
 * callback to notify external code of updates.
 */
class StatisticsProcessor
{
public:
    // Define a type for our callback function for cleaner code.
    // It will be a function that takes a double and returns void.
    using CvdUpdateCallback = std::function<void(double)>;

    StatisticsProcessor();

    // The main entry point for processing new trades.
    void processTrade(const Trade &trade);

    // A public getter to allow other objects to query the current CVD
    double getCvd() const;

    // A method to register a callback function for CVD updates.
    void onCvdUpdated(CvdUpdateCallback callback);

private:
    double m_cvd;
    CvdUpdateCallback m_cvdUpdateCallback; // Member to store the callback
};

#endif // STATISTICSPROCESSOR_H 