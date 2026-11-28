#include "app.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>

#include "chart.h"
#include "civil.h"
#include "datafeed.h"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

const int kTfs[kNumTfs] = {1, 5, 15, 30, 60, 240, 1440};
const char* kTfNames[kNumTfs] = {"1m", "5m", "15m", "30m", "1h", "4h", "1D"};
const int kSpeeds[kNumSpeeds] = {1, 2, 5, 10, 30, 60, 120, 300, 600};
static const Rgba kPalette[] = {{1.0f, 0.8f, 0.2f, 1}, {0.55f, 0.65f, 1.0f, 1}, {0.9f, 0.5f, 0.9f, 1},
                                {0.4f, 0.9f, 0.9f, 1}, {1.0f, 0.6f, 0.3f, 1},  {0.7f, 0.9f, 0.4f, 1}};

App g;

void Ind::refresh() const {
    if (cached_gen == g_scripts.generation() && cached_for == params) return;
    cached_gen = g_scripts.generation(), cached_for = params;
    const ScriptDef* d = def();
    label_ = d ? script_label(g_scripts.L, *d, params) : script + " (missing)";
    warm_ = d ? script_warmup(g_scripts.L, *d, params) : 10;
}
const std::string& Ind::label() const { return refresh(), label_; }
int Ind::warmup() const { return refresh(), warm_; }

Ind make_ind(const std::string& script, int uid) {
    Ind d;
    d.script = script, d.uid = uid;
    d.col = kPalette[uid % 6];
    if (const ScriptDef* def = d.def()) d.params = default_params(*def);
    return d;
}

const Agg& App::agg() { return agg_for(tf()); }

const Agg& App::agg_for(int tf) {
    auto it = aggs.find(tf);
    if (it == aggs.end()) {
        it = aggs.emplace(tf, Agg{}).first;
        it->second.build(m, tf);
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
        if (g.auto_trade) {
            g.strat.after_minute(g.cursor);
            if (!g.strat.running()) g.auto_trade = false, g.status = "strategy stopped: " + g.strat.error();
        }
        ind_trading();
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
    if (g.auto_trade) start_strategy();  // a fresh start on the new day
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
    ++g.s_version;
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
std::vector<Band> default_bands() {
    auto band = [](bool on, const char* name, int start, int end, Rgba c) {
        Band b;
        b.on = on, b.start = start, b.end = end, b.col = c;
        std::snprintf(b.name, sizeof b.name, "%s", name);
        return b;
    };
    return {band(true, "Overnight", 16 * 60, 9 * 60 + 30, {0.35f, 0.45f, 0.85f, 0.07f}),
            band(true, "Open", 9 * 60 + 30, 10 * 60 + 30, {1.00f, 0.80f, 0.30f, 0.06f}),
            band(false, "Lunch", 11 * 60 + 30, 13 * 60 + 30, {0.60f, 0.60f, 0.60f, 0.06f}),
            band(true, "Close", 15 * 60, 16 * 60, {0.80f, 0.45f, 1.00f, 0.06f})};
}

double atr_pts() {
    int tf = kTfs[g.atr_tf_idx];
    if (tf >= 1440 * 7) return 0;
    std::vector<double> h, l, c;  // closed bars up to the cursor, oldest first
    size_t want = static_cast<size_t>(g.atr_len) * 5 + 1;
    if (tf == 1) {
        size_t i0 = g.cursor + 1 > want ? g.cursor + 1 - want : 0;
        for (size_t i = i0; i <= g.cursor; ++i) h.push_back(g.m.h[i]), l.push_back(g.m.l[i]), c.push_back(g.m.c[i]);
    } else {
        const Agg& a = g.agg_for(tf);
        size_t b = a.bucket_of(g.cursor);
        bool closed = b + 1 < a.first.size() ? static_cast<size_t>(a.first[b + 1]) == g.cursor + 1 : false;
        if (!closed) {
            if (b == 0) return 0;
            --b;
        }
        size_t b0 = b + 1 > want ? b + 1 - want : 0;
        for (size_t k = b0; k <= b; ++k) h.push_back(a.h[k]), l.push_back(a.l[k]), c.push_back(a.c[k]);
    }
    if (h.size() < static_cast<size_t>(g.atr_len) + 1) return 0;
    double atr = 0;  // Wilder's average of the true range, seeded with the first atr_len ranges
    for (size_t k = 1; k < h.size(); ++k) {
        double tr = std::max({h[k] - l[k], std::fabs(h[k] - c[k - 1]), std::fabs(l[k] - c[k - 1])});
        atr = k <= static_cast<size_t>(g.atr_len) ? atr + tr / g.atr_len : atr + (tr - atr) / g.atr_len;
    }
    return atr;
}

void bracket_now(double& stop, double& t1, double& t2) {
    stop = t1 = t2 = 0;
    if (!g.bracket) return;
    if (g.br_unit == 1) {
        double a = atr_pts();
        auto q = [](double v) { return std::max(0.0, std::round(v / kTick) * kTick); };
        stop = q(g.stop_atr * a), t1 = q(g.t1_atr * a), t2 = q(g.t2_atr * a);
    } else {
        stop = g.stop_pts, t1 = g.target_pts, t2 = g.target2_pts;
    }
}

int order_qty() {
    if (!g.risk_size) return g.qty;
    double stop, t1, t2;
    bracket_now(stop, t1, t2);
    if (stop <= 0) return g.qty;
    return std::max(1, static_cast<int>(std::floor(g.risk_usd / (stop * g.broker.mult) + 1e-9)));
}

namespace {
std::string fmt_price(double p) {
    char b[32];
    std::snprintf(b, sizeof b, "%.2f", p);
    return b;
}

void attach_bracket(Order& o) {
    double stop, t1, t2;
    bracket_now(stop, t1, t2);
    o.managed = true;
    o.stop_pts = stop, o.target_pts = t1, o.target2_pts = t2, o.t1_frac = g.t1_pct / 100.0;
}
}  // namespace

void market_order(int side, int qty) {
    if (qty <= 0) return;
    Order o;
    o.side = side, o.qty = qty, o.type = OrderType::Market, o.born = g.cursor;
    if (g.broker.pos == 0 || (g.broker.pos > 0) == (side > 0)) attach_bracket(o);
    g.broker.submit(o);
}

void pending_order(int side, OrderType t, double price, int qty) {
    Order o;
    o.side = side, o.type = t, o.qty = qty > 0 ? qty : order_qty(), o.price = round_tick(price), o.born = g.cursor;
    attach_bracket(o);
    g.broker.submit(o);
}

void add_to_position() {
    if (g.broker.pos) market_order(g.broker.pos > 0 ? 1 : -1, order_qty());
}

void scale_out(int qty) {
    Broker& b = g.broker;
    qty = std::min(qty, std::abs(b.pos));
    if (qty <= 0) return;
    Order o;
    o.side = b.pos > 0 ? -1 : 1, o.qty = qty, o.type = OrderType::Market, o.reduce_only = true, o.born = g.cursor;
    b.submit(o);
}

void set_stop(double price) {
    Broker& b = g.broker;
    if (!b.pos) return;
    if (Order* s = b.managed_stop()) {
        s->price = round_tick(price);
        return;
    }
    Order s;
    s.side = b.pos > 0 ? -1 : 1, s.type = OrderType::Stop, s.qty = std::abs(b.pos), s.price = round_tick(price);
    s.reduce_only = true, s.managed = true, s.born = g.cursor;
    b.submit(s);
}

void stop_to_breakeven() {
    const Broker& b = g.broker;
    if (!b.pos) return;
    // the average rounded to a tick on the safe side: a long's stop at or above it, a short's at or below
    double p = b.pos > 0 ? std::ceil(b.avg / kTick - 1e-9) * kTick : std::floor(b.avg / kTick + 1e-9) * kTick;
    set_stop(p);
}

void add_target(double price, int qty) {
    Broker& b = g.broker;
    if (!b.pos || qty <= 0) return;
    Order t;
    t.side = b.pos > 0 ? -1 : 1, t.type = OrderType::Limit, t.qty = qty, t.price = round_tick(price);
    t.reduce_only = true, t.managed = true, t.born = g.cursor;
    b.submit(t);
    b.sync_managed();
    b.purge();
}

void ind_trading() {
    static long key = -2;  // run_indicator caches the bars table by key: each run here gets a fresh one
    Broker& b = g.broker;
    for (Ind& d : g.inds) {
        if (!d.trade) continue;
        const ScriptDef* def = d.def();
        const Order* mine = nullptr;
        for (const Order& o : b.orders)
            if (o.id == d.order_id) mine = &o;
        if (!mine) d.order_id = 0;
        if (!d.on || !def || !def->trades) {
            if (mine) b.cancel(d.order_id), d.order_id = 0;
            continue;
        }
        if (b.pos != 0) continue;  // entries only from flat; a filled entry has left the order list
        const Agg& a = g.agg_for(d.trade_tf);
        int64_t cur = static_cast<int64_t>(a.bucket_of(g.cursor));
        int64_t lo = std::max<int64_t>(0, cur - std::max(600, 2 * d.warmup()));
        static Series s;
        build_series(g.m, a, g.cursor, lo, s);
        IndOutput out;
        run_indicator(*def, d.params, s, d.trade_tf, key--, out);
        if (!out.error.empty()) {
            b.log.push_back(d.label() + ": auto-trade stopped: " + out.error.substr(0, out.error.find('\n')));
            set_ind_trade(d, false);
            continue;
        }
        if (out.orders.empty()) {
            if (mine) b.cancel(d.order_id), d.order_id = 0;
            continue;
        }
        const IndOrder& w = out.orders.back();
        double stop, t1, t2;
        bracket_now(stop, t1, t2);  // the ticket's, unless the indicator gives its own
        auto q = [](double v) { return std::max(0.0, std::round(v / kTick) * kTick); };
        if (w.stop >= 0) stop = q(w.stop);
        if (w.target >= 0) t1 = q(w.target);
        if (w.target2 >= 0) t2 = q(w.target2);
        int qty = w.qty > 0 ? w.qty
                  : g.risk_size && stop > 0 ? std::max(1, static_cast<int>(std::floor(g.risk_usd / (stop * b.mult) + 1e-9)))
                                            : g.qty;
        Order o;
        o.side = w.side, o.type = w.kind, o.price = round_tick(w.price), o.qty = qty, o.born = g.cursor;
        o.managed = true, o.stop_pts = stop, o.target_pts = t1, o.target2_pts = t2, o.t1_frac = g.t1_pct / 100.0;
        if (mine && mine->side == o.side && mine->type == o.type) {
            for (Order& x : b.orders)
                if (x.id == d.order_id) {  // keep it working, at the new level and bracket
                    x.price = o.price, x.qty = o.qty;
                    x.stop_pts = o.stop_pts, x.target_pts = o.target_pts, x.target2_pts = o.target2_pts, x.t1_frac = o.t1_frac;
                }
            continue;
        }
        if (mine) b.cancel(d.order_id);
        d.order_id = b.submit(o);
        char buf[160];
        std::snprintf(buf, sizeof buf, "%s: %s %d %s%s, stop %.2f pt", d.label().c_str(), o.side > 0 ? "buy" : "sell", o.qty,
                      o.type == OrderType::Stop ? "stop @ " : o.type == OrderType::Limit ? "limit @ " : "market",
                      o.type == OrderType::Market ? "" : fmt_price(o.price).c_str(), stop);
        b.log.push_back(buf);
    }
}

void set_ind_trade(Ind& d, bool on) {
    d.trade = on;
    if (!on && d.order_id) g.broker.cancel(d.order_id), d.order_id = 0;
    if (on) ind_trading();
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

// ---------------------------------------------------------------- strategies
void select_strategy(const std::string& id) {
    if (g.strat_id == id && !g.strat_params.empty()) return;
    stop_strategy();
    g.strat_id = id;
    const ScriptDef* d = g_scripts.find(true, id);
    g.strat_params = d ? default_params(*d) : Params{};
}

bool start_strategy() {
    const ScriptDef* d = g_scripts.find(true, g.strat_id);
    if (!d) {
        g.auto_trade = false;
        g.status = "no strategy selected";
        return false;
    }
    if (g.strat_params.size() != d->params.size()) g.strat_params = default_params(*d);
    const Agg* a = d->timeframe > 1 ? &g.agg_for(d->timeframe) : nullptr;
    // warm-up: the strategy sees its history up to the cursor, then trades from the next minute
    size_t begin = d->warmup_days > 0 ? g.m.index_before(g.m.t[g.cursor] - static_cast<int64_t>(d->warmup_days) * 1440) : g.cursor;
    bool ok = g.strat.start(g_scripts.L, *d, g.strat_params, &g.broker, &g.m, a, begin, &g.broker.log, g.m.t[g.cursor] + 1);
    for (size_t k = begin + 1; ok && k <= g.cursor && g.strat.running(); ++k) g.strat.after_minute(k);
    if (!ok || !g.strat.running()) {
        g.auto_trade = false;
        g.status = "strategy error: " + g.strat.error();
        return false;
    }
    g.auto_trade = true;
    g.broker.log.push_back(d->name + " is trading the replay");
    return true;
}

void stop_strategy() {
    if (g.auto_trade) g.broker.log.push_back(g.strat.name() + " stopped");
    g.auto_trade = false;
    g.strat.stop();
}

void open_scripts_folder() {
    std::filesystem::path p = g_scripts.user_dir();
#ifdef _WIN32
    ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(__APPLE__)
    std::string cmd = "open \"" + p.string() + "\"";
    if (std::system(cmd.c_str()) != 0) g.status = "scripts folder: " + p.string();
#else
    std::string cmd = "xdg-open \"" + p.string() + "\" &";
    if (std::system(cmd.c_str()) != 0) g.status = "scripts folder: " + p.string();
#endif
}

// mods: 1 Shift; any other bit (Ctrl/Cmd, Alt) leaves the key to the system.
// ---------------------------------------------------------------- backtest trades
void goto_trade(int k) {
    const std::vector<RoundTrip>& tr = g.bt_result.trips;
    if (tr.empty() || g.m.size() == 0) return;
    k = std::clamp(k, 0, static_cast<int>(tr.size()) - 1);
    const RoundTrip& t = tr[k];
    size_t i_in = g.m.index_before(t.t_in + 1), i_out = g.m.index_before(t.t_out + 1);
    // reveal to the end of the exit's regular session (or 30 minutes past an exit outside it)
    int64_t day = t.t_out >= 0 ? t.t_out / 1440 : (t.t_out - 1439) / 1440, tod = t.t_out - day * 1440;
    size_t cur = tod < 16 * 60 ? g.m.index_before(day * 1440 + 16 * 60) : i_out;
    cur = std::min(std::max(cur, i_out + 30), g.m.size() - 1);
    char why[96];
    std::snprintf(why, sizeof why, "backtest trade %d of %zu", k + 1, tr.size());
    new_session(cur, why);
    g.bt_sel = k;
    const Agg& a = g.agg();
    chart_focus(static_cast<double>(a.bucket_of(i_in)), static_cast<double>(a.bucket_of(i_out)));
}

void step_trade(int dir) {
    const std::vector<RoundTrip>& tr = g.bt_result.trips;
    if (tr.empty()) {
        g.status = "run a backtest first";
        return;
    }
    int k = g.bt_sel;
    if (k < 0 || k >= static_cast<int>(tr.size())) {  // nothing selected: start from the replay position
        int64_t now = g.now();
        if (dir > 0)
            k = static_cast<int>(std::lower_bound(tr.begin(), tr.end(), now, [](const RoundTrip& r, int64_t v) { return r.t_in < v; }) - tr.begin());
        else
            k = static_cast<int>(std::lower_bound(tr.begin(), tr.end(), now, [](const RoundTrip& r, int64_t v) { return r.t_out < v; }) - tr.begin()) - 1;
    } else {
        k += dir;
    }
    goto_trade(k);
}

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
    else if (is("b") || is("B")) market_order(1, order_qty());
    else if (is("s") || is("S")) market_order(-1, order_qty());
    else if (is("a") || is("A")) add_to_position();
    else if (is("h") || is("H")) scale_out(std::max(1, std::abs(g.broker.pos) / 2));
    else if (is("e") || is("E")) stop_to_breakeven();
    else if (is("x") || is("X")) flatten();
    else if (is("c") || is("C")) g.broker.cancel_all();
    else if (is("l") || is("L")) g.follow = true;
    else if (is("bracketleft")) step_trade(-1);
    else if (is("bracketright")) step_trade(1);
    else return false;
    return true;
}
