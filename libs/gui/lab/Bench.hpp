#pragma once
#include <QString>
#include <cstdint>

namespace lab {
int runBench(int hours, const QString &layer, uint32_t synthetic, int tfMinutes = 1);
int runScreenshot(int hours, const QString &layer, uint32_t synthetic, int tfMinutes,
                  const QString &path);
}
