#pragma once
#include "servermodel/ServerDataModel.hpp"

struct FlowLiveTestAccess {
  static void tick(ServerDataModel &m) { m.publishFlow(); }
  static void stopTimer(ServerDataModel &m) { m.m_flowTimer.stop(); }
  static auto &series(ServerDataModel &m, const std::string &symbol,
                      int64_t day, int64_t tf) {
    return m.m_flowProducts.at(symbol).days.at(day).series.at(tf);
  }
  static size_t subscriptions(ServerDataModel &m) {
    return m.m_flowSubscriptions.size();
  }
  static size_t seriesCount(ServerDataModel &m, int64_t day) {
    return m.m_flowProducts.at("BTC-USD").days.at(day).series.size();
  }
  static size_t hourCount(ServerDataModel &m, int64_t day) {
    return m.m_flowProducts.at("BTC-USD").days.at(day).hours.size();
  }
  static uint64_t missingGridDrops(ServerDataModel &m) { return m.m_flowMissingGridTrades; }
  static uint64_t totalDrops(ServerDataModel &m) { return m.m_flowDroppedTrades; }
  static uint64_t hourShrinkRefusals(ServerDataModel &m) { return m.m_flowHourShrinkRefusals; }
  static const auto& hour(ServerDataModel &m, int64_t day, int64_t start) {
    return m.m_flowProducts.at("BTC-USD").days.at(day).hours.at(start);
  }
  static void discardEarlyMinutesAndRecompose(ServerDataModel &m, int64_t day,
                                               int64_t hourStart) {
    auto &product = m.m_flowProducts.at("BTC-USD");
    auto &state = product.days.at(day);
    auto &minute = state.series.at(60'000);
    const int64_t keep = hourStart + 59 * 60'000;
    minute.builder.dropBefore(keep);
    for (auto &slot : minute.bucketSlots)
      if (slot.start >= hourStart && slot.start < keep) slot = {};
    m.updateHour(state, product, hourStart);
  }
  static const auto& coarse(ServerDataModel &m, int64_t day, int64_t tf, int64_t start) {
    return m.m_flowProducts.at("BTC-USD").days.at(day).coarse.at({tf, start});
  }
};

