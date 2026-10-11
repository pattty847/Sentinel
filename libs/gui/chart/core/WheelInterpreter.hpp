#pragma once

namespace chart {
struct WheelInterpreter {
  static constexpr int kNoScrollPhase = 0, kScrollEnd = 3;
  enum class Kind { Notch, End, Continuous, Ignore };
  // Phase values match Qt's scroll phases; classification itself has no Qt dependency.
  static Kind classify(int angle, bool pixelDeltaNull, int phase, bool precise) {
    // Qt 6.11.2 Cocoa qnsview_mouse.mm:647-675 gives plain wheels pixels too, but
    // only precise scrolls have SynthesizedBySystem source; gestures have phases.
    // Device type cannot veto a notch: Cocoa shares deviceID 0 and its TouchPad
    // promotion is sticky (pointingDeviceFor, scrollWheel). QML has no source, so
    // axes use phase/deltas. Unphased non-precise 120-unit multiples are clicks even
    // with pixels; null-pixel partial angles retain the high-resolution wheel carry.
    const bool notch = phase == kNoScrollPhase && !precise &&
                       (pixelDeltaNull || (angle != 0 && angle % 120 == 0));
    if (notch) return Kind::Notch;
    if (phase == kScrollEnd) return Kind::End;
    if (angle != 0) return Kind::Continuous;
    return Kind::Ignore;
  }
};
} // namespace chart
