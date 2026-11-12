/*
 * Sentinel – TpoStyle
 *
 * One style model for the TPO (market profile) overlay. Every drawn element
 * (cells, letters, value area, POC, VAH/VAL, IBH/IBL, axis tags) carries the
 * same four properties: colour, opacity, visible and width. Named presets are
 * plain StyleModel values; Settings edits the properties, and the renderer
 * reads only the model. Qt-free.
 *
 * Property meaning per element:
 *   cells      colour: unused (the theme ramps the fill); opacity: fill alpha;
 *              visible: draw the cells; width: gap between cells in px (0 = none).
 *   letters    colour: light letter colour (light cells get dark letters);
 *              opacity: letter alpha; visible: letters on (off = blocks);
 *              width: largest letter size in px.
 *   outsideVa  rows outside the value area. opacity: brightness they keep
 *              (0..1, mixed toward the chart background); visible: dim them.
 *   valueArea  band behind the value-area rows. colour/opacity: band fill;
 *              visible: draw the band; width: unused.
 *   poc        colour: POC row cells and the POC line; opacity/width: line.
 *   vah, val,
 *   ibh, ibl   level lines: colour, opacity, visible, width in px.
 *   tags       level tags at the plot's right edge for the newest session.
 *              colour: tag text; opacity: tag chip alpha; width: text size px.
 *
 * Level lines are drawn under the cells and reach across the profile width
 * (LineReach::Profile) or the whole session (LineReach::Session).
 */
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tpo {

struct Rgba {
    uint8_t r = 0, g = 0, b = 0, a = 255;
    bool operator==(const Rgba&) const = default;
};

enum class Theme : int { Rainbow = 0, Calm = 1, Sage = 2, Indigo = 3 };
enum class LineReach : int { Profile = 0, Session = 1 };
enum class Preset : int { Letters = 0, Blocks = 1, Extended = 2, Indigo = 3 };

struct Element {
    Rgba color;
    double opacity = 1.0;
    bool visible = true;
    double width = 1.0;
    bool operator==(const Element&) const = default;
};

struct StyleModel {
    Theme theme = Theme::Rainbow;
    LineReach reach = LineReach::Profile;
    Element cells, letters, outsideVa, valueArea, poc, vah, val, ibh, ibl, tags;
    bool operator==(const StyleModel&) const = default;
};

Theme parseTheme(std::string_view name, Theme fallback);
const char* themeName(Theme theme);
LineReach parseReach(std::string_view name, LineReach fallback);
const char* reachName(LineReach reach);
bool parsePreset(std::string_view name, Preset& out);
const char* presetName(Preset preset);

// The default preset is Letters; every preset keeps the given theme unless it names its own.
StyleModel presetStyle(Preset preset, Theme theme = Theme::Rainbow);
// The preset whose model equals style, or false when the style was edited.
bool matchPreset(const StyleModel& style, Preset& out);

// Cell fill for one TPO: the theme ramp across the session, the POC colour on
// the POC row, and outside rows mixed toward background by outsideVa.opacity.
Rgba cellColor(const StyleModel& style, int period, int periods, bool inValueArea, bool poc);
// Element colour with its opacity applied to alpha.
Rgba elementColor(const Element& element);
// True when a letter on this cell should be dark rather than light.
bool prefersDarkText(const Rgba& cell);
// Background the outside rows dim toward.
constexpr Rgba kChartBackground{15, 17, 20, 255};

// "#rrggbb"; false for anything else.
bool parseHexColor(std::string_view text, Rgba& out);
std::string hexColor(const Rgba& color);

// Flat key/value form for settings ("theme", "reach", "<element>.<property>").
using KeyValues = std::vector<std::pair<std::string, std::string>>;
KeyValues toKeyValues(const StyleModel& style);
// Applies every known, valid key over base; unknown keys and bad values are skipped.
StyleModel fromKeyValues(const KeyValues& values, StyleModel base);

} // namespace tpo
