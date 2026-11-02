#pragma once
#include <functional>
#include <vector>

#include "queuesim/sim.hpp"

namespace queuesim::test {

constexpr std::uint64_t T0 = 10ull * 3600'000'000'000;  // 10:00, inside the default window

// Strategy driven by a script: at the first view at/after each scheduled time, run the action.
struct Scripted final : Strategy {
  struct Step {
    std::uint64_t at;
    std::function<void(Ctx&)> fn;
    bool done = false;
  };
  std::vector<Step> steps;
  std::vector<FillReport> fills;
  std::uint64_t feed_latency = 0;
  bool lookahead = false;
  const char* name() const override { return "scripted"; }
  void on_view(const View& v, Ctx& c) override {
    if (v.ts + feed_latency > c.now()) lookahead = true;
    for (auto& s : steps)
      if (!s.done && c.now() >= s.at) {
        s.fn(c);
        s.done = true;
      }
  }
  void on_fill(const FillReport& f, Ctx&) override { fills.push_back(f); }
};

// Small builder for L3 events.
struct Market {
  Simulator& sim;
  std::uint64_t ts = T0;
  void add(std::uint64_t ref, Side s, Price px, Qty q) {
    AddOrder a{};
    a.h.timestamp = ts;
    a.ref = ref;
    a.side = s;
    a.shares = static_cast<std::uint32_t>(q);
    a.price = static_cast<std::uint32_t>(px);
    sim.on_add(a);
  }
  void exec(std::uint64_t ref, Qty q) { sim.on_executed(ts, ref, q, 0); }
  void cancel(std::uint64_t ref, Qty q) { sim.on_remove(ts, ref, q); }
  void del(std::uint64_t ref) { sim.on_remove(ts, ref, -1); }
  void tick(std::uint64_t ns) { ts += ns; }
};

inline Qty filled(const SimResult& r) { return r.volume; }

}  // namespace queuesim::test
