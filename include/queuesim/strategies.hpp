// Example strategies. They see only View objects and their own fill reports (no look-ahead).
#pragma once

#include <cmath>
#include <cstdint>

#include "queuesim/sim.hpp"

namespace queuesim {

// Avellaneda & Stoikov (2008) market maker, in the common intraday form: a fixed short horizon
// tau instead of time-to-close, inventory measured in quote lots, volatility from an EWMA of
// 1-second mid changes.
//   reservation price r = mid - q * gamma * sigma^2 * tau
//   total spread      d = gamma * sigma^2 * tau + (2/gamma) * ln(1 + gamma/kappa)
// Quotes are rounded outward to the $0.01 tick and never cross the visible touch (post-only).
struct ASParams {
  double gamma = 0.1;        // risk aversion (1/$)
  double kappa = 150.0;      // order-arrival decay (1/$): higher = fills fall off faster with distance
  double tau_s = 30.0;       // horizon (seconds)
  Qty lot = 100;             // quote size (shares)
  int max_lots = 5;          // inventory limit (lots)
  double sigma_halflife_s = 60.0;
  std::uint64_t min_requote_ns = 100'000'000;  // throttle: at most 10 requotes/s
};

class AvellanedaStoikov final : public Strategy {
 public:
  explicit AvellanedaStoikov(ASParams p) : p_(p) {}
  const char* name() const override { return "avellaneda_stoikov"; }

  void on_view(const View& v, Ctx& c) override {
    if (!v.valid()) return;
    const double mid = v.mid() / 1e4;
    // EWMA variance of mid changes sampled once per second ($^2 per second).
    if (last_sample_ns_ == 0) {
      last_sample_ns_ = v.ts;
      last_mid_ = mid;
    } else if (v.ts >= last_sample_ns_ + 1'000'000'000ull) {
      const double dt = static_cast<double>(v.ts - last_sample_ns_) / 1e9;
      const double d = mid - last_mid_;
      const double a = 1.0 - std::exp(-dt * std::log(2.0) / p_.sigma_halflife_s);
      var_ = var_ == 0 ? d * d / dt : (1 - a) * var_ + a * d * d / dt;
      last_sample_ns_ = v.ts;
      last_mid_ = mid;
    }
    if (var_ == 0 || c.now() < last_quote_ns_ + p_.min_requote_ns) return;
    const double q = static_cast<double>(c.position()) / static_cast<double>(p_.lot);
    const double s2t = var_ * p_.tau_s;
    const double r = mid - q * p_.gamma * s2t;
    const double half = 0.5 * (p_.gamma * s2t + (2.0 / p_.gamma) * std::log(1.0 + p_.gamma / p_.kappa));
    // Round outward to cents, convert to ITCH ticks, keep passive.
    Price bid = static_cast<Price>(std::floor((r - half) * 100.0)) * 100;
    Price ask = static_cast<Price>(std::ceil((r + half) * 100.0)) * 100;
    if (bid >= v.ask) bid = v.ask - 100;
    if (ask <= v.bid) ask = v.bid + 100;
    const bool can_buy = q < p_.max_lots, can_sell = q > -p_.max_lots;
    requote(c, Side::Buy, can_buy ? bid : 0, bid_id_, bid_px_);
    requote(c, Side::Sell, can_sell ? ask : 0, ask_id_, ask_px_);
    last_quote_ns_ = c.now();
  }
  void on_fill(const FillReport& f, Ctx&) override {
    if (f.order_id == bid_id_ && f.qty >= 0) bid_open_ -= f.qty;
    if (f.order_id == ask_id_) ask_open_ -= f.qty;
  }

 private:
  void requote(Ctx& c, Side s, Price px, std::uint64_t& id, Price& cur) {
    const bool live = id != 0 && c.open_qty(id) > 0;
    if (live && px == cur) return;
    if (live) c.cancel(id);
    id = 0;
    cur = 0;
    if (px <= 0) return;
    id = c.place(s, px, p_.lot);
    cur = px;
  }
  ASParams p_;
  std::uint64_t last_sample_ns_ = 0, last_quote_ns_ = 0;
  double last_mid_ = 0, var_ = 0;
  std::uint64_t bid_id_ = 0, ask_id_ = 0;
  Price bid_px_ = 0, ask_px_ = 0;
  Qty bid_open_ = 0, ask_open_ = 0;
};

// Order-book imbalance taker: buy when the top-of-book imbalance (bidQ-askQ)/(bidQ+askQ) exceeds
// theta, sell when below -theta; exit after a fixed holding period. Crosses the spread, pays fees.
struct ImbParams {
  double theta = 0.8;
  Qty qty = 100;
  std::uint64_t hold_ns = 5'000'000'000;
  std::uint64_t cooldown_ns = 1'000'000'000;
};

class ImbalanceTaker final : public Strategy {
 public:
  explicit ImbalanceTaker(ImbParams p) : p_(p) {}
  const char* name() const override { return "imbalance_taker"; }
  void on_view(const View& v, Ctx& c) override {
    if (!v.valid()) return;
    const Qty pos = c.position();
    if (pos != 0) {
      if (c.now() >= entry_ns_ + p_.hold_ns && !exiting_) {
        c.market(pos > 0 ? Side::Sell : Side::Buy, pos > 0 ? pos : -pos);
        exiting_ = true;
        last_exit_ns_ = c.now();
      }
      return;
    }
    exiting_ = false;
    if (pending_ && c.now() > entry_ns_ + 1'000'000'000ull) pending_ = false;  // entry never filled
    if (pending_ || c.now() < last_exit_ns_ + p_.cooldown_ns) return;
    const double imb = static_cast<double>(v.bid_qty - v.ask_qty) / static_cast<double>(v.bid_qty + v.ask_qty);
    if (imb > p_.theta || imb < -p_.theta) {
      c.market(imb > 0 ? Side::Buy : Side::Sell, p_.qty);
      entry_ns_ = c.now();
      pending_ = true;
    }
  }
  void on_fill(const FillReport&, Ctx& c) override {
    if (c.position() != 0) pending_ = false;
    if (c.position() == 0) {
      pending_ = false;
      exiting_ = false;
    }
  }

 private:
  ImbParams p_;
  std::uint64_t entry_ns_ = 0, last_exit_ns_ = 0;
  bool pending_ = false, exiting_ = false;
};

}  // namespace queuesim
