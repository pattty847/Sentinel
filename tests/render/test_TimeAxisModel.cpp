// The time axis in the chart's display zone (DisplayTime): UTC labels and grid stay
// as they were; Local mode labels local clock times and puts 1h/4h/1d ticks on local
// hours and midnights, across a daylight-saving change and in a 30-minute-offset
// zone. The zone is injected, never the machine's.
#include "models/DisplayTime.hpp"
#include "models/TimeAxisModel.hpp"
#include "render/GridViewState.hpp"

#include <gtest/gtest.h>

#include <QDateTime>
#include <QTimeZone>

#include <vector>

namespace {
constexpr qint64 kMinute = 60'000, kHour = 60 * kMinute, kDay = 24 * kHour;

qint64 utcMs(int y, int m, int d, int h, int min = 0) {
    return QDateTime(QDate(y, m, d), QTime(h, min), QTimeZone::UTC).toMSecsSinceEpoch();
}

class TimeAxisModelZone : public ::testing::Test {
protected:
    void TearDown() override {
        DisplayTime::instance().setZone(DisplayTime::Zone::Utc);
        DisplayTime::instance().setSystemZoneIdSource({});
        DisplayTime::instance().setLocalZone(QTimeZone());
    }
    static void local(const char* zone) {
        DisplayTime::instance().setLocalZone(QTimeZone(zone));
        DisplayTime::instance().setZone(DisplayTime::Zone::Local);
    }
    // 880 px: 11 labels at 80 px, so a 10-step range picks that step.
    std::vector<AxisModel::TickSnapshot> ticks(qint64 from, qint64 to) {
        axis.setViewportSize(880.0, 400.0);
        axis.setGridViewState(&view);
        view.setViewport(from, to, 100.0, 200.0);
        return current();
    }
    std::vector<AxisModel::TickSnapshot> current() const {
        std::vector<AxisModel::TickSnapshot> out;
        axis.copyTicks(out);
        return out;
    }
    static QStringList labels(const std::vector<AxisModel::TickSnapshot>& list) {
        QStringList out;
        for (const auto& t : list) out << t.label;
        return out;
    }
    static void expectLocalGrid(const std::vector<AxisModel::TickSnapshot>& list, const char* zone, qint64 stepMs) {
        ASSERT_GE(list.size(), 3u);
        for (const auto& t : list) {
            const auto wall = QDateTime::fromMSecsSinceEpoch(qint64(t.value), QTimeZone(zone));
            const qint64 sinceMidnight = wall.time().msecsSinceStartOfDay();
            EXPECT_EQ(sinceMidnight % stepMs, 0) << wall.toString(Qt::ISODate).toStdString();
        }
    }
    GridViewState view;
    TimeAxisModel axis;
};
} // namespace

TEST_F(TimeAxisModelZone, UtcGridAndLabelsAreUtc) {
    const auto list = ticks(utcMs(2026, 3, 7, 20), utcMs(2026, 3, 8, 6));
    ASSERT_GE(list.size(), 9u);
    for (const auto& t : list) EXPECT_EQ(qint64(t.value) % kHour, 0);
    EXPECT_TRUE(labels(list).contains("8 Mar")); // the date label at UTC midnight
    EXPECT_TRUE(labels(list).contains("21:00"));
}

TEST_F(TimeAxisModelZone, LocalHoursSkipTheSpringForwardHourInNewYork) {
    local("America/New_York"); // 2026-03-08 02:00 EST -> 03:00 EDT (07:00Z)
    const auto list = ticks(utcMs(2026, 3, 8, 2), utcMs(2026, 3, 8, 12));
    expectLocalGrid(list, "America/New_York", kHour);
    const QStringList text = labels(list);
    const int before = int(text.indexOf("01:00"));
    ASSERT_GE(before, 0) << text.join(' ').toStdString();
    EXPECT_EQ(text.value(before + 1), "03:00") << text.join(' ').toStdString();
    EXPECT_FALSE(text.contains("02:00"));
    EXPECT_TRUE(text.contains("8 Mar")) << "the date label at local midnight (05:00Z)";
    for (const auto& t : list)
        if (t.label == "8 Mar") EXPECT_EQ(qint64(t.value), utcMs(2026, 3, 8, 5));
}

TEST_F(TimeAxisModelZone, FourHourTicksLandOnLocalHoursAcrossBothNewYorkChanges) {
    local("America/New_York");
    auto spring = ticks(utcMs(2026, 3, 7, 12), utcMs(2026, 3, 9, 4));
    expectLocalGrid(spring, "America/New_York", 4 * kHour);
    bool sawShortGap = false;
    for (size_t i = 1; i < spring.size(); ++i)
        sawShortGap = sawShortGap || qint64(spring[i].value - spring[i - 1].value) == 3 * kHour;
    EXPECT_TRUE(sawShortGap) << "00:00 EST to 04:00 EDT is 3 hours";

    // 2026-11-01 02:00 EDT -> 01:00 EST (06:00Z): 00:00 EDT (04Z) to 04:00 EST (09Z).
    auto fall = ticks(utcMs(2026, 10, 31, 12), utcMs(2026, 11, 2, 4));
    expectLocalGrid(fall, "America/New_York", 4 * kHour);
    bool sawLongGap = false;
    for (size_t i = 1; i < fall.size(); ++i)
        if (qint64(fall[i].value) == utcMs(2026, 11, 1, 9))
            sawLongGap = qint64(fall[i - 1].value) == utcMs(2026, 11, 1, 4);
    EXPECT_TRUE(sawLongGap) << labels(fall).join(' ').toStdString();
}

TEST_F(TimeAxisModelZone, FallBackRepeatsTheLocalHour) {
    local("America/New_York");
    const auto list = ticks(utcMs(2026, 11, 1, 1), utcMs(2026, 11, 1, 11));
    expectLocalGrid(list, "America/New_York", kHour);
    EXPECT_EQ(labels(list).count("01:00"), 2) << labels(list).join(' ').toStdString();
}

TEST_F(TimeAxisModelZone, DayTicksAreLocalMidnights) {
    local("America/New_York");
    const auto list = ticks(utcMs(2026, 3, 3, 12), utcMs(2026, 3, 13, 12));
    expectLocalGrid(list, "America/New_York", kDay);
    for (size_t i = 1; i < list.size(); ++i) {
        const auto day = QDateTime::fromMSecsSinceEpoch(qint64(list[i].value), QTimeZone("America/New_York")).date();
        EXPECT_EQ(list[i].label, QString::number(day.day()));
        if (day == QDate(2026, 3, 9)) EXPECT_EQ(qint64(list[i].value - list[i - 1].value), 23 * kHour);
    }
}

TEST_F(TimeAxisModelZone, KolkataHalfHourOffsetTicksAndDate) {
    local("Asia/Kolkata"); // UTC+5:30, no DST
    const auto hours = ticks(utcMs(2026, 3, 7, 14), utcMs(2026, 3, 8, 0));
    expectLocalGrid(hours, "Asia/Kolkata", kHour);
    for (const auto& t : hours) EXPECT_EQ(qint64(t.value) % kHour, 30 * kMinute);
    EXPECT_TRUE(labels(hours).contains("8 Mar")) << labels(hours).join(' ').toStdString();
    for (const auto& t : hours)
        if (t.label == "8 Mar") EXPECT_EQ(qint64(t.value), utcMs(2026, 3, 7, 18, 30));
    expectLocalGrid(ticks(utcMs(2026, 3, 3, 0), utcMs(2026, 3, 13, 0)), "Asia/Kolkata", kDay);
}

TEST_F(TimeAxisModelZone, ToggleRelabelsWithoutAnythingElseChanging) {
    const auto utc = ticks(utcMs(2026, 3, 7, 20), utcMs(2026, 3, 8, 6));
    local("Asia/Kolkata");
    const auto kolkata = current();
    ASSERT_FALSE(kolkata.empty());
    EXPECT_NE(labels(kolkata), labels(utc));
    for (const auto& t : kolkata) EXPECT_EQ(qint64(t.value) % kHour, 30 * kMinute);
    DisplayTime::instance().setZone(DisplayTime::Zone::Utc);
    EXPECT_EQ(labels(current()), labels(utc));
}

TEST(DisplayTimeZone, LabelsStateTheActiveZone) {
    auto& time = DisplayTime::instance();
    const qint64 winter = utcMs(2026, 1, 15, 12), summer = utcMs(2026, 7, 15, 12);
    EXPECT_EQ(time.zoneLabel(winter), "UTC");
    time.setLocalZone(QTimeZone("America/New_York"));
    EXPECT_EQ(time.zoneLabel(winter), "UTC") << "the local zone is unused in UTC mode";
    time.setZone(DisplayTime::Zone::Local);
    EXPECT_EQ(time.zoneLabel(winter), "UTC-5");
    EXPECT_EQ(time.zoneLabel(summer), "UTC-4");
    EXPECT_EQ(time.format(summer, "HH:mm"), "08:00");
    time.setLocalZone(QTimeZone("Asia/Kolkata"));
    EXPECT_EQ(time.zoneLabel(winter), "UTC+5:30");
    EXPECT_EQ(time.format(winter, "HH:mm"), "17:30");
    time.setLocalZone(QTimeZone("Asia/Kathmandu"));
    EXPECT_EQ(time.zoneLabel(winter), "UTC+5:45");
    EXPECT_EQ(time.nextBoundary(utcMs(2026, 1, 15, 12), kHour), utcMs(2026, 1, 15, 12, 15)); // 18:00 local
    time.setZone(DisplayTime::Zone::Utc);
    time.setLocalZone(QTimeZone());
    EXPECT_EQ(DisplayTime::parse("local"), DisplayTime::Zone::Local);
    EXPECT_EQ(DisplayTime::parse("utc"), DisplayTime::Zone::Utc);
}

// A range across a daylight-saving change states each end's own offset, so two
// different instants never print alike; within one offset (and in UTC) one suffix.
TEST(DisplayTimeZone, RangeAcrossAChangeStatesBothOffsets) {
    auto& time = DisplayTime::instance();
    const QString pattern = "MM-dd HH:mm";
    const qint64 fallFrom = utcMs(2026, 11, 1, 5), fallTo = utcMs(2026, 11, 1, 6);
    const qint64 springFrom = utcMs(2026, 3, 8, 6), springTo = utcMs(2026, 3, 8, 7);
    EXPECT_EQ(time.rangeText(fallFrom, fallTo, pattern), "11-01 05:00 to 11-01 06:00 UTC"); // the former UTC text
    time.setLocalZone(QTimeZone("America/New_York"));
    time.setZone(DisplayTime::Zone::Local);
    EXPECT_EQ(time.rangeText(fallFrom, fallTo, pattern), "11-01 01:00 UTC-4 to 11-01 01:00 UTC-5");
    EXPECT_EQ(time.rangeText(springFrom, springTo, pattern), "03-08 01:00 UTC-5 to 03-08 03:00 UTC-4");
    EXPECT_EQ(time.rangeText(utcMs(2026, 7, 1, 12), utcMs(2026, 7, 1, 14), pattern), "07-01 08:00 to 07-01 10:00 UTC-4");
    time.setZone(DisplayTime::Zone::Utc);
    time.setLocalZone(QTimeZone());
    EXPECT_EQ(time.rangeText(springFrom, springTo, pattern), "03-08 06:00 to 03-08 07:00 UTC");
}

// Local mode follows the computer's zone without a restart: a changed system zone
// re-labels the axis, an unchanged one emits nothing, selecting Local re-reads it,
// and an injected zone wins over the system one.
TEST_F(TimeAxisModelZone, LocalFollowsASystemZoneChange) {
    auto& time = DisplayTime::instance();
    QByteArray system = "America/New_York";
    time.setSystemZoneIdSource([&system] { return system; });
    time.setLocalZone(QTimeZone()); // follow the (fake) system zone
    time.setZone(DisplayTime::Zone::Local);
    int changes = 0;
    const auto counter = QObject::connect(&time, &DisplayTime::zoneChanged, [&changes] { ++changes; });
    const auto newYork = ticks(utcMs(2026, 7, 1, 2), utcMs(2026, 7, 1, 12));
    EXPECT_TRUE(labels(newYork).contains("1 Jul")) << labels(newYork).join(' ').toStdString(); // 04:00Z
    EXPECT_FALSE(time.refreshSystemZone());
    EXPECT_EQ(changes, 0) << "no change emits nothing";
    system = "America/Los_Angeles";
    EXPECT_TRUE(time.refreshSystemZone());
    EXPECT_EQ(changes, 1);
    const auto losAngeles = current();
    EXPECT_TRUE(labels(losAngeles).contains("1 Jul")) << labels(losAngeles).join(' ').toStdString();
    for (const auto& t : losAngeles)
        if (t.label == "1 Jul") EXPECT_EQ(qint64(t.value), utcMs(2026, 7, 1, 7)) << "Los Angeles midnight";
    time.setZone(DisplayTime::Zone::Utc);
    system = "Asia/Kolkata"; // changed while UTC is shown
    time.setZone(DisplayTime::Zone::Local);
    EXPECT_EQ(time.zoneLabel(utcMs(2026, 7, 1, 0)), "UTC+5:30") << "selecting Local re-reads the system zone";
    time.setLocalZone(QTimeZone("Asia/Tokyo"));
    changes = 0;
    system = "Europe/London";
    EXPECT_FALSE(time.refreshSystemZone());
    EXPECT_EQ(changes, 0);
    EXPECT_EQ(time.zoneLabel(utcMs(2026, 7, 1, 0)), "UTC+9") << "an injected zone wins";
    QObject::disconnect(counter);
}

// A transition that keeps the total offset (Lisbon 1992-09-27 01:00Z: summer time
// becomes standard UTC+1) just after the query must not split the segment into
// one-millisecond pieces: the next local hour is the transition instant itself.
TEST(DisplayTimeZone, OffsetKeepingTransitionStillFindsTheHour) {
    auto& time = DisplayTime::instance();
    time.setLocalZone(QTimeZone("Europe/Lisbon"));
    time.setZone(DisplayTime::Zone::Local);
    const qint64 at = utcMs(1992, 9, 27, 1);
    EXPECT_EQ(time.nextBoundary(at - 501, kHour), at);
    EXPECT_EQ(time.zoneLabel(at - 501), "UTC+1");
    time.setZone(DisplayTime::Zone::Utc);
    time.setLocalZone(QTimeZone());
}
