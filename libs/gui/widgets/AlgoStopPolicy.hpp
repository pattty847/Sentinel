#pragma once

#include <string_view>

// What the paper ticket does with the position after the user stops an algo. The
// server already canceled the algo's resting quotes; this only decides the flatten.
namespace paper {

enum class OnAlgoStop { Ask, Always, Never };
enum class AlgoStopStep { Prompt, Flatten, Keep };

inline constexpr std::string_view settingValue(OnAlgoStop policy) {
    switch (policy) {
    case OnAlgoStop::Always: return "always";
    case OnAlgoStop::Never: return "never";
    case OnAlgoStop::Ask: break;
    }
    return "ask";
}

// Anything unknown (missing key, older or hand-edited value) asks.
inline constexpr OnAlgoStop onAlgoStopFromSetting(std::string_view value) {
    if (value == "always") return OnAlgoStop::Always;
    if (value == "never") return OnAlgoStop::Never;
    return OnAlgoStop::Ask;
}

inline constexpr AlgoStopStep stepAfterAlgoStop(OnAlgoStop policy) {
    switch (policy) {
    case OnAlgoStop::Always: return AlgoStopStep::Flatten;
    case OnAlgoStop::Never: return AlgoStopStep::Keep;
    case OnAlgoStop::Ask: break;
    }
    return AlgoStopStep::Prompt;
}

// The prompt's answer: true = Yes (flatten).
inline constexpr bool flattenAfterAlgoStop(OnAlgoStop policy, bool promptAnswer) {
    switch (stepAfterAlgoStop(policy)) {
    case AlgoStopStep::Flatten: return true;
    case AlgoStopStep::Keep: return false;
    case AlgoStopStep::Prompt: break;
    }
    return promptAnswer;
}

} // namespace paper
