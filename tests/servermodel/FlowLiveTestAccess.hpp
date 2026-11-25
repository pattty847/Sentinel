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
  static size_t retiredRevisionMinutes(ServerDataModel& m, int64_t day) {
    return m.m_flowProducts.at("BTC-USD").days.at(day).retiredRevisionByMinute.size();
  }
  static uint64_t missingGridDrops(ServerDataModel &m) { return m.m_flowMissingGridTrades; }
  static size_t heldBytes(ServerDataModel &m, const std::string &symbol = "BTC-USD") { return m.m_flowProducts.at(symbol).heldBytes; }
  static size_t heldTrades(ServerDataModel &m, const std::string &symbol = "BTC-USD") { return m.m_flowProducts.at(symbol).heldTrades.size(); }
  static void expireHeld(ServerDataModel &m) {
    for (auto &held : m.m_flowProducts.at("BTC-USD").heldTrades) held.expires = {};
    m.publishFlow();
  }
  static void add(ServerDataModel &m, const Trade &trade) {
    m.addFlowTrade(trade, std::chrono::duration_cast<std::chrono::milliseconds>(trade.timestamp.time_since_epoch()).count());
  }
  static uint64_t totalDrops(ServerDataModel &m) { return m.m_flowDroppedTrades; }
  static uint64_t hourShrinkRefusals(ServerDataModel &m) { return m.m_flowHourShrinkRefusals; }
  static uint64_t lateDrops(ServerDataModel &m) { return m.m_flowLateTrades["BTC-USD"]; }
  static uint64_t sealMismatches(ServerDataModel &m) { return m.m_flowSealMismatches["BTC-USD"]; }
  static int64_t receiveMax(ServerDataModel &m) { return m.m_flowProducts.at("BTC-USD").receiveMax; }
  static bool sealed(ServerDataModel &m, int64_t hour) { return m.flowHourSealed("BTC-USD", hour); }
  static auto composed(ServerDataModel &m, const FlowSubscription &sub, int64_t start) {
    return m.composeLiveFlowBar(sub, start);
  }
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
  static void discardMinute(ServerDataModel& m, int64_t day, int64_t minute) {
    auto& series = m.m_flowProducts.at("BTC-USD").days.at(day).series.at(60'000);
    series.builder.dropBucket(minute);
    series.bucketSlots[(minute / 60'000) % series.bucketSlots.size()] = {};
  }
};

