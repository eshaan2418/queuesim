// Simulator-level properties on random L3 flows: model ordering, determinism, no look-ahead.
#include <gtest/gtest.h>

#include <random>

#include "harness.hpp"
#include "queuesim/strategies.hpp"

using namespace queuesim;
using namespace queuesim::test;

namespace {
// Non-reactive strategy: posts fixed passive orders on a schedule, never cancels. Fills cannot
// change its behaviour, so per-model fills are directly comparable.
struct Poster final : Strategy {
  std::uint64_t next = T0;
  std::uint64_t feed_latency;
  bool lookahead = false;
  explicit Poster(std::uint64_t fl) : feed_latency(fl) {}
  const char* name() const override { return "poster"; }
  void on_view(const View& v, Ctx& c) override {
    if (v.ts + feed_latency > c.now()) lookahead = true;
    if (!v.valid() || c.now() < next) return;
    c.place(Side::Buy, v.bid, 100);
    c.place(Side::Sell, v.ask, 100);
    next = c.now() + 50'000'000;  // every 50 ms
  }
};

// Random but consistent L3 flow around a drifting mid; returns the simulator's result.
SimResult run_flow(FillModel m, std::uint64_t seed, Strategy& s, std::uint64_t lat = 20'000) {
  SimConfig cfg;
  cfg.model = m;
  cfg.feed_latency_ns = lat;
  cfg.order_latency_ns = lat;
  Simulator sim(cfg, s, 1);
  Market k{sim};
  std::mt19937_64 rng(seed);
  std::vector<std::tuple<std::uint64_t, Side, Price, Qty>> live;
  Price mid = 1'000'000;
  std::uint64_t ref = 1;
  for (int i = 0; i < 60'000; ++i) {
    k.tick(1 + rng() % 200'000);
    if (rng() % 50 == 0) mid += (rng() % 3 == 0) ? 100 : (rng() % 2 ? -100 : 0);
    const int op = static_cast<int>(rng() % 10);
    if (op < 5 || live.size() < 20) {
      const Side sd = rng() % 2 ? Side::Buy : Side::Sell;
      const Price px = sd == Side::Buy ? mid - 100 * static_cast<Price>(1 + rng() % 5) : mid + 100 * static_cast<Price>(1 + rng() % 5);
      const Qty q = 100 * static_cast<Qty>(1 + rng() % 5);
      k.add(ref, sd, px, q);
      live.emplace_back(ref++, sd, px, q);
    } else {
      const std::size_t j = rng() % live.size();
      auto [r, sd, px, q] = live[j];
      if (op < 8) k.del(r);
      else k.exec(r, q);
      live[j] = live.back();
      live.pop_back();
    }
  }
  return sim.finish();
}
}  // namespace

TEST(SimProperty, FillModelOrderingTouchGeQueueGeThrough) {
  for (std::uint64_t seed = 1; seed <= 8; ++seed) {
    Poster a(20'000), b(20'000), c(20'000);
    const auto q = run_flow(FillModel::Queue, seed, a);
    const auto t = run_flow(FillModel::Touch, seed, b);
    const auto th = run_flow(FillModel::Through, seed, c);
    EXPECT_GE(t.maker_volume, q.maker_volume) << seed;
    EXPECT_GE(q.maker_volume, th.maker_volume) << seed;
    EXPECT_GT(q.maker_volume, 0) << seed;
    EXPECT_EQ(q.orders, t.orders);  // the strategy is non-reactive: identical order flow
    EXPECT_FALSE(a.lookahead || b.lookahead || c.lookahead);
  }
}

TEST(SimProperty, DeterministicReplay) {
  ASParams p;
  AvellanedaStoikov s1(p), s2(p);
  const auto r1 = run_flow(FillModel::Queue, 42, s1);
  const auto r2 = run_flow(FillModel::Queue, 42, s2);
  EXPECT_EQ(r1.hash, r2.hash);
  EXPECT_EQ(r1.fills, r2.fills);
  EXPECT_DOUBLE_EQ(r1.pnl, r2.pnl);
}

TEST(SimProperty, NoLookaheadAcrossLatencies) {
  for (std::uint64_t lat : {0ull, 1'000ull, 100'000ull, 5'000'000ull}) {
    Poster s(lat);
    const auto r = run_flow(FillModel::Queue, 7, s, lat);
    EXPECT_EQ(r.lookahead_violations, 0u) << lat;
    EXPECT_FALSE(s.lookahead) << lat;
  }
}

TEST(SimProperty, AccountingConservation) {
  // Cash and position are consistent with the sum of fills: buying then selling the same shares at
  // the same price yields zero PnL before fees.
  Poster s(20'000);
  const auto r = run_flow(FillModel::Queue, 3, s);
  EXPECT_NEAR(r.pnl, r.pnl_ex_fees - r.fees, 1e-6);
  EXPECT_LE(r.maker_fills + r.taker_fills, r.fills);
}
