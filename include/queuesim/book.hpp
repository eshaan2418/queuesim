// L3 book for one symbol, built from ITCH. Every order carries an arrival sequence number, which is
// what lets the simulator know whether a market order was ahead of or behind a virtual order in a
// price level's FIFO queue.
#pragma once

#include <cstdint>
#include <map>
#include <unordered_map>

#include "queuesim/messages.hpp"

namespace queuesim {

using Price = std::int64_t;  // ITCH 4-decimal ticks ($0.0001)
using Qty = std::int64_t;

// What changed in the book, as seen by the simulator's queue model.
struct MarketEvent {
  enum Kind : std::uint8_t { Add, Execute, Remove } kind;  // Remove = cancel/delete/replace-out
  Side side;
  Price price;      // resting order's price
  Qty shares;       // shares added / executed / removed
  std::uint64_t order_seq;  // arrival sequence of the affected resting order
  Price exec_price;  // execution price (C messages can differ from the resting price)
  std::uint64_t ts;
};

class L3Book {
 public:
  struct Order {
    Side side;
    Price price;
    Qty shares;
    std::uint64_t seq;
  };
  struct Level {
    Qty shares = 0;
    std::uint32_t orders = 0;
  };

  // Each mutator returns the event it produced (or false if it did not affect the book).
  bool add(std::uint64_t ref, Side s, Price px, Qty q, std::uint64_t ts, MarketEvent& ev) {
    const std::uint64_t seq = ++counter_;
    if (!orders_.emplace(ref, Order{s, px, q, seq}).second) return false;
    Level& l = side(s)[key(s, px)];
    l.shares += q;
    ++l.orders;
    ev = MarketEvent{MarketEvent::Add, s, px, q, seq, 0, ts};
    return true;
  }
  bool execute(std::uint64_t ref, Qty q, Price exec_px, std::uint64_t ts, MarketEvent& ev) {
    auto it = orders_.find(ref);
    if (it == orders_.end()) return false;
    Order& o = it->second;
    const Qty x = q < o.shares ? q : o.shares;
    ev = MarketEvent{MarketEvent::Execute, o.side, o.price, x, o.seq, exec_px ? exec_px : o.price, ts};
    reduce(it, x);
    return true;
  }
  bool remove(std::uint64_t ref, Qty q, std::uint64_t ts, MarketEvent& ev) {  // q < 0: all
    auto it = orders_.find(ref);
    if (it == orders_.end()) return false;
    Order& o = it->second;
    const Qty x = (q < 0 || q > o.shares) ? o.shares : q;
    ev = MarketEvent{MarketEvent::Remove, o.side, o.price, x, o.seq, 0, ts};
    reduce(it, x);
    return true;
  }
  bool lookup(std::uint64_t ref, Order& out) const {
    auto it = orders_.find(ref);
    if (it == orders_.end()) return false;
    out = it->second;
    return true;
  }

  [[nodiscard]] bool has_bid() const { return !bids_.empty(); }
  [[nodiscard]] bool has_ask() const { return !asks_.empty(); }
  [[nodiscard]] Price best_bid() const { return -bids_.begin()->first; }
  [[nodiscard]] Price best_ask() const { return asks_.begin()->first; }
  [[nodiscard]] Qty bid_qty() const { return bids_.begin()->second.shares; }
  [[nodiscard]] Qty ask_qty() const { return asks_.begin()->second.shares; }
  [[nodiscard]] Qty level_shares(Side s, Price px) const {
    const auto& m = s == Side::Buy ? bids_ : asks_;
    auto it = m.find(key(s, px));
    return it == m.end() ? 0 : it->second.shares;
  }
  // Walk the opposite side from the best price (taker fills); f(price, shares) -> continue?
  template <class F>
  void walk(Side taker_side, F&& f) const {
    if (taker_side == Side::Buy) {
      for (const auto& [k, l] : asks_)
        if (!f(k, l.shares)) return;
    } else {
      for (const auto& [k, l] : bids_)
        if (!f(-k, l.shares)) return;
    }
  }
  [[nodiscard]] std::uint64_t counter() const { return counter_; }
  [[nodiscard]] bool crossed() const { return has_bid() && has_ask() && best_bid() >= best_ask(); }

 private:
  // Both sides are ascending maps; bids are keyed by -price so begin() is the best bid.
  static Price key(Side s, Price px) { return s == Side::Buy ? -px : px; }
  std::map<Price, Level>& side(Side s) { return s == Side::Buy ? bids_ : asks_; }
  void reduce(std::unordered_map<std::uint64_t, Order>::iterator it, Qty x) {
    Order& o = it->second;
    auto& m = side(o.side);
    auto lv = m.find(key(o.side, o.price));
    lv->second.shares -= x;
    o.shares -= x;
    if (o.shares == 0) {
      if (--lv->second.orders == 0) m.erase(lv);
      orders_.erase(it);
    }
  }

  std::map<Price, Level> bids_, asks_;
  std::unordered_map<std::uint64_t, Order> orders_;
  std::uint64_t counter_ = 0;
};

}  // namespace queuesim
