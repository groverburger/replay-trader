// The left-hand column: Replay, Order ticket, Position & orders, Indicators and Trades panels, built once as a
// BUI widget tree and kept in step with the app state by panels_sync(). Lists that change shape (working orders,
// indicators, trades, log) are rebuilt when their contents change.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "BUI_kit.h"
#include "app.h"
#include "chart.h"
#include "civil.h"
#include "datafeed.h"
#include "ui.h"

namespace {

// ---------------------------------------------------------------- widget helpers
std::unordered_map<BKNode*, std::function<void(int)>> handlers;  // node -> callback(event kind)

BKNode* add(BKNode* parent, const char* type) {
    BKNode* n = BK_node_new(type);
    BK_node_add(parent, n, -1);
    return n;
}

void set_icon(BKNode* n, const char* icon) {
    if (icon && BK_icon_id(icon)) BK_set_str(n, "icon", icon);
}

BKNode* label(BKNode* parent, const std::string& text) {
    BKNode* n = add(parent, "label");
    BK_set_str(n, "text", text.c_str());
    return n;
}

BKNode* row(BKNode* parent) { return add(parent, "row"); }

BKNode* button(BKNode* parent, const char* text, const char* icon, std::function<void()> fn) {
    BKNode* n = add(parent, "button");
    BK_set_str(n, "text", text);
    set_icon(n, icon);
    handlers[n] = [fn](int kind) {
        if (kind == BK_EV_CLICK) fn();
    };
    return n;
}

BKNode* check(BKNode* parent, const char* text, bool on, std::function<void(bool)> fn) {
    BKNode* n = add(parent, "checkbox");
    BK_set_str(n, "label", text);
    BK_set_num(n, "value", on);
    handlers[n] = [n, fn](int kind) {
        if (kind == BK_EV_CHANGE) fn(BK_get_num(n, "value") != 0);
    };
    return n;
}

BKNode* toggle(BKNode* parent, const char* text, bool on, std::function<void(bool)> fn) {
    BKNode* n = add(parent, "toggle");
    BK_set_str(n, "label", text);
    BK_set_num(n, "value", on);
    handlers[n] = [n, fn](int kind) {
        if (kind == BK_EV_CHANGE) fn(BK_get_num(n, "value") != 0);
    };
    return n;
}

BKNode* number(BKNode* parent, const char* text, double v, double lo, double hi, double step, int precision,
               std::function<void(double)> fn, const char* suffix = nullptr) {
    BKNode* n = add(parent, "number");
    BK_set_str(n, "label", text);
    if (precision == 0) BK_set_num(n, "integer", 1);
    BK_set_num(n, "min", lo);
    BK_set_num(n, "max", hi);
    BK_set_num(n, "step", step);
    BK_set_num(n, "precision", precision);
    BK_set_num(n, "value", v);
    if (suffix) BK_set_str(n, "suffix", suffix);
    handlers[n] = [n, fn](int kind) {
        if (kind == BK_EV_CHANGE) fn(BK_get_num(n, "value"));
    };
    return n;
}

BKNode* choice(BKNode* parent, const char* type, const char* const* items, int count, int v, std::function<void(int)> fn) {
    BKNode* n = add(parent, type);
    BK_set_strs(n, "items", items, count);
    BK_set_num(n, "value", v);
    handlers[n] = [n, fn](int kind) {
        if (kind == BK_EV_CHANGE) fn(static_cast<int>(BK_get_num(n, "value")));
    };
    return n;
}

// A collapsible panel; returns its body.
BKNode* panel(BKNode* parent, const char* title) {
    BKNode* p = add(parent, "panel");
    BK_set_str(p, "text", title);
    return BK_node_child(p, 0);
}

void forget(BKNode* n) {
    handlers.erase(n);
    for (int i = 0; i < BK_node_child_count(n); ++i) forget(BK_node_child(n, i));
}

void clear(BKNode* parent) {
    while (BK_node_child_count(parent)) {
        BKNode* c = BK_node_child(parent, 0);
        forget(c);
        BK_node_remove(parent, c);
        BK_node_free(c);
    }
}

// Setters that only touch a widget (and so only redraw it) when the value really changed.
void set_text(BKNode* n, const std::string& s, const char* key = "text") {
    if (s != BK_get_str(n, key)) BK_set_str(n, key, s.c_str());
}

std::unordered_map<BKNode*, Rgba> text_colors;
void set_color(BKNode* n, Rgba c) {
    auto it = text_colors.find(n);
    if (it != text_colors.end() && std::memcmp(&it->second, &c, sizeof c) == 0) return;
    text_colors[n] = c;
    float v[4] = {c.r, c.g, c.b, c.a};
    BK_set_color(n, "text_color", v);
}

void set_value(BKNode* n, double v) {
    if (BK_get_num(n, "editing") == 0 && BK_get_num(n, "value") != v) BK_set_num(n, "value", v);
}

void set_enabled(BKNode* n, bool on) {
    if ((BK_get_num(n, "enabled") != 0) != on) BK_set_num(n, "enabled", on);
}

Rgba dim() {
    float c[4];
    if (!BK_theme_color("text", c)) return {0.6f, 0.6f, 0.6f, 1};
    return {c[0], c[1], c[2], 0.5f};
}

BKNode* dim_label(BKNode* parent, const std::string& text) {
    BKNode* n = label(parent, text);
    set_color(n, dim());
    return n;
}

std::string money(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%+.0f", v);
    return buf;
}

// ---------------------------------------------------------------- the widgets panels_sync() updates
struct {
    BKNode *scroll, *col;
    // replay
    BKNode *clock, *session, *play, *speed, *date, *tf, *visible, *follow, *rth, *hide, *lines;
    // ticket
    BKNode *inst, *bracket_row, *price;
    // position
    BKNode *pos, *open, *closed, *total, *fees, *orders;
    // indicators
    BKNode *inds, *add_row;
    int inds_gen = -1;
    // strategy
    BKNode *strat_row, *strat_choice = nullptr, *strat_desc, *strat_params, *strat_auto, *strat_status, *bt_from, *bt_to, *bt_run, *bt_stop,
        *bt_bar, *bt_summary, *bt_export;
    int strat_gen = -1, bt_seen = -1;
    std::string strat_sig;
    // trades
    BKNode *stats, *trips, *log;
    int unit = 0;
    bool play_icon = false;
    std::string orders_sig, inds_sig = "-", trips_sig, log_sig;
} W;

int g_add_index = 0;  // in the Add dropdown (indicator scripts by name)

// Number / checkbox / dropdown widgets for a script's params, two to a row; `get` finds the live values
// (they may move or vanish while the widgets exist).
void param_widgets(BKNode* parent, const ScriptDef& d, std::function<Params*()> get) {
    BKNode* r = nullptr;
    int in_row = 0;
    for (size_t k = 0; k < d.params.size(); ++k) {
        const ParamDef& q = d.params[k];
        Params* p = get();
        double v = p && k < p->size() ? (*p)[k] : q.def;
        if (q.kind == ParamDef::Bool) {
            check(parent, q.label.c_str(), v != 0, [get, k](bool on) {
                if (Params* p = get(); p && k < p->size()) (*p)[k] = on;
            });
            continue;
        }
        if (!r || in_row == 2) r = row(parent), in_row = 0;
        ++in_row;
        if (q.kind == ParamDef::Choice) {
            static std::map<const ScriptDef*, std::vector<std::vector<const char*>>> items;  // stable c-strings
            auto& per = items[&d];
            per.resize(d.params.size());
            per[k].clear();
            for (const std::string& o : q.options) per[k].push_back(o.c_str());
            BKNode* c = choice(r, "dropdown", per[k].data(), static_cast<int>(per[k].size()), static_cast<int>(v), [get, k](int i) {
                if (Params* p = get(); p && k < p->size()) (*p)[k] = i;
            });
            BK_set_str(c, "label", q.label.c_str());
        } else {
            number(r, q.label.c_str(), v, q.min, q.max, q.step, q.precision, [get, k](double x) {
                if (Params* p = get(); p && k < p->size()) (*p)[k] = x;
            });
        }
    }
}

void build_replay(BKNode* body) {
    BKNode* r = row(body);
    W.clock = label(r, "");
    W.session = label(r, "");
    BK_set_num(W.session, "weight", 0);

    r = row(body);
    W.play = button(r, "Play", "PLAY", [] { g.playing = !g.playing; });
    button(r, "+1 min", "FRAME_NEXT", [] { advance(1); });
    button(r, "Finish bar", "NEXT_KEYFRAME", [] { finish_chart_bar(); });
    r = row(body);
    button(r, "To 09:30 open", "FF", [] { skip_to_open(); });
    button(r, "Random day", "FILE_REFRESH", [] { random_day(); });

    static std::vector<std::string> speeds;
    static std::vector<const char*> speed_items;
    for (int s : kSpeeds) speeds.push_back(std::to_string(s) + " min/s");
    for (auto& s : speeds) speed_items.push_back(s.c_str());
    W.speed = choice(body, "dropdown", speed_items.data(), kNumSpeeds, g.speed_idx, [](int v) { g.speed_idx = v; });
    BK_set_str(W.speed, "label", "Speed");

    W.rth = check(body, "Skip overnight", g.rth_only, [](bool v) { g.rth_only = v; });
    W.hide = check(body, "Hide dates", g.hide_dates, [](bool v) { g.hide_dates = v; });
    W.lines = check(body, "09:30 / 16:00 lines", g.sessions, [](bool v) { g.sessions = v; });

    r = row(body);
    W.date = add(r, "text");
    BK_set_str(W.date, "value", "2024-08-05 09:25");
    BK_set_str(W.date, "placeholder", "YYYY-MM-DD HH:MM");
    handlers[W.date] = [](int kind) {
        if (kind == BK_EV_SUBMIT) start_at(BK_get_str(W.date, "value"));
    };
    BKNode* b = button(r, "Start here", nullptr, [] { start_at(BK_get_str(W.date, "value")); });
    BK_set_num(b, "weight", 0.6);

    W.tf = choice(body, "enum", kTfNames, kNumTfs, g.tf_idx, [](int v) {
        g.tf_idx = v;
        g.follow = true;
    });
    r = row(body);
    W.visible = number(r, "Bars in view", g.visible, 10, 20000, 1, 0, [](double v) {
        g.visible = v;
        if (!g.follow) g.xmin = g.xmax - g.visible;
    });
    W.follow = toggle(r, "Follow (L)", g.follow, [](bool v) { g.follow = v; });
    BK_set_num(W.follow, "weight", 0.7);
}

void build_ticket(BKNode* body) {
    Broker& b = g.broker;
    BKNode* r = row(body);
    static const char* insts[] = {"NQ  $20/pt", "MNQ  $2/pt"};
    W.inst = choice(r, "dropdown", insts, 2, g.instrument, [](int v) {
        g.instrument = v;
        g.broker.mult = v ? 2.0 : 20.0;
        g.broker.commission = v ? 0.62 : 2.15;
    });
    number(r, "Qty", g.qty, 1, 100, 1, 0, [](double v) { g.qty = static_cast<int>(v); });
    r = row(body);
    number(r, "Slippage", b.slip_ticks, 0, 20, 1, 0, [](double v) { g.broker.slip_ticks = static_cast<int>(v); },
           " ticks");
    number(r, "Fee", b.commission, 0, 20, 0.01, 2, [](double v) { g.broker.commission = v; }, " $/side");
    check(body, "Limit fills on touch", b.limit_touch, [](bool v) { g.broker.limit_touch = v; });
    check(body, "Bracket (stop + target on fill)", g.bracket, [](bool v) { g.bracket = v; });
    W.bracket_row = row(body);
    number(W.bracket_row, "Stop", g.stop_pts, 0, 1000, 0.25, 2, [](double v) { g.stop_pts = v; }, " pt");
    number(W.bracket_row, "Target", g.target_pts, 0, 1000, 0.25, 2, [](double v) { g.target_pts = v; }, " pt");

    r = row(body);
    BK_set_num(r, "height", 1.4);
    button(r, "Buy Mkt (B)", "TRIA_UP", [] { market_order(1, g.qty); });
    button(r, "Sell Mkt (S)", "TRIA_DOWN", [] { market_order(-1, g.qty); });
    button(r, "Flatten (X)", "CANCEL", [] { flatten(); });
    r = row(body);
    button(r, "Reverse", "ARROW_LEFTRIGHT", [] { reverse(); });
    button(r, "Cancel orders (C)", "X", [] { g.broker.cancel_all(); });

    r = row(body);
    W.price = number(r, "Price", 0, 0, 1e6, kTick, 2, [](double v) { g.ticket_price = round_tick(v); });
    BKNode* last = button(r, "Last", nullptr, [] { g.ticket_price = g.last(); });
    BK_set_num(last, "weight", 0.3);
    r = row(body);
    button(r, "Buy Lmt", nullptr, [] { pending_order(1, OrderType::Limit, g.ticket_price); });
    button(r, "Sell Lmt", nullptr, [] { pending_order(-1, OrderType::Limit, g.ticket_price); });
    button(r, "Buy Stp", nullptr, [] { pending_order(1, OrderType::Stop, g.ticket_price); });
    button(r, "Sell Stp", nullptr, [] { pending_order(-1, OrderType::Stop, g.ticket_price); });
    dim_label(body, "Market orders fill at the next minute's open: step or play.");
}

void build_position(BKNode* body) {
    W.pos = label(body, "");
    BKNode* r = row(body);
    W.open = label(r, "");
    W.closed = label(r, "");
    W.total = label(r, "");
    W.fees = dim_label(body, "");
    W.orders = add(body, "column");
}

void build_indicators(BKNode* body) {
    W.add_row = row(body);
    W.inds = add(body, "column");
    BK_set_num(W.inds, "spacing", 4);
    BKNode* r = row(body);
    button(r, "Scripts folder", "FILE_FOLDER", [] { open_scripts_folder(); });
    button(r, "Reload scripts", "FILE_REFRESH", [] {
        g_scripts.reload();
        g.status = "scripts reloaded";
    });
    dim_label(body, "Indicators are Lua scripts: add your own to the scripts folder.");
}

void rebuild_add_row() {
    clear(W.add_row);
    static std::vector<std::string> names;
    static std::vector<const char*> items;
    static std::vector<std::string> ids;
    names.clear(), items.clear(), ids.clear();
    for (const ScriptDef* d : g_scripts.list(false)) {
        names.push_back(d->error.empty() ? d->name : d->id + " (error)");
        ids.push_back(d->id);
    }
    for (auto& n : names) items.push_back(n.c_str());
    if (items.empty()) {
        dim_label(W.add_row, "No indicator scripts.");
        return;
    }
    g_add_index = std::min(g_add_index, static_cast<int>(items.size()) - 1);
    choice(W.add_row, "dropdown", items.data(), static_cast<int>(items.size()), g_add_index, [](int v) { g_add_index = v; });
    BKNode* b = button(W.add_row, "Add", "ADD", [] {
        if (g_add_index < static_cast<int>(ids.size())) g.inds.push_back(make_ind(ids[g_add_index], g.next_uid++));
    });
    BK_set_num(b, "weight", 0.4);
}

void build_strategy(BKNode* body) {
    W.strat_row = row(body);
    W.strat_desc = dim_label(body, "");
    W.strat_params = add(body, "column");
    W.strat_auto = check(body, "Auto-trade the replay", false, [](bool on) {
        if (on) start_strategy();
        else stop_strategy();
    });
    W.strat_status = dim_label(body, "");
    add(body, "separator");
    BKNode* r = row(body);
    W.bt_from = add(r, "text");
    BK_set_str(W.bt_from, "label", "From");
    BK_set_str(W.bt_from, "placeholder", "YYYY-MM-DD");
    W.bt_to = add(r, "text");
    BK_set_str(W.bt_to, "label", "To");
    BK_set_str(W.bt_to, "placeholder", "YYYY-MM-DD");
    if (g.m.size()) {  // default: from the start of last year to the end of the data
        int64_t last = g.m.t.back();
        std::string to = fmt_time(last, "%Y-%m-%d"), from = std::to_string(std::stoi(to.substr(0, 4)) - 1) + "-01-01";
        BK_set_str(W.bt_from, "value", from.c_str());
        BK_set_str(W.bt_to, "value", to.c_str());
    }
    r = row(body);
    W.bt_run = button(r, "Run backtest", "PLAY", [] {
        const ScriptDef* d = g_scripts.find(true, g.strat_id);
        int64_t from, to;
        if (!d || !d->error.empty()) {
            g.status = "pick a working strategy first";
            return;
        }
        if (!parse_minutes(BK_get_str(W.bt_from, "value"), from) || !parse_minutes(BK_get_str(W.bt_to, "value"), to)) {
            g.status = "backtest dates: YYYY-MM-DD";
            return;
        }
        g.bt.start(*d, g.strat_params, &g.m, from, to + 1440, g.broker);  // "to" is inclusive
    });
    W.bt_stop = button(r, "Stop", "CANCEL", [] { g.bt.cancel(); });
    W.bt_bar = add(body, "progress");
    W.bt_summary = add(body, "column");
    BK_node_add(body, backtest_curve_new(), -1);
    r = row(body);
    W.bt_export = button(r, "Export trades", "EXPORT", [] {
        std::filesystem::path dir = data_dir() / "sessions";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        std::time_t now = std::time(nullptr);
        char name[64];
        std::strftime(name, sizeof name, "backtest_%Y%m%d_%H%M%S.csv", std::localtime(&now));
        g.status = export_trips_csv(dir / name, g.bt_result.trips) ? "saved " + (dir / name).string() : "could not save the CSV";
    });
    button(r, "Scripts folder", "FILE_FOLDER", [] { open_scripts_folder(); });
}

std::string money_k(double v) {  // $12,345
    char buf[32];
    std::snprintf(buf, sizeof buf, "%s$%.0f", v < 0 ? "-" : "", std::fabs(v));
    std::string s = buf;
    size_t start = v < 0 ? 2 : 1;
    for (int i = static_cast<int>(s.size()) - 3; i > static_cast<int>(start); i -= 3) s.insert(static_cast<size_t>(i), ",");
    return s;
}

void rebuild_strategy() {
    clear(W.strat_row);
    W.strat_choice = nullptr;
    static std::vector<std::string> names, ids;
    static std::vector<const char*> items;
    names.clear(), ids.clear(), items.clear();
    for (const ScriptDef* d : g_scripts.list(true)) {
        names.push_back(d->error.empty() ? d->name : d->id + " (error)");
        ids.push_back(d->id);
    }
    for (auto& n : names) items.push_back(n.c_str());
    if (ids.empty()) {
        dim_label(W.strat_row, "No strategy scripts.");
        return;
    }
    if (g.strat_id.empty() || !g_scripts.find(true, g.strat_id)) select_strategy(ids[0]);
    int sel = static_cast<int>(std::find(ids.begin(), ids.end(), g.strat_id) - ids.begin());
    BKNode* c = choice(W.strat_row, "dropdown", items.data(), static_cast<int>(items.size()), sel, [](int v) {
        if (v < static_cast<int>(ids.size())) select_strategy(ids[v]);
    });
    BK_set_str(c, "label", "Strategy");
    W.strat_choice = c;
}

int strategy_index() {  // the selected strategy's position in the dropdown
    auto list = g_scripts.list(true);
    for (size_t k = 0; k < list.size(); ++k)
        if (list[k]->id == g.strat_id) return static_cast<int>(k);
    return 0;
}

void rebuild_strategy_params() {
    clear(W.strat_params);
    const ScriptDef* d = g_scripts.find(true, g.strat_id);
    if (!d) return;
    if (!d->error.empty()) {
        set_color(label(W.strat_params, d->error.substr(0, d->error.find('\n'))), kSell);
        return;
    }
    if (g.strat_params.size() != d->params.size()) g.strat_params = default_params(*d);
    param_widgets(W.strat_params, *d, [] { return &g.strat_params; });
}

void sync_strategy() {
    std::string sig = g.strat_id + "|" + std::to_string(g_scripts.generation());
    if (W.strat_gen != g_scripts.generation()) {
        W.strat_gen = g_scripts.generation();
        rebuild_strategy();
        if (g.auto_trade) start_strategy();  // the script changed: restart it with the new code
    }
    if (sig != W.strat_sig) {
        W.strat_sig = sig;
        rebuild_strategy_params();
        const ScriptDef* d = g_scripts.find(true, g.strat_id);
        std::string desc = d ? d->description : "";
        if (d && d->timeframe > 1) desc = std::to_string(d->timeframe) + "-minute bars. " + desc;
        set_text(W.strat_desc, desc);
    }
    if (W.strat_choice) set_value(W.strat_choice, strategy_index());
    set_value(W.strat_auto, g.auto_trade);
    std::string st = g.auto_trade ? "Trading the replay on the paper account above."
                   : !g.strat.error().empty() ? "Stopped: " + g.strat.error().substr(0, g.strat.error().find('\n')) : "";
    set_text(W.strat_status, st);
    bool busy = g.bt.running();
    set_enabled(W.bt_run, !busy);
    set_enabled(W.bt_stop, busy);
    double f = busy ? g.bt.progress() : (g.bt_result.trips.empty() && g.bt_result.error.empty() ? 0.0 : 1.0);
    if (BK_get_num(W.bt_bar, "factor") != f) BK_set_num(W.bt_bar, "factor", f);
    set_enabled(W.bt_export, !g.bt_result.trips.empty());
    if (W.bt_seen == g.bt_seen) return;
    W.bt_seen = g.bt_seen;
    clear(W.bt_summary);
    const BacktestResult& r = g.bt_result;
    backtest_curve_changed();
    if (g.bt_seen == 0) return;
    if (!r.error.empty()) set_color(label(W.bt_summary, r.error.substr(0, r.error.find('\n'))), kSell);
    if (r.trips.empty() && r.error.empty()) dim_label(W.bt_summary, "No trades.");
    if (r.trips.empty()) return;
    const BacktestStats& s = r.stats;
    char buf[160];
    label(W.bt_summary, r.params);
    BKNode* net = label(W.bt_summary, "");
    std::snprintf(buf, sizeof buf, "Net %s   %d trades   win %.0f%%   PF %.2f", money_k(s.net).c_str(), s.trades,
                  100.0 * s.wins / std::max(1, s.trades), s.gross_loss > 0 ? s.gross_win / s.gross_loss : 0.0);
    set_text(net, buf);
    set_color(net, s.net >= 0 ? kBuy : kSell);
    int losses = s.trades - s.wins;
    std::snprintf(buf, sizeof buf, "Avg win %s   avg loss %s   %s / trade", money_k(s.wins ? s.gross_win / s.wins : 0).c_str(),
                  money_k(losses ? -s.gross_loss / losses : 0).c_str(), money_k(s.net / std::max(1, s.trades)).c_str());
    label(W.bt_summary, buf);
    std::snprintf(buf, sizeof buf, "Max drawdown %s   Sharpe %.2f (daily)   days up %.0f%%", money_k(s.max_dd).c_str(), s.sharpe,
                  s.pct_days_up);
    label(W.bt_summary, buf);
    std::snprintf(buf, sizeof buf, "Best %s   worst %s   longest losing run %d", money_k(s.largest_win).c_str(),
                  money_k(s.largest_loss).c_str(), s.max_consec_losses);
    label(W.bt_summary, buf);
    std::snprintf(buf, sizeof buf, "Avg hold %.0f min   fees %s   %s to %s, %d sessions", s.avg_minutes, money_k(s.fees).c_str(),
                  fmt_time(r.from, "%Y-%m-%d").c_str(), fmt_time(r.to, "%Y-%m-%d").c_str(), s.days);
    dim_label(W.bt_summary, buf);
}

void build_trades(BKNode* body) {
    W.stats = label(body, "");
    BK_node_add(body, equity_new(), -1);
    BKNode* head = row(body);
    for (const char* h : {"In", "Side", "Qty", "Entry / exit", "$"}) set_color(label(head, h), dim());
    W.trips = add(body, "scroll");
    BK_set_num(W.trips, "height", 7);
    add(W.trips, "column");
    BKNode* r = row(body);
    button(r, "Export CSV", "EXPORT", [] { export_csv(); });
    button(r, "Reset P&L", "LOOP_BACK", [] { new_session(g.cursor, "P&L reset"); });
}

// ---------------------------------------------------------------- dynamic lists
Ind* find_ind(int uid) {
    for (Ind& d : g.inds)
        if (d.uid == uid) return &d;
    return nullptr;
}

void rebuild_indicators() {
    clear(W.inds);
    if (g.inds.empty()) {
        dim_label(W.inds, "No indicators. Pick one and press Add.");
        return;
    }
    for (const Ind& d : g.inds) {
        int uid = d.uid;
        BKNode* box = add(W.inds, "box");
        BKNode* top = row(box);
        const ScriptDef* def = d.def();
        static std::map<int, std::string> titles;  // stable strings for the checkbox labels
        titles[uid] = def ? def->name : d.script + " (missing)";
        check(top, titles[uid].c_str(), d.on, [uid](bool v) {
            if (Ind* p = find_ind(uid)) p->on = v;
        });
        BKNode* sw = add(top, "swatch");
        float c[4] = {d.col.r, d.col.g, d.col.b, d.col.a};
        BK_set_color(sw, "value", c);
        BK_set_num(sw, "weight", 0.35);
        handlers[sw] = [sw, uid](int kind) {
            float v[4];
            if (kind == BK_EV_CHANGE && BK_get_color(sw, "value", v))
                if (Ind* p = find_ind(uid)) p->col = {v[0], v[1], v[2], 1};
        };
        BKNode* x = button(top, "", "X", [uid] {
            g.inds.erase(std::remove_if(g.inds.begin(), g.inds.end(), [uid](const Ind& i) { return i.uid == uid; }),
                         g.inds.end());
        });
        BK_set_num(x, "weight", 0);
        BK_set_num(x, "emboss", 0);
        if (!def) continue;
        if (!def->error.empty()) {
            set_color(label(box, def->error.substr(0, def->error.find('\n'))), kSell);
            continue;
        }
        if (d.params.size() != def->params.size())  // the script's params changed: start from its defaults
            if (Ind* p = find_ind(uid)) p->params = default_params(*def);
        param_widgets(box, *def, [uid]() -> Params* {
            Ind* p = find_ind(uid);
            return p ? &p->params : nullptr;
        });
    }
}

std::string order_text(const Order& o) {
    char buf[96];
    const char* type = o.type == OrderType::Market ? "MKT" : o.type == OrderType::Limit ? "LMT" : "STP";
    if (o.type == OrderType::Market)
        std::snprintf(buf, sizeof buf, "%s %d  next open%s", type, o.qty, o.reduce_only ? "  (bracket)" : "");
    else
        std::snprintf(buf, sizeof buf, "%s %d @ %.2f%s", type, o.qty, o.price, o.reduce_only ? "  (bracket)" : "");
    return buf;
}

void sync_orders() {
    std::string sig;
    for (const Order& o : g.broker.orders) sig += std::to_string(o.id) + ",";
    if (sig != W.orders_sig) {
        W.orders_sig = sig;
        clear(W.orders);
        for (const Order& o : g.broker.orders) {
            int id = o.id;
            BKNode* r = row(W.orders);
            BKNode* side = label(r, o.side > 0 ? "BUY" : "SELL");
            set_color(side, o.side > 0 ? kBuy : kSell);
            BK_set_num(side, "weight", 0.3);
            label(r, "");
            BKNode* x = button(r, "", "X", [id] { g.broker.cancel(id); });
            BK_set_num(x, "weight", 0);
            BK_set_num(x, "emboss", 0);
        }
    }
    for (size_t k = 0; k < g.broker.orders.size(); ++k)  // prices move when lines are dragged
        set_text(BK_node_child(BK_node_child(W.orders, static_cast<int>(k)), 1), order_text(g.broker.orders[k]));
}

void sync_trades() {
    const auto& tr = g.broker.trips;
    int wins = 0;
    double net = 0, gw = 0, gl = 0, peak = 0, dd = 0;
    for (const RoundTrip& r : tr) {
        net += r.pnl, wins += r.pnl > 0;
        (r.pnl > 0 ? gw : gl) += std::fabs(r.pnl);
        peak = std::max(peak, net), dd = std::max(dd, peak - net);
    }
    char buf[160];
    std::snprintf(buf, sizeof buf, "%zu trades   win %.0f%%   net %+.0f   PF %.2f   max DD %.0f", tr.size(),
                  tr.empty() ? 0.0 : 100.0 * wins / tr.size(), net, gl > 0 ? gw / gl : 0.0, dd);
    set_text(W.stats, buf);

    std::string sig = std::to_string(tr.size()) + (g.hide_dates ? "h" : "d");
    if (sig == W.trips_sig) return;
    W.trips_sig = sig;
    BKNode* col = BK_node_child(W.trips, 0);
    clear(col);
    int shown = 0;
    for (auto it = tr.rbegin(); it != tr.rend() && shown < 200; ++it, ++shown) {
        BKNode* r = row(col);
        label(r, fmt_time(it->t_in, g.hide_dates ? "%H:%M" : "%m-%d %H:%M"));
        set_color(label(r, it->side > 0 ? "long" : "short"), it->side > 0 ? kBuy : kSell);
        label(r, std::to_string(it->qty));
        std::snprintf(buf, sizeof buf, "%.2f / %.2f", it->p_in, it->p_out);
        label(r, buf);
        set_color(label(r, money(it->pnl)), it->pnl > 0 ? kBuy : kSell);
    }
    if (tr.empty()) dim_label(col, "No closed trades.");
}

void sync_log() {
    const auto& log = g.broker.log;
    std::string sig = std::to_string(log.size()) + "|" + g.status;
    if (sig == W.log_sig) return;
    W.log_sig = sig;
    clear(W.log);
    for (size_t k = log.size() > 6 ? log.size() - 6 : 0; k < log.size(); ++k) dim_label(W.log, log[k]);
    if (!g.status.empty()) dim_label(W.log, g.status);
}

}  // namespace

// ---------------------------------------------------------------- public
BKNode* panels_new() {
    W.scroll = BK_node_new("scroll");
    W.col = add(W.scroll, "column");
    BK_set_num(W.col, "padding", 6);
    BK_set_num(W.col, "spacing", 3);
    build_replay(panel(W.col, "Replay"));
    build_ticket(panel(W.col, "Order Ticket"));
    build_position(panel(W.col, "Position & Orders"));
    build_indicators(panel(W.col, "Indicators"));
    build_trades(panel(W.col, "Trades"));
    build_strategy(panel(W.col, "Strategy (Lua)"));
    W.log = add(W.col, "column");
    return W.scroll;
}

void panels_sync() {
    if (BK_unit() != W.unit) {  // widths are in raster pixels: follow the UI scale (Retina)
        W.unit = BK_unit();
        BK_geom_set(W.scroll, "reqwidth", 21 * W.unit);
        BK_node_relayout(W.scroll);
    }
    // replay
    set_text(W.clock, fmt_time(g.now(), g.hide_dates ? "%H:%M NY" : "%a %Y-%m-%d %H:%M NY"));
    bool rth = in_rth(g.m.t[g.cursor]);
    set_text(W.session, rth ? "RTH" : "ETH");
    set_color(W.session, rth ? kBuy : dim());
    set_text(W.play, g.playing ? "Pause (Space)" : "Play (Space)");
    if (g.playing != W.play_icon) set_icon(W.play, (W.play_icon = g.playing) ? "PAUSE" : "PLAY");
    set_value(W.speed, g.speed_idx);
    set_value(W.tf, g.tf_idx);
    set_value(W.visible, std::round(g.visible));
    set_value(W.follow, g.follow);
    // ticket
    set_enabled(W.inst, g.broker.pos == 0);
    for (int i = 0; i < BK_node_child_count(W.bracket_row); ++i) set_enabled(BK_node_child(W.bracket_row, i), g.bracket);
    if (g.ticket_price == 0) g.ticket_price = g.last();
    set_value(W.price, g.ticket_price);
    // position
    const Broker& b = g.broker;
    double lp = g.last(), u = b.unrealized(lp), net = b.realized - b.fees;
    char buf[128];
    std::snprintf(buf, sizeof buf, "Position %s%d   avg %.2f   last %.2f", b.pos > 0 ? "+" : "", b.pos, b.avg, lp);
    set_text(W.pos, buf);
    set_text(W.open, "Open " + money(u));
    set_color(W.open, u >= 0 ? kBuy : kSell);
    set_text(W.closed, "Closed " + money(net));
    set_color(W.closed, net >= 0 ? kBuy : kSell);
    set_text(W.total, "Total " + money(net + u));
    set_color(W.total, net + u >= 0 ? kBuy : kSell);
    std::snprintf(buf, sizeof buf, "Closed P&L is after $%.2f fees.", b.fees);
    set_text(W.fees, buf);
    sync_orders();
    // indicators
    if (W.inds_gen != g_scripts.generation()) {  // scripts reloaded: new list, maybe new params
        W.inds_gen = g_scripts.generation();
        rebuild_add_row();
        W.inds_sig = "-";
    }
    std::string sig;
    for (const Ind& d : g.inds) sig += std::to_string(d.uid) + ",";
    if (sig != W.inds_sig) {  // starts as "-" so the first sync builds the list
        W.inds_sig = sig;
        rebuild_indicators();
    }
    sync_trades();
    sync_strategy();
    sync_log();
}

bool panels_event(BKNode* node, int kind) {
    auto it = handlers.find(node);
    if (it == handlers.end()) return false;
    auto fn = it->second;  // the handler may rebuild lists; keep a copy alive while it runs
    fn(kind);
    return true;
}
