#include "chart/core/TpoStyle.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <locale>
#include <sstream>

namespace tpo {
namespace {
template <class E> struct Named {
    const char* name;
    E value;
};

constexpr std::array<Named<Theme>, 4> kThemes{{
    {"rainbow", Theme::Rainbow}, {"calm", Theme::Calm}, {"sage", Theme::Sage}, {"indigo", Theme::Indigo}}};
constexpr std::array<Named<LineReach>, 2> kReaches{{{"profile", LineReach::Profile}, {"session", LineReach::Session}}};
constexpr std::array<Named<Preset>, 4> kPresets{{
    {"letters", Preset::Letters}, {"blocks", Preset::Blocks}, {"extended", Preset::Extended}, {"indigo", Preset::Indigo}}};

template <class E, size_t N> bool lookup(const std::array<Named<E>, N>& table, std::string_view name, E& out) {
    for (const auto& entry : table) {
        if (name == entry.name) {
            out = entry.value;
            return true;
        }
    }
    return false;
}

template <class E, size_t N> const char* nameOf(const std::array<Named<E>, N>& table, E value) {
    for (const auto& entry : table) {
        if (entry.value == value) return entry.name;
    }
    return table.front().name;
}

using ElementField = Element StyleModel::*;
constexpr std::array<Named<ElementField>, 10> kElements{{
    {"cells", &StyleModel::cells}, {"letters", &StyleModel::letters}, {"outsideVa", &StyleModel::outsideVa},
    {"valueArea", &StyleModel::valueArea}, {"poc", &StyleModel::poc}, {"vah", &StyleModel::vah},
    {"val", &StyleModel::val}, {"ibh", &StyleModel::ibh}, {"ibl", &StyleModel::ibl}, {"tags", &StyleModel::tags}}};

constexpr double kMaxWidth = 32.0;

Element element(Rgba color, double opacity, bool visible, double width) {
    return Element{color, opacity, visible, width};
}

StyleModel lettersPreset(Theme theme) {
    StyleModel s;
    s.theme = theme;
    s.reach = LineReach::Profile;
    s.cells = element({0, 0, 0, 255}, 0.95, true, 1.0);
    s.letters = element({240, 243, 247, 255}, 1.0, true, 13.0);
    s.outsideVa = element(kChartBackground, 0.55, true, 0.0);
    s.valueArea = element({143, 192, 255, 255}, 0.08, false, 0.0);
    s.poc = element({226, 229, 234, 255}, 0.80, true, 1.0);
    s.vah = element({201, 209, 220, 255}, 0.55, true, 1.0);
    s.val = s.vah;
    s.ibh = element({169, 155, 224, 255}, 0.55, true, 1.0);
    s.ibl = s.ibh;
    s.tags = element({240, 243, 247, 255}, 0.90, true, 13.0);
    return s;
}

uint8_t to8(double f) {
    return static_cast<uint8_t>(std::clamp(std::lround(f * 255.0), 0L, 255L));
}

Rgba hsv(double hDeg, double s, double v) {
    const double h = std::fmod(std::fmod(hDeg, 360.0) + 360.0, 360.0) / 60.0;
    const double c = v * s;
    const double x = c * (1.0 - std::abs(std::fmod(h, 2.0) - 1.0));
    static constexpr int kOrder[6][3] = {{0, 1, 2}, {1, 0, 2}, {2, 0, 1}, {2, 1, 0}, {1, 2, 0}, {0, 2, 1}};
    const double parts[3] = {c, x, 0.0};
    const auto& order = kOrder[std::clamp(static_cast<int>(h), 0, 5)];
    const double m = v - c;
    return {to8(parts[order[0]] + m), to8(parts[order[1]] + m), to8(parts[order[2]] + m), 255};
}

Rgba mix(const Rgba& a, const Rgba& b, double t) {
    auto lerp = [t](uint8_t x, uint8_t y) {
        return static_cast<uint8_t>(std::lround(x + (static_cast<double>(y) - x) * t));
    };
    return {lerp(a.r, b.r), lerp(a.g, b.g), lerp(a.b, b.b), lerp(a.a, b.a)};
}

Rgba themeRamp(Theme theme, double t) {
    switch (theme) {
    case Theme::Rainbow: return hsv(280.0 * t, 0.86, 0.92);  // red -> violet across the session
    case Theme::Calm: return mix({70, 112, 182, 255}, {98, 142, 210, 255}, t);
    case Theme::Sage: return mix({104, 140, 100, 255}, {128, 160, 116, 255}, t);
    case Theme::Indigo: return mix({46, 42, 120, 255}, {143, 192, 255, 255}, t);
    }
    return {226, 229, 234, 255};
}

bool parseNumber(const std::string& text, double& out) {
    std::istringstream in(text);
    in.imbue(std::locale::classic());
    double value = 0.0;
    in >> value;
    if (in.fail() || !in.eof() || !std::isfinite(value)) return false;
    out = value;
    return true;
}

std::string formatNumber(double value) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << value;
    return out.str();
}

bool applyProperty(Element& e, std::string_view property, const std::string& value) {
    double number = 0.0;
    if (property == "color") return parseHexColor(value, e.color);
    if (property == "visible") {
        if (value != "true" && value != "false") return false;
        e.visible = value == "true";
        return true;
    }
    if (!parseNumber(value, number)) return false;
    if (property == "opacity") e.opacity = std::clamp(number, 0.0, 1.0);
    else if (property == "width") e.width = std::clamp(number, 0.0, kMaxWidth);
    else return false;
    return true;
}

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
} // namespace

Theme parseTheme(std::string_view name, Theme fallback) {
    Theme out = fallback;
    return lookup(kThemes, name, out) ? out : fallback;
}
const char* themeName(Theme theme) { return nameOf(kThemes, theme); }
LineReach parseReach(std::string_view name, LineReach fallback) {
    LineReach out = fallback;
    return lookup(kReaches, name, out) ? out : fallback;
}
const char* reachName(LineReach reach) { return nameOf(kReaches, reach); }
bool parsePreset(std::string_view name, Preset& out) { return lookup(kPresets, name, out); }
const char* presetName(Preset preset) { return nameOf(kPresets, preset); }

StyleModel presetStyle(Preset preset, Theme theme) {
    StyleModel s = lettersPreset(preset == Preset::Indigo ? Theme::Indigo : theme);
    if (preset == Preset::Blocks) s.letters.visible = false;
    if (preset == Preset::Extended) s.reach = LineReach::Session;
    return s;
}

bool matchPreset(const StyleModel& style, Preset& out) {
    for (const Preset preset : {Preset::Indigo, Preset::Letters, Preset::Blocks, Preset::Extended}) {
        if (presetStyle(preset, style.theme) == style) {
            out = preset;
            return true;
        }
    }
    return false;
}

Rgba elementColor(const Element& element) {
    Rgba c = element.color;
    c.a = to8(element.opacity);
    return c;
}

Rgba cellColor(const StyleModel& style, int period, int periods, bool inValueArea, bool poc) {
    Rgba fill;
    if (poc) {
        fill = style.poc.color;
    } else {
        const double t = periods > 1 ? std::clamp(static_cast<double>(period) / (periods - 1), 0.0, 1.0) : 0.0;
        fill = themeRamp(style.theme, t);
        if (!inValueArea && style.outsideVa.visible) {
            fill = mix(kChartBackground, fill, std::clamp(style.outsideVa.opacity, 0.0, 1.0));
        }
    }
    fill.a = to8(style.cells.opacity);
    return fill;
}

bool prefersDarkText(const Rgba& cell) {
    const double luma = 0.2126 * cell.r + 0.7152 * cell.g + 0.0722 * cell.b;
    return luma > 150.0;
}

bool parseHexColor(std::string_view text, Rgba& out) {
    if (text.size() != 7 || text[0] != '#') return false;
    int v[6];
    for (int i = 0; i < 6; ++i) {
        v[i] = hexDigit(text[static_cast<size_t>(i) + 1]);
        if (v[i] < 0) return false;
    }
    out = {static_cast<uint8_t>(v[0] * 16 + v[1]), static_cast<uint8_t>(v[2] * 16 + v[3]),
           static_cast<uint8_t>(v[4] * 16 + v[5]), 255};
    return true;
}

std::string hexColor(const Rgba& color) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out = "#";
    for (const uint8_t channel : {color.r, color.g, color.b}) {
        out += kDigits[channel >> 4];
        out += kDigits[channel & 15];
    }
    return out;
}

KeyValues toKeyValues(const StyleModel& style) {
    KeyValues out{{"theme", themeName(style.theme)}, {"reach", reachName(style.reach)}};
    for (const auto& entry : kElements) {
        const Element& e = style.*(entry.value);
        const std::string prefix = std::string(entry.name) + ".";
        out.emplace_back(prefix + "color", hexColor(e.color));
        out.emplace_back(prefix + "opacity", formatNumber(e.opacity));
        out.emplace_back(prefix + "visible", e.visible ? "true" : "false");
        out.emplace_back(prefix + "width", formatNumber(e.width));
    }
    return out;
}

StyleModel fromKeyValues(const KeyValues& values, StyleModel base) {
    for (const auto& [key, value] : values) {
        if (key == "theme") {
            base.theme = parseTheme(value, base.theme);
            continue;
        }
        if (key == "reach") {
            base.reach = parseReach(value, base.reach);
            continue;
        }
        const size_t dot = key.find('.');
        ElementField field = nullptr;
        if (dot == std::string::npos || !lookup(kElements, std::string_view(key).substr(0, dot), field)) continue;
        Element candidate = base.*field;
        if (applyProperty(candidate, std::string_view(key).substr(dot + 1), value)) base.*field = candidate;
    }
    return base;
}

} // namespace tpo
