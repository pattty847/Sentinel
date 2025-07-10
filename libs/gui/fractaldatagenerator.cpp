#include "fractaldatagenerator.h"
#include <cmath>

FractalDataGenerator::FractalDataGenerator(QObject* parent) : QObject(parent) {}

void FractalDataGenerator::generateData(int numFrames, int levelsPerFrame) {
    for (int i = 0; i < numFrames; ++i) {
        OrderBook book;
        book.timestamp = std::chrono::system_clock::now();

        for (int j = 0; j < levelsPerFrame; ++j) {
            double bidPrice = m_midPrice - (j + 1) * m_priceIncrement;
            double bidSize = (j + 1) * m_sizeIncrement;
            book.bids.emplace_back(bidPrice, bidSize);

            double askPrice = m_midPrice + (j + 1) * m_priceIncrement;
            double askSize = (j + 1) * m_sizeIncrement;
            book.asks.emplace_back(askPrice, askSize);
        }

        emit orderBookUpdated(book);

        m_midPrice += sin(i * 0.1) * 10.0;
    }
}
