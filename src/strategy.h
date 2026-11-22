// Automated strategies (Lua, see script.h): a StrategyRun drives one strategy against a paper broker, minute
// by minute, either live in the replay or inside a Backtest on its own thread. A strategy sees only bars that
// have closed, on its own timeframe, and its orders fill from the next minute on, exactly like manual orders.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "broker.h"
#include "market.h"
#include "script.h"

class StrategyRun {
public:
    // `agg` is the strategy timeframe's aggregation of `m` (nullptr for 1-minute strategies). Calls init and
    // on_day at minute `at` (the bar just revealed). Orders are refused until a minute closes at or after
    // `trade_from` (the warm-up). Returns false (with error()) if the script fails.
    bool start(lua_State* L, const ScriptDef& def, const Params& p, Broker* broker, const Market* m, const Agg* agg,
               size_t at, std::vector<std::string>* log, int64_t trade_from = INT64_MIN);
    bool trading() const { return m_->t[i_] + 1 >= trade_from_; }
    // After the broker has processed minute i: fills, then on_day / on_bar when a strategy bar closed.
    void after_minute(size_t i);
    void stop();
    bool running() const { return ref_ != -2 && error_.empty(); }
    const std::string& error() const { return error_; }
    const std::string& name() const { return name_; }

    // used by the Lua bindings
    int get_bar(lua_State* L);
    int count(lua_State* L);
    int now(lua_State* L);
    int position(lua_State* L);
    int last(lua_State* L);
    int submit(lua_State* L);
    int cancel(lua_State* L);
    int cancel_all(lua_State* L);
    int modify(lua_State* L);
    int orders(lua_State* L);
    int log(lua_State* L);
    int point_value(lua_State* L);
    int is_trading(lua_State* L);
    int open_next(lua_State* L);
    int bar_end(lua_State* L);

private:
    bool call(const char* fn, int nargs);  // def[fn](ctx, p, ...) with nargs extra values on the stack
    void fail(const std::string& e);
    lua_State* L_ = nullptr;
    int ref_ = -2, ctx_ref_ = -2, p_ref_ = -2;  // definition, ctx and params tables
    Broker* b_ = nullptr;
    const Market* m_ = nullptr;
    const Agg* agg_ = nullptr;
    size_t i_ = 0;                 // the minute just processed
    long done_ = -1;               // last completed strategy bar (bucket index, or minute for 1-minute)
    int64_t session_ = INT64_MIN;
    size_t fills_seen_ = 0;
    int64_t trade_from_ = INT64_MIN;
    int tf_ = 1;
    std::vector<std::string>* log_ = nullptr;
    std::string error_, name_;
};

struct BacktestStats {
    int trades = 0, wins = 0, days = 0, max_consec_losses = 0;
    double net = 0, gross_win = 0, gross_loss = 0, fees = 0, max_dd = 0, largest_win = 0, largest_loss = 0;
    double sharpe = 0, avg_minutes = 0, pct_days_up = 0;
};

struct BacktestResult {
    std::string error, summary;
    std::vector<RoundTrip> trips;
    std::vector<double> equity;    // cumulative $ after each trade
    std::vector<std::string> log;
    BacktestStats stats;
    int64_t from = 0, to = 0;      // New York minutes actually covered
    std::string strategy, params;
};

class Backtest {
public:
    ~Backtest() { cancel(); }
    // Runs `def` (by source, in a fresh Lua state) over [from, to) on a copy of the broker's settings.
    void start(const ScriptDef& def, const Params& p, const Market* m, int64_t from, int64_t to, const Broker& settings);
    void cancel();
    bool running() const { return running_; }
    double progress() const { return progress_; }
    int runs() const { return runs_; }
    BacktestResult result() const;
    void (*on_progress)() = nullptr;

private:
    void run(ScriptDef def, Params p, const Market* m, int64_t from, int64_t to, Broker b);
    std::thread th_;
    std::atomic<bool> running_{false}, cancel_{false};
    std::atomic<double> progress_{0};
    std::atomic<int> runs_{0};
    mutable std::mutex mu_;
    BacktestResult result_;
};

BacktestStats compute_stats(const std::vector<RoundTrip>& trips, const Market& m, size_t i0, size_t i1);
bool export_trips_csv(const fs::path& path, const std::vector<RoundTrip>& trips);
