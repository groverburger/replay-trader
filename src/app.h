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

struct Rgba {
    float r, g, b, a;
    Rgba alpha(float x) const { return {r, g, b, x}; }
};

const Rgba kUp{0.15f, 0.65f, 0.60f, 1.0f}, kDown{0.94f, 0.33f, 0.31f, 1.0f};
const Rgba kBuy{0.30f, 0.85f, 0.45f, 1.0f}, kSell{1.00f, 0.45f, 0.35f, 1.0f};

constexpr int kNumTfs = 7, kNumSpeeds = 9;
extern const int kTfs[kNumTfs];
extern const char* kTfNames[kNumTfs];
extern const int kSpeeds[kNumSpeeds];  // minutes revealed per second

// ---------------------------------------------------------------- indicators the user can add
enum IndType { SMA, EMA, BOLL, VWAP, PIVOTS, RSI, MACD, ATR, VOLUME, IND_COUNT };
extern const char* kIndNames[IND_COUNT];

struct Ind {
    int type, uid;
    int a = 20, b = 0, c = 0;  // periods: SMA/EMA a; BOLL a; PIVOTS left a, right b, levels c; RSI a;
    float k = 2.0f;            // MACD a,b,c = fast, slow, signal; ATR a; BOLL k = std devs
    Rgba col;
    bool on = true;
    std::string label() const;
    bool oscillator() const { return type == RSI || type == MACD || type == ATR || type == VOLUME; }
    int warmup() const { return type == MACD ? 4 * (b + c) : type == PIVOTS ? a + b + 1 : 4 * a + 10; }
};
Ind make_ind(int type, int uid);

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
    int visible = 120;          // chart bars in view when following
    double xmin = 0, xmax = 100;
    Broker broker;
    int qty = 1, instrument = 0;  // 0 NQ, 1 MNQ
    bool bracket = false;
    double stop_pts = 20, target_pts = 40;
    double ticket_price = 0;
    std::vector<Ind> inds;
    int next_uid = 0;
    Series s;
    size_t s_cursor = SIZE_MAX;
    int64_t s_lo = -1;
    int s_tf = -1;
    std::string status;
    std::mt19937 rng{std::random_device{}()};

    int tf() const { return kTfs[tf_idx]; }
    const Agg& agg();
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

// Trading.
void market_order(int side, int qty);
void pending_order(int side, OrderType t, double price);
void click_order(double price, bool limit);  // chart Ctrl/Cmd+click (limit) or Shift+click (stop)
void flatten();
void reverse();
void export_csv();

// Keyboard shortcuts shared by the window and the focused chart. Returns true when the key was used.
bool hotkey(const char* keysym, int mods);
