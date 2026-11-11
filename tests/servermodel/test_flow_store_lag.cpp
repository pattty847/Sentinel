#include "servermodel/FlowStoreLag.hpp"
#include <gtest/gtest.h>

namespace {
constexpr int64_t Hour = 3'600'000, Grace = 60'000, Epoch = 1'791'158'400'000;
}

TEST(FlowStoreLag, UnbuiltStoreWithoutLiveTradeHasNoLag) {
    for (const auto clock : {int64_t{0}, Epoch, Epoch + 8 * Hour + Grace})
        EXPECT_FALSE(sentinel::flow::storeLagMs(clock, 0, -1, Grace).has_value());
}

TEST(FlowStoreLag, FirstLiveHourAndStoreSealProvideRealReferences) {
    EXPECT_EQ(sentinel::flow::storeLagMs(Epoch + 8 * Hour + Grace, 0,
                                       Epoch + Hour + 123, Grace), 7 * Hour);
    EXPECT_EQ(sentinel::flow::storeLagMs(Epoch + 8 * Hour + Grace, Epoch + 2 * Hour,
                                       -1, Grace), 6 * Hour);
    EXPECT_EQ(sentinel::flow::storeLagMs(Epoch + 8 * Hour + Grace, Epoch + 2 * Hour,
                                       Epoch + 3 * Hour, Grace), 5 * Hour);
    EXPECT_EQ(sentinel::flow::storeLagMs(Epoch + 8 * Hour + Grace - 1, 0,
                                       Epoch + Hour, Grace), 6 * Hour);
    EXPECT_EQ(sentinel::flow::storeLagMs(8 * Hour + Grace, 0, 0, Grace), 8 * Hour);
}
