#include <gtest/gtest.h>

#include "chart/core/TpoStyle.hpp"

#include <algorithm>

using namespace tpo;

namespace {
int brightness(const Rgba& c) { return c.r + c.g + c.b; }
} // namespace

TEST(TpoStyle, DefaultPresetCarriesTheOwnerLabPicks) {
    const StyleModel s = presetStyle(Preset::Letters);
    EXPECT_EQ(s.theme, Theme::Rainbow);
    EXPECT_TRUE(s.letters.visible);                 // T1: letters
    EXPECT_DOUBLE_EQ(s.cells.opacity, 0.95);        // T2: 95% over candles
    EXPECT_TRUE(s.outsideVa.visible);               // T3: outside rows dimmed
    EXPECT_LT(s.outsideVa.opacity, 1.0);
    EXPECT_EQ(s.reach, LineReach::Profile);         // T4: lines across the profile
    EXPECT_TRUE(s.tags.visible);                    //     with tags
    for (const Element* level : {&s.poc, &s.vah, &s.val, &s.ibh, &s.ibl}) {
        EXPECT_TRUE(level->visible);
        EXPECT_GT(level->width, 0.0);
    }
    // POC is neutral white, not the sell red.
    EXPECT_GT(std::min({s.poc.color.r, s.poc.color.g, s.poc.color.b}), 200);
    EXPECT_DOUBLE_EQ(s.cells.width, 1.0);           // today's cell gap
}

TEST(TpoStyle, PresetsChangeOnlyTheirOwnProperties) {
    const StyleModel base = presetStyle(Preset::Letters, Theme::Sage);
    StyleModel blocks = presetStyle(Preset::Blocks, Theme::Sage);
    EXPECT_FALSE(blocks.letters.visible);
    blocks.letters.visible = true;
    EXPECT_EQ(blocks, base);

    StyleModel extended = presetStyle(Preset::Extended, Theme::Sage);
    EXPECT_EQ(extended.reach, LineReach::Session);
    extended.reach = LineReach::Profile;
    EXPECT_EQ(extended, base);

    const StyleModel indigo = presetStyle(Preset::Indigo, Theme::Sage);
    EXPECT_EQ(indigo.theme, Theme::Indigo);
    EXPECT_EQ(indigo, presetStyle(Preset::Letters, Theme::Indigo));
}

TEST(TpoStyle, MatchPresetFindsUneditedPresetsOnly) {
    Preset found = Preset::Letters;
    for (const Preset p : {Preset::Letters, Preset::Blocks, Preset::Extended, Preset::Indigo}) {
        ASSERT_TRUE(matchPreset(presetStyle(p, Theme::Calm), found)) << presetName(p);
        if (p != Preset::Letters) EXPECT_EQ(found, p);
    }
    StyleModel edited = presetStyle(Preset::Blocks);
    edited.poc.color = {246, 70, 93, 255};
    EXPECT_FALSE(matchPreset(edited, found));
}

TEST(TpoStyle, NamesRoundTrip) {
    for (const Theme t : {Theme::Rainbow, Theme::Calm, Theme::Sage, Theme::Indigo})
        EXPECT_EQ(parseTheme(themeName(t), Theme::Calm == t ? Theme::Sage : Theme::Calm), t);
    EXPECT_EQ(parseTheme("turquoise", Theme::Sage), Theme::Sage);
    EXPECT_EQ(parseReach(reachName(LineReach::Session), LineReach::Profile), LineReach::Session);
    Preset p = Preset::Letters;
    EXPECT_TRUE(parsePreset("extended", p));
    EXPECT_EQ(p, Preset::Extended);
    EXPECT_FALSE(parsePreset("split", p));
    EXPECT_EQ(p, Preset::Extended);
}

TEST(TpoStyle, CellColourFollowsThemeValueAreaAndPoc) {
    StyleModel s = presetStyle(Preset::Letters);
    const Rgba first = cellColor(s, 0, 48, true, false);
    const Rgba last = cellColor(s, 47, 48, true, false);
    EXPECT_GT(first.r, first.b);  // rainbow starts red
    EXPECT_GT(last.b, last.g);    // and ends violet
    EXPECT_EQ(first.a, 242);      // 95% fill

    for (const Theme theme : {Theme::Rainbow, Theme::Calm, Theme::Sage, Theme::Indigo}) {
        s.theme = theme;
        s.outsideVa.visible = true;
        const Rgba inside = cellColor(s, 10, 48, true, false);
        const Rgba outside = cellColor(s, 10, 48, false, false);
        EXPECT_GT(brightness(inside), brightness(outside)) << themeName(theme);
        s.outsideVa.visible = false;
        EXPECT_EQ(cellColor(s, 10, 48, false, false), inside) << themeName(theme);
        EXPECT_TRUE(prefersDarkText(cellColor(s, 10, 48, true, true))) << themeName(theme);
    }

    s = presetStyle(Preset::Indigo);
    EXPECT_EQ(cellColor(s, 0, 10, true, false), (Rgba{0x2e, 0x2a, 0x78, 242}));
    EXPECT_EQ(cellColor(s, 9, 10, true, false), (Rgba{0x8f, 0xc0, 0xff, 242}));

    s.poc.color = {246, 70, 93, 255};  // the red POC option
    s.cells.opacity = 0.5;
    EXPECT_EQ(cellColor(s, 3, 10, true, true), (Rgba{246, 70, 93, 128}));
}

TEST(TpoStyle, OutsideOpacityMixesTowardTheBackground) {
    StyleModel s = presetStyle(Preset::Letters, Theme::Calm);
    s.outsideVa.opacity = 0.0;
    Rgba bg = kChartBackground;
    bg.a = 242;
    EXPECT_EQ(cellColor(s, 0, 10, false, false), bg);
    s.outsideVa.opacity = 1.0;
    EXPECT_EQ(cellColor(s, 0, 10, false, false), cellColor(s, 0, 10, true, false));
}

TEST(TpoStyle, HexColours) {
    Rgba c;
    ASSERT_TRUE(parseHexColor("#2E2a78", c));
    EXPECT_EQ(c, (Rgba{0x2e, 0x2a, 0x78, 255}));
    EXPECT_EQ(hexColor(c), "#2e2a78");
    EXPECT_FALSE(parseHexColor("2e2a78", c));
    EXPECT_FALSE(parseHexColor("#2e2a7", c));
    EXPECT_FALSE(parseHexColor("#2e2a7g", c));
}

TEST(TpoStyle, KeyValuesRoundTripEveryProperty) {
    StyleModel s = presetStyle(Preset::Extended, Theme::Indigo);
    s.cells.width = 0.0;
    s.letters.width = 15.0;
    s.valueArea.visible = true;
    s.valueArea.color = {12, 34, 56, 255};
    s.poc.color = {246, 70, 93, 255};
    s.poc.opacity = 0.25;
    s.ibl.visible = false;
    s.tags.width = 14.5;
    const KeyValues values = toKeyValues(s);
    EXPECT_EQ(values.size(), 2u + 10u * 4u);
    EXPECT_EQ(fromKeyValues(values, presetStyle(Preset::Letters)), s);
}

TEST(TpoStyle, KeyValuesSkipUnknownKeysAndBadValues) {
    const StyleModel base = presetStyle(Preset::Letters);
    const StyleModel out = fromKeyValues({{"theme", "turquoise"}, {"poc.color", "red"}, {"poc.opacity", "1.5"},
                                          {"vah.visible", "yes"}, {"ibh.width", "nan"}, {"bogus.width", "3"},
                                          {"cells.size", "3"}, {"letters", "true"}, {"val.width", "2"}},
                                         base);
    StyleModel expected = base;
    expected.poc.opacity = 1.0;  // clamped
    expected.val.width = 2.0;
    EXPECT_EQ(out, expected);
}
