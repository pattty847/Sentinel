#pragma once
// Where a centred time-axis label survives placement. AxisTextService draws a time
// label only if its measured run lies fully inside the axis; TimeAxisModel uses the
// same rule, with a conservative width, to put the year on a label that is drawn.
#include "AxisLayout.hpp"

namespace AxisLabelFit {
// AxisTextService's placement rule: the run's left and right inside [safeLeft, safeRight].
inline bool inside(double left, double right, double safeLeft, double safeRight) {
    return left >= safeLeft && right <= safeRight;
}
// The widest a label of `chars` characters can draw before the atlas is known: the
// largest axis label size and a wide-glyph advance (0.75 em). Wider than real text,
// so a label it accepts is always drawn.
constexpr double conservativeWidthPx(int chars) { return chars * 0.75 * AxisLayout::kAutoAxisLabelMaxPx; }
inline bool centredFits(double centre, int chars, double axisLeft, double axisRight) {
    const double half = 0.5 * conservativeWidthPx(chars);
    return inside(centre - half, centre + half, axisLeft, axisRight);
}
} // namespace AxisLabelFit
