// queuesim CLI: run a strategy over one symbol of an ITCH file under one or all fill models in a
// single pass (each model gets its own simulator and strategy instance).
//
//   queuesim --itch FILE --symbol AAPL --strategy as|imb --model all|queue|touch|through
//            [--feed-us 50] [--order-us 50] [--gamma ..] [--kappa ..] [--tau ..] [--lot ..]
//            [--theta ..] [--hold-s ..] [--out result.json]
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "queuesim/gz_reader.hpp"
#include "queuesim/parser.hpp"
#include "queuesim/sim.hpp"
#include "queuesim/strategies.hpp"

using namespace queuesim;

namespace {
const char* arg(int argc, char** argv, const char* k, const char* d) {
  for (int i = 1; i + 1 < argc; ++i)
    if (std::strcmp(argv[i], k) == 0) return argv[i + 1];
  return d;
}

struct Run {
  std::unique_ptr<Strategy> strat;
  std::unique_ptr<Simulator> sim;
};

void json_result(std::FILE* f, const SimResult& r, const SimConfig& c, bool last) {
  std::fprintf(f, "    {\"model\": \"%s\", \"strategy\": \"%s\", \"symbol\": \"%s\", \"market_events\": %llu, \"views\": %llu,\n",
               r.model.c_str(), r.strategy.c_str(), r.symbol.c_str(), (unsigned long long)r.market_events, (unsigned long long)r.views);
  std::fprintf(f, "     \"orders\": %llu, \"cancels\": %llu, \"fills\": %llu, \"maker_fills\": %llu, \"taker_fills\": %llu, \"fill_rate\": %.4f,\n",
               (unsigned long long)r.orders, (unsigned long long)r.cancels, (unsigned long long)r.fills, (unsigned long long)r.maker_fills,
               (unsigned long long)r.taker_fills, r.orders ? static_cast<double>(r.fills) / static_cast<double>(r.orders) : 0.0);
  std::fprintf(f, "     \"volume\": %lld, \"maker_volume\": %lld, \"max_abs_position\": %lld, \"final_position\": %lld,\n",
               (long long)r.volume, (long long)r.maker_volume, (long long)r.max_abs_position, (long long)r.final_position);
  std::fprintf(f, "     \"pnl\": %.2f, \"pnl_ex_fees\": %.2f, \"fees\": %.2f, \"pnl_per_share\": %.5f, \"sharpe_annualized_1min\": %.2f,\n",
               r.pnl, r.pnl_ex_fees, r.fees, r.volume ? r.pnl / static_cast<double>(r.volume) : 0.0, r.sharpe_annualized);
  std::fprintf(f, "     \"markout_maker_per_share\": {");
  for (std::size_t h = 0; h < c.markout_ns.size(); ++h)
    std::fprintf(f, "%s\"%llus\": %.5f", h ? ", " : "", (unsigned long long)(c.markout_ns[h] / 1'000'000'000), r.markouts[h].avg());
  std::fprintf(f, "}, \"markout_taker_per_share\": {");
  for (std::size_t h = 0; h < c.markout_ns.size(); ++h)
    std::fprintf(f, "%s\"%llus\": %.5f", h ? ", " : "", (unsigned long long)(c.markout_ns[h] / 1'000'000'000), r.markouts_taker[h].avg());
  std::fprintf(f, "},\n     \"lookahead_violations\": %llu, \"fill_hash\": \"%016llx\", \"equity_minutes\": %zu}%s\n",
               (unsigned long long)r.lookahead_violations, (unsigned long long)r.hash, r.equity.size(), last ? "" : ",");
}
}  // namespace

int main(int argc, char** argv) {
  const std::string path = arg(argc, argv, "--itch", "");
  std::string sym = arg(argc, argv, "--symbol", "AAPL");
  const std::string strat = arg(argc, argv, "--strategy", "as");
  const std::string model = arg(argc, argv, "--model", "all");
  const char* out = arg(argc, argv, "--out", nullptr);
  SimConfig cfg;
  cfg.feed_latency_ns = static_cast<std::uint64_t>(std::atof(arg(argc, argv, "--feed-us", "50")) * 1000);
  cfg.order_latency_ns = static_cast<std::uint64_t>(std::atof(arg(argc, argv, "--order-us", "50")) * 1000);
  cfg.maker_fee = std::atof(arg(argc, argv, "--maker-fee", "-0.0020"));
  cfg.taker_fee = std::atof(arg(argc, argv, "--taker-fee", "0.0030"));
  ASParams ap;
  ap.gamma = std::atof(arg(argc, argv, "--gamma", "0.1"));
  ap.kappa = std::atof(arg(argc, argv, "--kappa", "150"));
  ap.tau_s = std::atof(arg(argc, argv, "--tau", "30"));
  ap.lot = std::atoll(arg(argc, argv, "--lot", "100"));
  ap.max_lots = std::atoi(arg(argc, argv, "--max-lots", "5"));
  ImbParams ip;
  ip.theta = std::atof(arg(argc, argv, "--theta", "0.8"));
  ip.hold_ns = static_cast<std::uint64_t>(std::atof(arg(argc, argv, "--hold-s", "5")) * 1e9);
  ip.qty = std::atoll(arg(argc, argv, "--qty", "100"));

  std::vector<FillModel> models;
  if (model == "all") models = {FillModel::Queue, FillModel::Touch, FillModel::Through};
  else models = {model == "touch" ? FillModel::Touch : model == "through" ? FillModel::Through : FillModel::Queue};
  sym.resize(8, ' ');

  GzReader reader(path);
  if (!reader.ok()) {
    std::fprintf(stderr, "cannot open %s\n", path.c_str());
    return 1;
  }
  FrameChunker chunker(reader, 64u << 20);
  int locate = -1;
  std::vector<Run> runs;
  const auto t0 = std::chrono::steady_clock::now();
  std::uint64_t msgs = 0, sym_msgs = 0;
  const std::uint8_t* data;
  std::size_t size;
  while (chunker.fill(data, size)) {
    for_each_frame(data, size, [&](const std::uint8_t* m, std::uint16_t len) {
      ++msgs;
      if (len < 11) return;
      if (locate < 0) {
        if (m[0] == 'R' && len == kStockDirectory && std::memcmp(m + 11, sym.data(), 8) == 0) {
          locate = be::u16(m + 1);
          for (FillModel fm : models) {
            SimConfig c = cfg;
            c.model = fm;
            Run r;
            if (strat == "imb") r.strat = std::make_unique<ImbalanceTaker>(ip);
            else r.strat = std::make_unique<AvellanedaStoikov>(ap);
            r.sim = std::make_unique<Simulator>(c, *r.strat, static_cast<std::uint16_t>(locate));
            runs.push_back(std::move(r));
          }
        }
        return;
      }
      if (be::u16(m + 1) != locate) return;
      ++sym_msgs;
      const std::uint64_t ts = be::u48(m + 5);
      switch (m[0]) {
        case 'A': case 'F': {
          const AddOrder a = decode_add_order(m);
          for (auto& r : runs) r.sim->on_add(a);
          break;
        }
        case 'E': {
          const auto e = decode_order_executed(m);
          for (auto& r : runs) r.sim->on_executed(ts, e.ref, e.shares, 0);
          break;
        }
        case 'C': {
          const auto e = decode_order_executed_price(m);
          for (auto& r : runs) r.sim->on_executed(ts, e.ref, e.shares, e.price);
          break;
        }
        case 'X': {
          const auto x = decode_order_cancel(m);
          for (auto& r : runs) r.sim->on_remove(ts, x.ref, x.shares);
          break;
        }
        case 'D': {
          const auto d = decode_order_delete(m);
          for (auto& r : runs) r.sim->on_remove(ts, d.ref, -1);
          break;
        }
        case 'U': {
          const auto u = decode_order_replace(m);
          for (auto& r : runs) r.sim->on_replace(ts, u.old_ref, u.new_ref, u.shares, u.price);
          break;
        }
        default: break;
      }
    });
  }
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  if (locate < 0) {
    std::fprintf(stderr, "symbol not found\n");
    return 1;
  }
  std::FILE* f = out ? std::fopen(out, "w") : stdout;
  std::fprintf(f, "{\n  \"itch_messages\": %llu, \"symbol_messages\": %llu, \"seconds\": %.2f, \"feed_latency_us\": %.1f, \"order_latency_us\": %.1f,\n",
               (unsigned long long)msgs, (unsigned long long)sym_msgs, secs, cfg.feed_latency_ns / 1e3, cfg.order_latency_ns / 1e3);
  std::fprintf(f, "  \"params\": {\"gamma\": %g, \"kappa\": %g, \"tau_s\": %g, \"lot\": %lld, \"max_lots\": %d, \"theta\": %g, \"hold_s\": %g, \"maker_fee\": %g, \"taker_fee\": %g},\n",
               ap.gamma, ap.kappa, ap.tau_s, (long long)ap.lot, ap.max_lots, ip.theta, ip.hold_ns / 1e9, cfg.maker_fee, cfg.taker_fee);
  std::fprintf(f, "  \"results\": [\n");
  for (std::size_t i = 0; i < runs.size(); ++i) {
    SimResult r = runs[i].sim->finish();
    r.symbol = arg(argc, argv, "--symbol", "AAPL");
    json_result(f, r, cfg, i + 1 == runs.size());
  }
  std::fprintf(f, "  ]\n}\n");
  if (out) std::fclose(f);
  return 0;
}
