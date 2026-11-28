// Replay state shared by the chart and the panels: the market, the replay cursor, the paper broker and the
// indicators the user added. UI-toolkit free.
#pragma once

#include <cstdint>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "broker.h"
#include "market.h"
#include "rgba.h"
#include "script.h"
#include "strategy.h"


const Rgba kUp{0.15f, 0.65f, 0.60f, 1.0f}, kDown{0.94f, 0.33f, 0.31f, 1.0f};
const Rgba kBuy{0.30f, 0.85f, 0.45f, 1.0f}, kSell{1.00f, 0.45f, 0.35f, 1.0f};

constexpr int kNumTfs = 7, kNumSpeeds = 9;
extern const int kTfs[kNumTfs];
extern const char* kTfNames[kNumTfs];
extern const int kSpeeds[kNumSpeeds];  // minutes revealed per second

// ---------------------------------------------------------------- indicators the user added (Lua scripts)
struct Ind {
    std::string script;        // ScriptDef id (file name without .lua)
    int uid = 0;
    Params params;
    Rgba col{1, 1, 1, 1};      // the indicator's colour (plots without their own)
    bool on = true;
    // auto-trading (scripts with trades = true): the replay keeps the indicator's out:order working, computed on
    // trade_tf bars (chosen next to the Auto-trade box; 5m to start), while flat
    bool trade = false;
    int trade_tf = 5;
    int order_id = 0;           // its working entry order in g.broker (0: none)
    const ScriptDef* def() const { return g_scripts.find(false, script); }
    // label and warm-up bars, recomputed when the params or the scripts change
    const std::string& label() const;
    int warmup() const;

    mutable Params cached_for;
    mutable int cached_gen = -1;
    mutable std::string label_;
    mutable int warm_ = 300;
    void refresh() const;
};
Ind make_ind(const std::string& script, int uid);

// ---------------------------------------------------------------- session colours
// A time-of-day band shaded behind intraday bars: [start, end) in New York minutes since midnight (wraps past
// midnight when end < start).
struct Band {
    bool on = true;
    char name[24] = "";
    int start = 0, end = 0;
    Rgba col{1, 1, 1, 0.06f};
};
std::vector<Band> default_bands();

// ---------------------------------------------------------------- app state
struct App {
    Market m;
    std::map<int, Agg> aggs;
    std::vector<size_t> opens;  // 1-minute bars starting 09:30
    size_t cursor = 0;          // last revealed 1-minute bar
    int tf_idx = 0;
    bool playing = false, rth_only = false, hide_dates = false, follow = true, sessions = true;
    int speed_idx = 3;
    double accum = 0;
    double visible = 120;       // chart bars in view when following (fractional while a zoom animates)
    double xmin = 0, xmax = 100;
    Broker broker;
    int qty = 1, instrument = 0;  // 0 NQ, 1 MNQ
    bool bracket = false;
    // Bracket distances: points, or multiples of ATR(atr_len) on the atr_tf_idx timeframe. Target 2 (0 = off)
    // takes the part of each entry that target 1 doesn't (t1_pct %).
    int br_unit = 0;            // 0 points, 1 ATR
    double stop_pts = 20, target_pts = 40, target2_pts = 0;
    double stop_atr = 1.0, t1_atr = 1.5, t2_atr = 0;
    int t1_pct = 50;
    int atr_len = 14, atr_tf_idx = 1;
    bool risk_size = false;     // size each entry from risk_usd and the stop distance
    double risk_usd = 500;
    std::vector<Band> bands = default_bands();
    bool bands_on = true;
    double ticket_price = 0;
    std::vector<Ind> inds;
    int next_uid = 0;
    Series s;
    long s_version = 0;         // bumped whenever s is rebuilt (indicator caches key on it)
    size_t s_cursor = SIZE_MAX;
    int64_t s_lo = -1;
    int s_tf = -1;
    std::string status;
    // strategy: auto-trading the replay, and backtests
    std::string strat_id;
    Params strat_params;
    bool auto_trade = false;
    StrategyRun strat;
    Backtest bt;
    BacktestResult bt_result;
    int bt_seen = 0;            // bt.runs() whose result is in bt_result
    bool bt_qc_rolls = false;   // backtest on QuantConnect's roll schedule
    bool bt_fee_by_year = false;  // backtests: commission from bt_fees instead of the ticket's flat fee
    std::vector<std::pair<int, double>> bt_fees;  // {year, $ per side from that year on}
    bool bt_by_year = false;      // show the backtest's results by year
    bool bt_show = true;        // draw the backtest's trades on the chart
    int bt_sel = -1;            // the backtest trade last jumped to (-1: none)
    std::mt19937 rng{std::random_device{}()};

    int tf() const { return kTfs[tf_idx]; }
    const Agg& agg();
    const Agg& agg_for(int tf);
    double last() const { return m.c[cursor]; }
    int64_t now() const { return m.t[cursor] + 1; }  // the revealed bar has closed
};

extern App g;

std::string fmt_time(int64_t minutes, const char* f);
double round_tick(double p);
bool in_rth(int64_t t);

// Replay.
void advance(size_t n);
void finish_chart_bar();
void skip_to_open();
void new_session(size_t cursor, const char* why);
void random_day();
bool start_at(const char* date);  // "YYYY-MM-DD HH:MM"
void tick(double dt);             // playback

// Chart data: rebuild the series for the cursor and view, and move the view when following.
void refresh_series();
void update_view();
void visible_range(long& j0, long& j1);

// Trading. Manual entries carry the ticket's bracket as a position-level bracket (Order::managed).
double atr_pts();                                   // ATR(atr_len) on the ATR timeframe, closed bars, points (0: n/a)
void bracket_now(double& stop, double& t1, double& t2);  // the ticket's distances in points now (0 = off)
int order_qty();                                    // the ticket's quantity, or sized from the risk and stop
void market_order(int side, int qty);
void pending_order(int side, OrderType t, double price, int qty = 0);  // qty 0: order_qty()
void flatten();
void add_to_position();                             // market order of order_qty() in the position's direction
void scale_out(int qty);                            // reduce-only market order
void stop_to_breakeven();
void set_stop(double price);                        // the position's stop (created if there is none)
void add_target(double price, int qty);             // a reduce-only limit for qty of the position
// Indicators with Auto-trade ticked: recompute each on its timeframe at the cursor and place, move or cancel
// its entry order. Runs after every revealed minute.
void ind_trading();
void set_ind_trade(Ind& d, bool on);
void reverse();
void export_csv();

// Strategy auto-trading in the replay (on the replay's broker, from the current bar).
bool start_strategy();
void stop_strategy();
void select_strategy(const std::string& id);
void open_scripts_folder();

// Backtest trades: jump the replay to trade k (0-based) of g.bt_result: a new session revealed past the exit, with
// the view framing the trade. step_trade(+1 / -1) goes to the next / previous one (from the replay position when
// none is selected).
void goto_trade(int k);
void step_trade(int dir);

// Keyboard shortcuts shared by the window and the focused chart. Returns true when the key was used.
bool hotkey(const char* keysym, int mods);
