// Event-driven L3 backtester with queue-position modelling.
//
// The real market (an ITCH L3 stream for one symbol) drives an exact replica of the book. A
// strategy sees the market only through a delayed view (feed latency) and its orders reach the
// simulated exchange only after the order latency. Strategy orders are *virtual*: they do not
// change the real book (no market impact; see docs). A resting virtual order joins the back of its
// price level's queue on arrival, and its fills are decided by the fill model:
//
//   Queue   (default, realistic): shares ahead = level size on arrival; they decrease as orders that
//           were ahead cancel or execute. An execution of an order that arrived *after* ours means
//           we would have been filled first (FIFO), so it fills us. Trade-throughs and incoming
//           orders that cross our price also fill us.
//   Touch   (naive): any execution at our price fills us, regardless of queue position.
//   Through (pessimistic): only executions strictly through our price fill us.
//
// No look-ahead by construction: the strategy only ever receives View objects stamped with a market
// time <= now - feed_latency, and fill reports after the same delay.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <queue>
#include <string>
#include <vector>

#include "queuesim/book.hpp"

namespace queuesim {

enum class FillModel : std::uint8_t { Queue, Touch, Through };

struct SimConfig {
  std::uint64_t feed_latency_ns = 50'000;   // market data -> strategy
  std::uint64_t order_latency_ns = 50'000;  // strategy -> exchange
  FillModel model = FillModel::Queue;
  double maker_fee = -0.0020;  // $ per share; negative = rebate (Nasdaq-like tiers)
  double taker_fee = 0.0030;
  std::uint64_t start_ns = 9ull * 3600'000'000'000 + 35ull * 60'000'000'000;  // 09:35
  std::uint64_t end_ns = 15ull * 3600'000'000'000 + 55ull * 60'000'000'000;   // 15:55
  std::vector<std::uint64_t> markout_ns{1'000'000'000, 5'000'000'000, 30'000'000'000};
  std::uint64_t equity_sample_ns = 60'000'000'000;  // 1-minute equity curve
};

// What the strategy sees: the market as of `ts` (<= now - feed latency).
struct View {
  std::uint64_t ts;   // market timestamp of this snapshot
  std::uint64_t now;  // simulation time at delivery
  Price bid = 0, ask = 0;
  Qty bid_qty = 0, ask_qty = 0;
  Price last_trade = 0;
  [[nodiscard]] bool valid() const { return bid > 0 && ask > 0 && bid < ask; }
  [[nodiscard]] double mid() const { return 0.5 * static_cast<double>(bid + ask); }
};

struct FillReport {
  std::uint64_t order_id;
  Side side;
  Price price;
  Qty qty;
  bool maker;
  std::uint64_t exch_ts;  // when it happened at the exchange
  double fee;
};

class Ctx {
 public:
  virtual ~Ctx() = default;
  virtual std::uint64_t now() const = 0;
  virtual Qty position() const = 0;  // as known to the strategy (fills reported so far)
  // Limit order: rests passively; if marketable on arrival it takes visible liquidity up to its
  // limit (taker) and rests any remainder.
  virtual std::uint64_t place(Side s, Price px, Qty q) = 0;
  virtual std::uint64_t market(Side s, Qty q) = 0;
  virtual void cancel(std::uint64_t id) = 0;
  virtual Qty open_qty(std::uint64_t id) const = 0;  // as known to the strategy
};

class Strategy {
 public:
  virtual ~Strategy() = default;
  virtual const char* name() const = 0;
  virtual void on_view(const View& v, Ctx& c) = 0;
  virtual void on_fill(const FillReport&, Ctx&) {}
};

struct MarkoutStats {
  double sum = 0;  // $ per share, signed so positive = favourable
  double n = 0;
  [[nodiscard]] double avg() const { return n ? sum / n : 0; }
};

struct SimResult {
  std::string strategy, model, symbol;
  std::uint64_t market_events = 0, views = 0;
  std::uint64_t orders = 0, cancels = 0, fills = 0, maker_fills = 0, taker_fills = 0;
  Qty volume = 0, maker_volume = 0, max_abs_position = 0, final_position = 0;
  double cash = 0, fees = 0, pnl = 0, pnl_ex_fees = 0;
  std::vector<MarkoutStats> markouts;     // per horizon, maker fills
  std::vector<MarkoutStats> markouts_taker;
  std::vector<double> equity;             // sampled equity (mark-to-mid, after fees)
  double sharpe_annualized = 0;
  std::uint64_t lookahead_violations = 0;  // must be 0 (checked on every view)
  std::uint64_t hash = 14695981039346656037ull;  // determinism fingerprint over fills
};

class Simulator final : public Ctx {
 public:
  Simulator(const SimConfig& cfg, Strategy& s, std::uint16_t locate) : cfg_(cfg), strat_(s), locate_(locate) {
    res_.strategy = s.name();
    res_.model = cfg.model == FillModel::Queue ? "queue" : cfg.model == FillModel::Touch ? "touch" : "through";
    res_.markouts.resize(cfg.markout_ns.size());
    res_.markouts_taker.resize(cfg.markout_ns.size());
    next_equity_ = cfg.start_ns;
  }

  // ---- ITCH message handlers (only messages for our locate are passed in)
  // Every handler first runs pending work (deliveries, order arrivals) due up to the event's
  // timestamp, *then* applies the event: an order arriving at t must see the book as of t.
  void on_add(const AddOrder& m) {
    advance(m.h.timestamp);
    MarketEvent ev;
    if (!book_.add(m.ref, m.side, m.price, m.shares, m.h.timestamp, ev)) return;
    cross_fill(ev);
    after_market(m.h.timestamp, false);
  }
  void on_executed(std::uint64_t ts, std::uint64_t ref, Qty q, Price exec_px) {
    advance(ts);
    MarketEvent ev;
    if (!book_.execute(ref, q, exec_px, ts, ev)) return;
    last_trade_ = ev.exec_price;
    queue_on_execute(ev);
    after_market(ts, true);
  }
  void on_remove(std::uint64_t ts, std::uint64_t ref, Qty q) {
    advance(ts);
    MarketEvent ev;
    if (!book_.remove(ref, q, ts, ev)) return;
    queue_on_remove(ev);
    after_market(ts, false);
  }
  void on_replace(std::uint64_t ts, std::uint64_t old_ref, std::uint64_t new_ref, Qty q, Price px) {
    L3Book::Order o;
    if (!book_.lookup(old_ref, o)) return;
    on_remove(ts, old_ref, -1);
    AddOrder a{};
    a.h.timestamp = ts;
    a.ref = new_ref;
    a.side = o.side;
    a.shares = static_cast<std::uint32_t>(q);
    a.price = static_cast<std::uint32_t>(px);
    on_add(a);
  }
  // Finish: run pending work up to the end of the window and mark to market.
  SimResult finish() {
    advance(cfg_.end_ns + cfg_.markout_ns.back() + 1);
    const double m = mid();
    res_.final_position = pos_;
    res_.pnl = cash_ + static_cast<double>(pos_) * m / 1e4 - res_.fees;
    res_.pnl_ex_fees = cash_ + static_cast<double>(pos_) * m / 1e4;
    res_.cash = cash_;
    // Sharpe on 1-minute equity increments, annualised with 390 minutes x 252 days. One day of
    // one symbol is far too little data for this to mean much: reported with that caveat.
    std::vector<double> r;
    for (std::size_t i = 1; i < res_.equity.size(); ++i) r.push_back(res_.equity[i] - res_.equity[i - 1]);
    if (r.size() > 2) {
      double mu = 0, var = 0;
      for (double x : r) mu += x;
      mu /= static_cast<double>(r.size());
      for (double x : r) var += (x - mu) * (x - mu);
      var /= static_cast<double>(r.size() - 1);
      if (var > 0) res_.sharpe_annualized = mu / std::sqrt(var) * std::sqrt(390.0 * 252.0);
    }
    return res_;
  }
  const L3Book& book() const { return book_; }

  // ---- Ctx
  std::uint64_t now() const override { return now_; }
  Qty position() const override { return known_pos_; }
  std::uint64_t place(Side s, Price px, Qty q) override { return submit(s, px, q, false); }
  std::uint64_t market(Side s, Qty q) override { return submit(s, 0, q, true); }
  void cancel(std::uint64_t id) override {
    ++res_.cancels;
    push(now_ + cfg_.order_latency_ns, Pending::Cancel, id);
  }
  Qty open_qty(std::uint64_t id) const override {
    for (const auto& k : known_open_)
      if (k.first == id) return k.second;
    return 0;
  }

 private:
  struct VOrder {
    std::uint64_t id;
    Side side;
    Price price;  // 0 for market
    Qty qty;      // remaining
    Qty ahead = 0;
    std::uint64_t join_seq = 0;
    bool market = false;
  };
  struct Pending {
    enum Kind : std::uint8_t { ViewDelivery, Arrival, Cancel, Report, Markout } kind;
    std::uint64_t at;
    std::uint64_t seq;  // FIFO tie-break
    std::uint64_t ref;  // index into the payload vectors
  };
  struct PendingCmp {
    bool operator()(const Pending& a, const Pending& b) const { return a.at != b.at ? a.at > b.at : a.seq > b.seq; }
  };
  struct MarkoutReq {
    Side side;
    Price price;
    std::size_t horizon;
    bool maker;
  };

  void push(std::uint64_t at, Pending::Kind k, std::uint64_t ref) { pq_.push(Pending{k, at, ++pseq_, ref}); }

  std::uint64_t submit(Side s, Price px, Qty q, bool mkt) {
    const std::uint64_t id = ++next_id_;
    ++res_.orders;
    staged_.push_back(VOrder{id, s, px, q, 0, 0, mkt});
    push(now_ + cfg_.order_latency_ns, Pending::Arrival, staged_.size() - 1);
    known_open_.push_back({id, q});
    return id;
  }

  double mid() const {
    if (!book_.has_bid() || !book_.has_ask()) return static_cast<double>(last_trade_);
    return 0.5 * static_cast<double>(book_.best_bid() + book_.best_ask());
  }

  // Run every pending item due at or before t (in time order), then set now_ = t.
  void advance(std::uint64_t t) {
    while (!pq_.empty() && pq_.top().at <= t) {
      const Pending p = pq_.top();
      pq_.pop();
      now_ = std::max(now_, p.at);
      sample_equity(now_);
      switch (p.kind) {
        case Pending::ViewDelivery: deliver_view(views_[p.ref]); break;
        case Pending::Arrival: arrive(staged_[p.ref]); break;
        case Pending::Cancel: do_cancel(p.ref); break;
        case Pending::Report: deliver_report(reports_[p.ref]); break;
        case Pending::Markout: resolve_markout(p.ref); break;
      }
    }
    now_ = std::max(now_, t);
    sample_equity(now_);
  }

  void sample_equity(std::uint64_t t) {
    while (t >= next_equity_ && next_equity_ <= cfg_.end_ns) {
      res_.equity.push_back(cash_ + static_cast<double>(pos_) * mid() / 1e4 - res_.fees);
      next_equity_ += cfg_.equity_sample_ns;
    }
  }

  void after_market(std::uint64_t ts, bool trade) {
    ++res_.market_events;
    if (!book_.has_bid() || !book_.has_ask()) return;
    View v{ts, 0, book_.best_bid(), book_.best_ask(), book_.bid_qty(), book_.ask_qty(), last_trade_};
    if (!trade && v.bid == last_view_.bid && v.ask == last_view_.ask && v.bid_qty == last_view_.bid_qty &&
        v.ask_qty == last_view_.ask_qty)
      return;
    last_view_ = v;
    if (ts + cfg_.feed_latency_ns < cfg_.start_ns || ts > cfg_.end_ns) return;
    views_.push_back(v);
    push(ts + cfg_.feed_latency_ns, Pending::ViewDelivery, views_.size() - 1);
  }

  void deliver_view(View v) {
    v.now = now_;
    // No look-ahead: what the strategy sees must be at least feed_latency old.
    if (v.ts + cfg_.feed_latency_ns > now_) ++res_.lookahead_violations;
    ++res_.views;
    if (now_ >= cfg_.start_ns && now_ <= cfg_.end_ns) strat_.on_view(v, *this);
  }

  void deliver_report(const FillReport& r) {
    known_pos_ += r.side == Side::Buy ? r.qty : -r.qty;
    for (auto it = known_open_.begin(); it != known_open_.end(); ++it)
      if (it->first == r.order_id) {
        it->second -= r.qty;
        if (it->second <= 0) known_open_.erase(it);
        break;
      }
    strat_.on_fill(r, *this);
  }

  void arrive(VOrder o) {
    // Marketable part trades as taker against visible liquidity (no impact on the real book).
    Qty taken = 0;
    book_.walk(o.side, [&](Price px, Qty avail) {
      if (o.qty <= 0) return false;
      if (!o.market && (o.side == Side::Buy ? px > o.price : px < o.price)) return false;
      const Qty q = std::min(o.qty, avail);
      fill(o, px, q, false);
      o.qty -= q;
      taken += q;
      return true;
    });
    if (o.qty <= 0) return;
    if (o.market) {  // unfilled market remainder is cancelled
      drop_known(o.id);
      return;
    }
    o.ahead = book_.level_shares(o.side, o.price);
    o.join_seq = book_.counter();  // every resting order with seq <= this is ahead of us
    live_.push_back(o);
  }

  void drop_known(std::uint64_t id) {
    for (auto it = known_open_.begin(); it != known_open_.end(); ++it)
      if (it->first == id) {
        known_open_.erase(it);
        return;
      }
  }

  void do_cancel(std::uint64_t id) {
    for (auto it = live_.begin(); it != live_.end(); ++it)
      if (it->id == id) {
        live_.erase(it);
        break;
      }
    drop_known(id);  // strategy learns the cancel after the round trip; simplified as immediate here
  }

  void fill(const VOrder& o, Price px, Qty q, bool maker) {
    if (q <= 0) return;
    const double dpx = static_cast<double>(px) / 1e4;
    const double sgn = o.side == Side::Buy ? 1.0 : -1.0;
    cash_ -= sgn * dpx * static_cast<double>(q);  // dollars
    pos_ += o.side == Side::Buy ? q : -q;
    res_.max_abs_position = std::max(res_.max_abs_position, pos_ < 0 ? -pos_ : pos_);
    const double fee = (maker ? cfg_.maker_fee : cfg_.taker_fee) * static_cast<double>(q);
    res_.fees += fee;
    ++res_.fills;
    res_.volume += q;
    if (maker) {
      ++res_.maker_fills;
      res_.maker_volume += q;
    } else {
      ++res_.taker_fills;
    }
    reports_.push_back(FillReport{o.id, o.side, px, q, maker, now_, fee});
    push(now_ + cfg_.feed_latency_ns, Pending::Report, reports_.size() - 1);
    for (std::size_t h = 0; h < cfg_.markout_ns.size(); ++h) {
      markreq_.push_back(MarkoutReq{o.side, px, h, maker});
      markqty_.push_back(q);
      push(now_ + cfg_.markout_ns[h], Pending::Markout, markreq_.size() - 1);
    }
    std::uint64_t x = (o.id * 1315423911ull) ^ static_cast<std::uint64_t>(px) ^ (static_cast<std::uint64_t>(q) << 32) ^ now_;
    res_.hash = (res_.hash ^ x) * 1099511628211ull;
  }

  void resolve_markout(std::uint64_t i) {
    const MarkoutReq& m = markreq_[i];
    const double q = static_cast<double>(markqty_[i]);
    const double edge = (m.side == Side::Buy ? mid() - static_cast<double>(m.price) : static_cast<double>(m.price) - mid()) / 1e4;
    auto& st = m.maker ? res_.markouts[m.horizon] : res_.markouts_taker[m.horizon];
    st.sum += edge * q;
    st.n += q;
  }

  // ---- queue model
  static bool more_aggressive(Side s, Price ours, Price theirs) { return s == Side::Buy ? ours > theirs : ours < theirs; }

  void queue_on_execute(const MarketEvent& ev) {
    for (std::size_t i = 0; i < live_.size();) {
      VOrder& v = live_[i];
      Qty q = 0;
      if (v.side == ev.side) {
        if (more_aggressive(v.side, v.price, ev.price)) {
          q = ev.shares;  // traded through our price: we were better and would have filled first
        } else if (v.price == ev.price) {
          switch (cfg_.model) {
            case FillModel::Queue:
              if (ev.order_seq <= v.join_seq) {
                // An order ahead of us traded: our position improves.
                const Qty used = std::min(v.ahead, ev.shares);
                v.ahead -= used;
                q = ev.shares - used;  // any excess beyond the queue ahead reaches us
              } else {
                v.ahead = 0;  // an order behind us traded => everything ahead is gone, and so are we
                q = ev.shares;
              }
              break;
            case FillModel::Touch: q = ev.shares; break;
            case FillModel::Through: q = 0; break;
          }
        }
      }
      q = std::min(q, v.qty);
      if (q > 0) {
        fill(v, v.price, q, true);
        v.qty -= q;
      }
      if (v.qty <= 0) live_.erase(live_.begin() + static_cast<std::ptrdiff_t>(i));
      else ++i;
    }
  }

  void queue_on_remove(const MarketEvent& ev) {
    for (auto& v : live_)
      if (v.side == ev.side && v.price == ev.price && ev.order_seq <= v.join_seq)
        v.ahead = std::max<Qty>(0, v.ahead - ev.shares);
  }

  // An incoming order on the other side priced through our resting price would have traded
  // with us in reality (the real book shows it resting only because we did not exist).
  void cross_fill(const MarketEvent& ev) {
    for (std::size_t i = 0; i < live_.size();) {
      VOrder& v = live_[i];
      if (v.side != ev.side && (v.side == Side::Buy ? ev.price <= v.price : ev.price >= v.price)) {
        const Qty q = std::min(v.qty, ev.shares);
        fill(v, v.price, q, true);
        v.qty -= q;
      }
      if (v.qty <= 0) live_.erase(live_.begin() + static_cast<std::ptrdiff_t>(i));
      else ++i;
    }
  }

  SimConfig cfg_;
  Strategy& strat_;
  [[maybe_unused]] std::uint16_t locate_;
  L3Book book_;
  std::priority_queue<Pending, std::vector<Pending>, PendingCmp> pq_;
  std::uint64_t pseq_ = 0, next_id_ = 0, now_ = 0, next_equity_ = 0;
  std::vector<View> views_;
  std::vector<VOrder> staged_, live_;
  std::vector<FillReport> reports_;
  std::vector<MarkoutReq> markreq_;
  std::vector<Qty> markqty_;
  std::vector<std::pair<std::uint64_t, Qty>> known_open_;
  View last_view_{};
  Price last_trade_ = 0;
  Qty pos_ = 0, known_pos_ = 0;
  double cash_ = 0;
  SimResult res_;
};

}  // namespace queuesim
