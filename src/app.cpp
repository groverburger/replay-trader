#include "app.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>

#include "civil.h"
#include "datafeed.h"

const int kTfs[kNumTfs] = {1, 5, 15, 30, 60, 240, 1440};
const char* kTfNames[kNumTfs] = {"1m", "5m", "15m", "30m", "1h", "4h", "1D"};
const int kSpeeds[kNumSpeeds] = {1, 2, 5, 10, 30, 60, 120, 300, 600};
const char* kIndNames[IND_COUNT] = {"SMA", "EMA", "Bollinger bands", "VWAP (session)", "Swing pivots", "RSI", "MACD",
                                    "ATR", "Volume"};
static const Rgba kPalette[] = {{1.0f, 0.8f, 0.2f, 1}, {0.55f, 0.65f, 1.0f, 1}, {0.9f, 0.5f, 0.9f, 1},
                                {0.4f, 0.9f, 0.9f, 1}, {1.0f, 0.6f, 0.3f, 1},  {0.7f, 0.9f, 0.4f, 1}};

App g;

std::string Ind::label() const {
    char buf[64];
    switch (type) {
        case SMA: std::snprintf(buf, sizeof buf, "SMA %d", a); break;
        case EMA: std::snprintf(buf, sizeof buf, "EMA %d", a); break;
        case BOLL: std::snprintf(buf, sizeof buf, "BB %d, %.1f", a, k); break;
        case PIVOTS: std::snprintf(buf, sizeof buf, "Pivots %d/%d", a, b); break;
        case RSI: std::snprintf(buf, sizeof buf, "RSI %d", a); break;
        case MACD: std::snprintf(buf, sizeof buf, "MACD %d/%d/%d", a, b, c); break;
        case ATR: std::snprintf(buf, sizeof buf, "ATR %d", a); break;
        default: std::snprintf(buf, sizeof buf, "%s", kIndNames[type]);
    }
    return buf;
}

Ind make_ind(int type, int uid) {
    Ind d{type, uid};
    d.col = kPalette[uid % 6];
    switch (type) {
        case EMA: d.a = 21; break;
        case PIVOTS: d.a = 5, d.b = 5, d.c = 1; break;
        case RSI: d.a = 14; break;
        case MACD: d.a = 12, d.b = 26, d.c = 9; break;
        case ATR: d.a = 14; break;
        default: break;
    }
    return d;
}

const Agg& App::agg() {
    auto it = aggs.find(tf());
    if (it == aggs.end()) {
        it = aggs.emplace(tf(), Agg{}).first;
        it->second.build(m, tf());
    }
    return it->second;
}

std::string fmt_time(int64_t minutes, const char* f) {  // times are naive New York local time
    int64_t day = floor_div(minutes, 1440), tod = minutes - day * 1440;
    int y, m, d;
    civil_from_days(day, y, m, d);
    std::tm tm{};
    tm.tm_year = y - 1900, tm.tm_mon = m - 1, tm.tm_mday = d;
    tm.tm_hour = static_cast<int>(tod / 60), tm.tm_min = static_cast<int>(tod % 60);
    tm.tm_wday = weekday_of(day);
    tm.tm_yday = static_cast<int>(day - days_from_civil(y, 1, 1));
    char buf[40];
    std::strftime(buf, sizeof buf, f, &tm);
    return buf;
}

double round_tick(double p) { return std::round(p / kTick) * kTick; }

bool in_rth(int64_t t) {
    int64_t tod = ((t % 1440) + 1440) % 1440;
    return tod >= 570 && tod < 960;
}

// ---------------------------------------------------------------- replay
void advance(size_t n) {
    for (size_t k = 0; k < n && g.cursor + 1 < g.m.size(); ++k) {
        ++g.cursor;
        float d = g.m.adj[g.cursor] - g.m.adj[g.cursor - 1];
        if (d != 0) g.broker.roll(d, g.m.t[g.cursor]);
        g.broker.on_bar(g.m, g.cursor);
    }
    if (g.cursor + 1 >= g.m.size()) g.playing = false;
}

void finish_chart_bar() {
    const Agg& a = g.agg();
    size_t b = a.bucket_of(g.cursor);
    size_t guard = 0;
    while (g.cursor + 1 < g.m.size() && a.bucket_of(g.cursor + 1) == b && ++guard < 20000) advance(1);
    if (guard == 0) advance(1);  // already complete: reveal the next bar's first minute
}

void skip_to_open() {  // reveal through 09:29 so the next step shows the opening minute
    auto it = std::upper_bound(g.opens.begin(), g.opens.end(), g.cursor + 1);
    if (it == g.opens.end()) return;
    advance(*it - 1 - g.cursor);
}

void new_session(size_t cursor, const char* why) {
    g.broker.reset();
    g.cursor = cursor;
    g.playing = false;
    g.follow = true;
    g.s_cursor = SIZE_MAX;
    g.ticket_price = 0;
    g.status = why;
}

void random_day() {
    std::uniform_int_distribution<size_t> d(0, g.opens.size() - 1);
    size_t i = g.opens[d(g.rng)];
    new_session(i >= 5 ? i - 5 : i, "new session on a random day (09:25)");
}

bool start_at(const char* date) {
    int64_t t;
    if (!parse_minutes(date, t)) {
        g.status = "date format: YYYY-MM-DD HH:MM";
        return false;
    }
    new_session(g.m.index_before(t), "new session");
    return true;
}

void tick(double dt) {
    if (!g.playing) {
        g.accum = 0;
        return;
    }
    g.accum += dt * kSpeeds[g.speed_idx];
    size_t n = static_cast<size_t>(g.accum);
    g.accum -= static_cast<double>(n);
    for (size_t k = 0; k < n && g.playing; ++k) {
        if (g.rth_only && g.cursor + 1 < g.m.size() && !in_rth(g.m.t[g.cursor + 1])) skip_to_open();
        advance(1);
    }
}

// ---------------------------------------------------------------- chart data
void refresh_series() {
    int warm = 300;
    for (const Ind& d : g.inds)
        if (d.on) warm = std::max(warm, d.warmup());
    const Agg& a = g.agg();
    int64_t cur_bucket = static_cast<int64_t>(a.bucket_of(g.cursor));
    int64_t view_lo = g.follow ? cur_bucket - static_cast<int64_t>(std::ceil(g.visible)) : static_cast<int64_t>(std::floor(g.xmin));
    int64_t lo = std::max<int64_t>(0, std::min(view_lo, cur_bucket) - warm);
    lo = std::max(lo, cur_bucket - 60000);  // bound the work when zoomed far out
    lo -= lo % 64;                          // fewer rebuilds while panning
    if (g.s_cursor == g.cursor && g.s_lo == lo && g.s_tf == g.tf()) return;
    build_series(g.m, a, g.cursor, lo, g.s);
    g.s_cursor = g.cursor, g.s_lo = lo, g.s_tf = g.tf();
}

void update_view() {
    refresh_series();
    if (g.follow) {
        double last_x = static_cast<double>(g.s.x0 + g.s.n() - 1);
        g.xmax = last_x + std::max(3.0, g.visible * 0.12);
        g.xmin = g.xmax - g.visible;
        refresh_series();
    }
}

void visible_range(long& j0, long& j1) {
    j0 = std::max(0L, static_cast<long>(std::floor(g.xmin)) - static_cast<long>(g.s.x0));
    j1 = std::min(static_cast<long>(g.s.n()) - 1, static_cast<long>(std::ceil(g.xmax)) - static_cast<long>(g.s.x0));
}

// ---------------------------------------------------------------- trading
void market_order(int side, int qty) {
    if (qty <= 0) return;
    Order o;
    o.side = side, o.qty = qty, o.type = OrderType::Market, o.born = g.cursor;
    if (g.bracket && (g.broker.pos == 0 || (g.broker.pos > 0) == (side > 0)))
        o.stop_pts = g.stop_pts, o.target_pts = g.target_pts;
    g.broker.submit(o);
}

void pending_order(int side, OrderType t, double price) {
    Order o;
    o.side = side, o.type = t, o.qty = g.qty, o.price = round_tick(price), o.born = g.cursor;
    o.stop_pts = g.bracket ? g.stop_pts : 0, o.target_pts = g.bracket ? g.target_pts : 0;
    g.broker.submit(o);
}

void click_order(double price, bool limit) {
    double p = round_tick(price);
    if (limit) pending_order(p < g.last() ? 1 : -1, OrderType::Limit, p);
    else pending_order(p > g.last() ? 1 : -1, OrderType::Stop, p);
}

void flatten() {
    g.broker.cancel_all();
    if (g.broker.pos) market_order(g.broker.pos > 0 ? -1 : 1, std::abs(g.broker.pos));
}

void reverse() {
    if (g.broker.pos) market_order(g.broker.pos > 0 ? -1 : 1, 2 * std::abs(g.broker.pos));
}

void export_csv() {
    std::filesystem::path dir = data_dir() / "sessions";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::time_t now = std::time(nullptr);
    char name[64];
    std::strftime(name, sizeof name, "session_%Y%m%d_%H%M%S.csv", std::localtime(&now));
    std::string path = (dir / name).string();
    FILE* f = open_file(dir / name, "w");
    if (!f) {
        g.status = "could not write " + path;
        return;
    }
    std::fprintf(f, "entry_time,exit_time,side,qty,entry,exit,pnl_usd\n");
    for (const RoundTrip& r : g.broker.trips)
        std::fprintf(f, "%s,%s,%s,%d,%.2f,%.2f,%.2f\n", fmt_time(r.t_in, "%Y-%m-%d %H:%M").c_str(),
                     fmt_time(r.t_out, "%Y-%m-%d %H:%M").c_str(), r.side > 0 ? "long" : "short", r.qty, r.p_in, r.p_out,
                     r.pnl);
    std::fclose(f);
    g.status = "saved " + path;
}

// mods: 1 Shift; any other bit (Ctrl/Cmd, Alt) leaves the key to the system.
bool hotkey(const char* k, int mods) {
    if (mods & ~1) return false;
    bool shift = mods & 1;
    auto is = [&](const char* name) { return std::strcmp(k, name) == 0; };
    if (is("Right")) {
        shift ? finish_chart_bar() : advance(1);
        return true;
    }
    if (shift) return false;
    if (is("space")) g.playing = !g.playing;
    else if (is("n") || is("N")) skip_to_open();
    else if (is("Up")) g.speed_idx = std::min(g.speed_idx + 1, kNumSpeeds - 1);
    else if (is("Down")) g.speed_idx = std::max(g.speed_idx - 1, 0);
    else if (is("b") || is("B")) market_order(1, g.qty);
    else if (is("s") || is("S")) market_order(-1, g.qty);
    else if (is("x") || is("X")) flatten();
    else if (is("c") || is("C")) g.broker.cancel_all();
    else if (is("l") || is("L")) g.follow = true;
    else return false;
    return true;
}
