// Queue-position model: scripted L3 scenarios with exact expected fills under each model.
#include <gtest/gtest.h>

#include "harness.hpp"

using namespace queuesim;
using namespace queuesim::test;

namespace {
constexpr Price P = 1'000'000;  // $100.00
constexpr std::uint64_t US = 1000;

struct Fixture {
  SimConfig cfg;
  Scripted strat;
  std::unique_ptr<Simulator> sim;
  std::unique_ptr<Market> mkt;
  explicit Fixture(FillModel m, std::uint64_t lat = 10 * US) {
    cfg.model = m;
    cfg.feed_latency_ns = lat;
    cfg.order_latency_ns = lat;
    strat.feed_latency = lat;
    sim = std::make_unique<Simulator>(cfg, strat, 1);
    mkt = std::make_unique<Market>(Market{*sim});
  }
};

// Book: bid 100.00 = A(100) then B(200); ask 100.01. We join the bid with 100 shares.
SimResult scenario(FillModel m) {
  Fixture f(m);
  auto& k = *f.mkt;
  k.add(1, Side::Buy, P, 100);
  k.add(2, Side::Buy, P, 200);
  k.add(3, Side::Sell, P + 100, 500);
  f.strat.steps.push_back({T0, [](Ctx& c) { c.place(Side::Buy, P, 100); }});
  k.tick(100 * US);
  k.add(9, Side::Sell, P + 200, 1);  // any event: delivers the view, the order goes out
  k.tick(100 * US);                  // order has arrived (latency 10 us)
  k.add(4, Side::Buy, P, 300);       // C: arrives after us => behind
  k.tick(US);
  k.exec(1, 100);  // A (ahead) trades: ahead 300 -> 200
  k.tick(US);
  k.cancel(2, 150);  // B partially cancels: ahead 200 -> 50
  k.tick(US);
  k.exec(2, 50);  // B's rest trades: ahead 0
  k.tick(US);
  k.exec(4, 30);  // C (behind us) trades => we would have traded first: fill 30
  k.tick(US);
  return f.sim->finish();
}
}  // namespace

TEST(QueueModel, FillsOnlyAfterQueueAheadIsConsumed) {
  const SimResult r = scenario(FillModel::Queue);
  EXPECT_EQ(r.maker_volume, 30);
  EXPECT_EQ(r.lookahead_violations, 0u);
}

TEST(QueueModel, TouchFillsOnFirstPrintAtOurPrice) {
  const SimResult r = scenario(FillModel::Touch);
  EXPECT_EQ(r.maker_volume, 100);  // A's 100-share print at our price fills us entirely
}

TEST(QueueModel, ThroughNeedsATradeThroughOurPrice) {
  const SimResult r = scenario(FillModel::Through);
  EXPECT_EQ(r.maker_volume, 0);
}

TEST(QueueModel, TradeThroughAndCrossFill) {
  for (FillModel m : {FillModel::Queue, FillModel::Touch, FillModel::Through}) {
    Fixture f(m);
    auto& k = *f.mkt;
    k.add(1, Side::Buy, P, 1000);
    k.add(2, Side::Buy, P - 100, 1000);
    k.add(3, Side::Sell, P + 100, 500);
    f.strat.steps.push_back({T0, [](Ctx& c) { c.place(Side::Buy, P, 100); }});
    k.tick(100 * US);
    k.add(9, Side::Sell, P + 200, 1);
    k.tick(100 * US);
    k.del(1);       // the level we are on empties (we stay as the only virtual order)
    k.tick(US);
    k.exec(2, 40);  // a sell traded at 99.99, *below* our 100.00 bid: we would have been hit first
    k.tick(US);
    k.add(5, Side::Sell, P, 25);  // an ask at our bid price would have matched us
    k.tick(US);
    const SimResult r = f.sim->finish();
    EXPECT_EQ(r.maker_volume, 65) << int(m);
  }
}

TEST(QueueModel, OrderLatencyDelaysQueueEntry) {
  Fixture f(FillModel::Queue, 500 * US);
  auto& k = *f.mkt;
  k.add(1, Side::Buy, P, 100);
  k.add(3, Side::Sell, P + 100, 500);
  f.strat.steps.push_back({T0, [](Ctx& c) { c.place(Side::Buy, P, 100); }});
  k.tick(600 * US);
  k.add(9, Side::Sell, P + 200, 1);  // view delivered at ts + 500us; order sent then
  k.tick(100 * US);
  k.add(4, Side::Buy, P, 50);  // added before our order arrives => ahead of us, not behind
  k.tick(100 * US);
  k.exec(4, 50);  // must not fill us: it was ahead (our order has not arrived yet)
  k.tick(1000 * US);
  k.exec(1, 100);
  const SimResult r = f.sim->finish();
  EXPECT_EQ(r.maker_volume, 0);
  EXPECT_FALSE(f.strat.lookahead);
}

TEST(QueueModel, TakerWalksVisibleBookAtArrival) {
  Fixture f(FillModel::Queue);
  auto& k = *f.mkt;
  k.add(1, Side::Buy, P - 100, 100);
  k.add(2, Side::Sell, P, 100);
  k.add(3, Side::Sell, P + 100, 300);
  f.strat.steps.push_back({T0, [](Ctx& c) { c.market(Side::Buy, 250); }});
  k.tick(100 * US);
  k.add(9, Side::Buy, P - 200, 1);
  k.tick(100 * US);
  k.add(10, Side::Buy, P - 300, 1);
  const SimResult r = f.sim->finish();
  EXPECT_EQ(r.taker_fills, 2u);
  EXPECT_EQ(r.volume, 250);
  // 100 @ 100.00 + 150 @ 100.01, plus taker fees of $0.003/share.
  EXPECT_NEAR(r.cash, -(100 * 100.00 + 150 * 100.01), 1e-6);
  EXPECT_NEAR(r.fees, 250 * 0.0030, 1e-9);
}
