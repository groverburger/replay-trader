// NQ replay trader: reveal 1-minute bars one at a time, chart them on any timeframe, add indicators, and paper
// trade against the replay. Nothing after the replay cursor is ever shown or used.
//
// Usage: replay_trader [--start "YYYY-MM-DD HH:MM"] [--screenshot out.bmp] [--demo]
// Keys:  Space play/pause, Right step 1 min, Shift+Right finish the chart bar, N skip to next 09:30 open,
//        Up/Down speed, B buy, S sell, X flatten, C cancel orders, L follow latest bar.
// Chart: Ctrl+click places a limit at that price, Shift+click a stop; drag order lines to move them.

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "broker.h"
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "implot.h"
#include "implot_internal.h"
#include "indicators.h"
#include "market.h"

namespace {

const ImVec4 kUp(0.15f, 0.65f, 0.60f, 1.0f), kDown(0.94f, 0.33f, 0.31f, 1.0f);
const ImVec4 kBuy(0.30f, 0.85f, 0.45f, 1.0f), kSell(1.00f, 0.45f, 0.35f, 1.0f);
const int kTfs[] = {1, 5, 15, 30, 60, 240, 1440};
const char* kTfNames[] = {"1m", "5m", "15m", "30m", "1h", "4h", "1D"};
const int kSpeeds[] = {1, 2, 5, 10, 30, 60, 120, 300, 600};  // minutes revealed per second

// ---------------------------------------------------------------- indicators the user can add
enum IndType { SMA, EMA, BOLL, VWAP, PIVOTS, RSI, MACD, ATR, VOLUME, IND_COUNT };
const char* kIndNames[] = {"SMA", "EMA", "Bollinger bands", "VWAP (session)", "Swing pivots", "RSI", "MACD", "ATR",
                           "Volume"};
const ImVec4 kPalette[] = {{1.0f, 0.8f, 0.2f, 1}, {0.55f, 0.65f, 1.0f, 1}, {0.9f, 0.5f, 0.9f, 1},
                           {0.4f, 0.9f, 0.9f, 1}, {1.0f, 0.6f, 0.3f, 1}, {0.7f, 0.9f, 0.4f, 1}};

struct Ind {
    int type, uid;
    int a = 20, b = 0, c = 0;  // periods: SMA/EMA a; BOLL a; PIVOTS left a, right b, levels c; RSI a;
    float k = 2.0f;            // MACD a,b,c = fast, slow, signal; ATR a; BOLL k = std devs
    ImVec4 col;
    bool on = true;
    std::string label() const {
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
    bool oscillator() const { return type == RSI || type == MACD || type == ATR || type == VOLUME; }
    int warmup() const { return type == MACD ? 4 * (b + c) : type == PIVOTS ? a + b + 1 : 4 * a + 10; }
};

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
    float stop_pts = 20, target_pts = 40;
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
    const Agg& agg() {
        auto it = aggs.find(tf());
        if (it == aggs.end()) {
            it = aggs.emplace(tf(), Agg{}).first;
            it->second.build(m, tf());
        }
        return it->second;
    }
    double last() const { return m.c[cursor]; }
    int64_t now() const { return m.t[cursor] + 1; }  // the revealed bar has closed
};

App g;

std::string fmt_time(int64_t minutes, const char* f) {
    time_t s = static_cast<time_t>(minutes) * 60;
    std::tm tm{};
    gmtime_r(&s, &tm);  // times are naive New York local time
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
    g.status = why;
}

void random_day() {
    std::uniform_int_distribution<size_t> d(0, g.opens.size() - 1);
    size_t i = g.opens[d(g.rng)];
    new_session(i >= 5 ? i - 5 : i, "new session on a random day (09:25)");
}

// ---------------------------------------------------------------- chart data
void refresh_series() {
    int warm = 300;
    for (const Ind& d : g.inds)
        if (d.on) warm = std::max(warm, d.warmup());
    const Agg& a = g.agg();
    int64_t cur_bucket = static_cast<int64_t>(a.bucket_of(g.cursor));
    int64_t view_lo = g.follow ? cur_bucket - g.visible : static_cast<int64_t>(std::floor(g.xmin));
    int64_t lo = std::max<int64_t>(0, std::min(view_lo, cur_bucket) - warm);
    lo = std::max(lo, cur_bucket - 60000);  // bound the work when zoomed far out
    lo -= lo % 64;                          // fewer rebuilds while panning
    if (g.s_cursor == g.cursor && g.s_lo == lo && g.s_tf == g.tf()) return;
    build_series(g.m, a, g.cursor, lo, g.s);
    g.s_cursor = g.cursor, g.s_lo = lo, g.s_tf = g.tf();
}

int time_axis_fmt(double value, char* buf, int size, void*) {
    long j = std::lround(value) - static_cast<long>(g.s.x0);
    if (j < 0 || j >= static_cast<long>(g.s.n())) return buf[0] = 0, 0;
    const char* f = g.tf() >= 1440 ? (g.hide_dates ? "%a" : "%y-%m-%d") : (g.hide_dates ? "%H:%M" : "%m-%d %H:%M");
    return std::snprintf(buf, size, "%s", fmt_time(static_cast<int64_t>(g.s.t[j]), f).c_str());
}

void visible_range(long& j0, long& j1) {
    j0 = std::max(0L, static_cast<long>(std::floor(g.xmin)) - static_cast<long>(g.s.x0));
    j1 = std::min(static_cast<long>(g.s.n()) - 1, static_cast<long>(std::ceil(g.xmax)) - static_cast<long>(g.s.x0));
}

void plot_candles() {
    const Series& s = g.s;
    long j0, j1;
    visible_range(j0, j1);
    if (j1 < j0) return;
    float px = ImPlot::PlotToPixels(1, 0).x - ImPlot::PlotToPixels(0, 0).x;
    if (px < 2.0f) {
        ImPlot::SetNextLineStyle(ImVec4(0.85f, 0.85f, 0.85f, 1), 1.0f);
        ImPlot::PlotLine("NQ", s.c.data() + j0, static_cast<int>(j1 - j0 + 1), 1.0, s.x0 + j0);
        return;
    }
    if (!ImPlot::BeginItem("NQ")) return;
    ImDrawList* dl = ImPlot::GetPlotDrawList();
    float half = std::max(1.0f, px * 0.36f);
    for (long j = j0; j <= j1; ++j) {
        double x = static_cast<double>(s.x0 + j);
        ImU32 col = ImGui::GetColorU32(s.c[j] >= s.o[j] ? kUp : kDown);
        if (j == static_cast<long>(s.n()) - 1 && s.last_partial) col = (col & 0x00FFFFFF) | 0xB0000000;  // forming
        ImVec2 hi = ImPlot::PlotToPixels(x, s.h[j]), lo = ImPlot::PlotToPixels(x, s.l[j]);
        ImVec2 op = ImPlot::PlotToPixels(x, s.o[j]), cl = ImPlot::PlotToPixels(x, s.c[j]);
        dl->AddLine(hi, lo, col);
        float top = std::min(op.y, cl.y), bot = std::max(op.y, cl.y);
        if (bot - top < 1.0f) bot = top + 1.0f;
        dl->AddRectFilled(ImVec2(op.x - half, top), ImVec2(op.x + half, bot), col);
    }
    ImPlot::EndItem();
}

void plot_line(const char* label, const ind::Vec& y, ImVec4 col, float w = 1.4f) {
    long j0, j1;
    visible_range(j0, j1);
    if (j1 < j0) return;
    ImPlot::SetNextLineStyle(col, w);
    ImPlot::PlotLine(label, y.data() + j0, static_cast<int>(j1 - j0 + 1), 1.0, g.s.x0 + j0);
}

// Dashed lines at 09:30 and 16:00 on intraday charts, placed inside the bar that contains them.
void draw_sessions() {
    if (!g.sessions || g.tf() >= 1440) return;
    long j0, j1;
    visible_range(j0, j1);
    if (j1 - j0 > 3000) return;
    ImDrawList* dl = ImPlot::GetPlotDrawList();
    ImPlotRect lim = ImPlot::GetPlotLimits();
    ImPlot::PushPlotClipRect();
    for (long j = j0; j <= j1; ++j) {
        int64_t t = static_cast<int64_t>(g.s.t[j]), tod = ((t % 1440) + 1440) % 1440;
        for (int mark : {570, 960}) {
            if (tod > mark || tod + g.tf() <= mark) continue;
            double x = g.s.x0 + j - 0.5 + static_cast<double>(mark - tod) / g.tf();
            ImVec2 a = ImPlot::PlotToPixels(x, lim.Y.Max), b = ImPlot::PlotToPixels(x, lim.Y.Min);
            ImU32 col = ImGui::GetColorU32(mark == 570 ? ImVec4(0.45f, 0.85f, 0.55f, 0.5f) : ImVec4(0.95f, 0.55f, 0.45f, 0.5f));
            for (float y = a.y; y < b.y; y += 8) dl->AddLine(ImVec2(a.x, y), ImVec2(a.x, std::min(y + 4, b.y)), col);
        }
    }
    ImPlot::PopPlotClipRect();
}

void fit_price_y() {
    long j0, j1;
    visible_range(j0, j1);
    double lo = INFINITY, hi = -INFINITY;
    for (long j = j0; j <= j1; ++j) lo = std::min(lo, g.s.l[j]), hi = std::max(hi, g.s.h[j]);
    if (!std::isfinite(lo)) return;
    double pad = std::max((hi - lo) * 0.08, 2.0);
    ImPlot::SetupAxisLimits(ImAxis_Y1, lo - pad, hi + pad, ImPlotCond_Always);
}

void fit_y(std::initializer_list<const ind::Vec*> ys, double pad_frac = 0.08) {
    long j0, j1;
    visible_range(j0, j1);
    double lo = INFINITY, hi = -INFINITY;
    for (auto* y : ys)
        for (long j = j0; j <= j1; ++j)
            if (std::isfinite((*y)[j])) lo = std::min(lo, (*y)[j]), hi = std::max(hi, (*y)[j]);
    if (!std::isfinite(lo)) return;
    double pad = std::max((hi - lo) * pad_frac, 1e-9);
    ImPlot::SetupAxisLimits(ImAxis_Y1, lo - pad, hi + pad, ImPlotCond_Always);
}

void price_overlays() {
    const Series& s = g.s;
    for (const Ind& d : g.inds) {
        if (!d.on || d.oscillator()) continue;
        std::string lab = d.label();
        switch (d.type) {
            case SMA: plot_line(lab.c_str(), ind::sma(s.c, d.a), d.col); break;
            case EMA: plot_line(lab.c_str(), ind::ema(s.c, d.a), d.col); break;
            case VWAP: plot_line(lab.c_str(), ind::vwap(s), d.col, 1.6f); break;
            case BOLL: {
                ind::Vec mid, up, lo;
                ind::bollinger(s.c, d.a, d.k, mid, up, lo);
                long j0, j1;
                visible_range(j0, j1);
                if (j1 >= j0) {
                    std::vector<double> xs(j1 - j0 + 1);
                    for (long j = j0; j <= j1; ++j) xs[j - j0] = static_cast<double>(s.x0 + j);
                    ImPlot::SetNextFillStyle(d.col, 0.08f);
                    ImPlot::PlotShaded(lab.c_str(), xs.data(), up.data() + j0, lo.data() + j0, static_cast<int>(xs.size()));
                }
                plot_line(lab.c_str(), up, d.col, 1.0f);
                plot_line(lab.c_str(), lo, d.col, 1.0f);
                plot_line(lab.c_str(), mid, ImVec4(d.col.x, d.col.y, d.col.z, 0.6f), 1.0f);
                break;
            }
            case PIVOTS: {
                ImDrawList* dl = ImPlot::GetPlotDrawList();
                ImPlot::PushPlotClipRect();
                long j0, j1;
                visible_range(j0, j1);
                for (const auto& p : ind::pivots(s, d.a, d.b)) {
                    double x = static_cast<double>(s.x0 + p.i);
                    if (d.c) {  // level until it is traded through
                        long end = p.broken >= 0 ? p.broken : static_cast<long>(s.n()) - 1;
                        if (end >= j0 && static_cast<long>(p.i) <= j1)
                            dl->AddLine(ImPlot::PlotToPixels(x, p.price), ImPlot::PlotToPixels(s.x0 + end, p.price),
                                        ImGui::GetColorU32(ImVec4(d.col.x, d.col.y, d.col.z, p.broken >= 0 ? 0.25f : 0.6f)));
                    }
                    if (static_cast<long>(p.i) < j0 || static_cast<long>(p.i) > j1) continue;
                    ImVec2 c = ImPlot::PlotToPixels(x, p.price);
                    ImU32 col = ImGui::GetColorU32(d.col);
                    if (p.high)
                        dl->AddTriangleFilled(ImVec2(c.x, c.y - 4), ImVec2(c.x - 4, c.y - 11), ImVec2(c.x + 4, c.y - 11), col);
                    else
                        dl->AddTriangleFilled(ImVec2(c.x, c.y + 4), ImVec2(c.x - 4, c.y + 11), ImVec2(c.x + 4, c.y + 11), col);
                }
                ImPlot::PopPlotClipRect();
                break;
            }
            default: break;
        }
    }
}

// Fills, working orders (draggable), position and last price on the price chart.
void trading_overlays() {
    Broker& b = g.broker;
    ImDrawList* dl = ImPlot::GetPlotDrawList();
    ImPlot::PushPlotClipRect();
    const Agg& a = g.agg();
    for (const Fill& f : b.fills) {
        size_t i = g.m.index_before(f.time + 1);
        double x = static_cast<double>(a.bucket_of(i));
        ImVec2 c = ImPlot::PlotToPixels(x, f.price);
        ImU32 col = ImGui::GetColorU32(f.side > 0 ? kBuy : kSell);
        if (f.side > 0)
            dl->AddTriangleFilled(ImVec2(c.x, c.y), ImVec2(c.x - 6, c.y + 10), ImVec2(c.x + 6, c.y + 10), col);
        else
            dl->AddTriangleFilled(ImVec2(c.x, c.y), ImVec2(c.x - 6, c.y - 10), ImVec2(c.x + 6, c.y - 10), col);
    }
    ImPlot::PopPlotClipRect();

    double lp = g.last();
    ImPlot::TagY(lp, ImVec4(0.8f, 0.8f, 0.8f, 1), "%.2f", lp);
    if (b.pos) {
        double avg = b.avg;
        ImVec4 col = b.pos > 0 ? kBuy : kSell;
        ImPlot::SetNextLineStyle(ImVec4(col.x, col.y, col.z, 0.8f), 1.5f);
        ImPlot::PlotInfLines("##pos", &avg, 1, ImPlotInfLinesFlags_Horizontal);
        double u = b.unrealized(lp);
        ImPlot::TagY(avg, col, "%s %d %s$%.0f", b.pos > 0 ? "LONG" : "SHORT", std::abs(b.pos), u < 0 ? "-" : "+",
                     std::fabs(u));
    }
    for (Order& o : b.orders) {
        if (o.type == OrderType::Market) continue;
        ImVec4 col = o.side > 0 ? kBuy : kSell;
        col.w = o.type == OrderType::Stop ? 0.75f : 1.0f;
        if (ImPlot::DragLineY(o.id, &o.price, col, o.type == OrderType::Stop ? 1.0f : 1.6f)) o.price = round_tick(o.price);
        ImPlot::TagY(o.price, col, "%s %s %d", o.side > 0 ? "BUY" : "SELL", o.type == OrderType::Stop ? "STP" : "LMT", o.qty);
    }
}

void place_from_click() {
    if (!ImPlot::IsPlotHovered() || !ImGui::IsMouseClicked(ImGuiMouseButton_Left)) return;
    const ImGuiIO& io = ImGui::GetIO();
    if (!io.KeyCtrl && !io.KeyShift) return;
    double p = round_tick(ImPlot::GetPlotMousePos().y);
    Order o;
    o.qty = g.qty, o.born = g.cursor, o.price = p;
    o.stop_pts = g.bracket ? g.stop_pts : 0, o.target_pts = g.bracket ? g.target_pts : 0;
    if (io.KeyCtrl) o.type = OrderType::Limit, o.side = p < g.last() ? 1 : -1;
    else o.type = OrderType::Stop, o.side = p > g.last() ? 1 : -1;
    g.broker.submit(o);
}

void hover_tooltip() {
    if (!ImPlot::IsPlotHovered()) return;
    long j = std::lround(ImPlot::GetPlotMousePos().x) - static_cast<long>(g.s.x0);
    if (j < 0 || j >= static_cast<long>(g.s.n())) return;
    const Series& s = g.s;
    ImGui::BeginTooltip();
    ImGui::Text("%s%s", fmt_time(static_cast<int64_t>(s.t[j]), g.hide_dates ? "%H:%M" : "%Y-%m-%d %H:%M").c_str(),
                j == static_cast<long>(s.n()) - 1 && s.last_partial ? "  (forming)" : "");
    ImGui::Text("O %.2f  H %.2f  L %.2f  C %.2f  V %.0f", s.o[j], s.h[j], s.l[j], s.c[j], s.v[j]);
    ImGui::TextDisabled("Ctrl+click: limit here   Shift+click: stop here");
    ImGui::EndTooltip();
}

void setup_x(bool labels) {
    ImPlot::SetupAxis(ImAxis_X1, nullptr, labels ? 0 : ImPlotAxisFlags_NoTickLabels);
    ImPlot::SetupAxisLinks(ImAxis_X1, &g.xmin, &g.xmax);
    ImPlot::SetupAxisFormat(ImAxis_X1, time_axis_fmt, nullptr);
}

void draw_charts() {
    refresh_series();
    const Series& s = g.s;
    double last_x = static_cast<double>(s.x0 + s.n() - 1);
    if (g.follow) {
        g.xmax = last_x + std::max(3.0, g.visible * 0.12);
        g.xmin = g.xmax - g.visible;
    }
    const double set_min = g.xmin, set_max = g.xmax;

    std::vector<const Ind*> oscs;
    for (const Ind& d : g.inds)
        if (d.on && d.oscillator()) oscs.push_back(&d);
    std::vector<float> ratios = {5.0f};
    for (size_t k = 0; k < oscs.size(); ++k) ratios.push_back(1.3f);
    if (!ImPlot::BeginSubplots("##charts", static_cast<int>(ratios.size()), 1, ImVec2(-1, -1), ImPlotSubplotFlags_NoTitle,
                               ratios.data()))
        return;

    if (ImPlot::BeginPlot("##price", ImVec2(), ImPlotFlags_NoMouseText)) {
        setup_x(oscs.empty());
        ImPlot::SetupAxis(ImAxis_Y1, nullptr, ImPlotAxisFlags_Opposite);
        fit_price_y();
        ImPlot::SetupLegend(ImPlotLocation_NorthWest);
        ImPlot::SetupFinish();
        draw_sessions();
        plot_candles();
        price_overlays();
        trading_overlays();
        place_from_click();
        hover_tooltip();
        ImPlot::EndPlot();
    }
    for (size_t k = 0; k < oscs.size(); ++k) {
        const Ind& d = *oscs[k];
        char id[32];
        std::snprintf(id, sizeof id, "##osc%d", d.uid);
        if (!ImPlot::BeginPlot(id, ImVec2(), ImPlotFlags_NoMouseText)) continue;
        setup_x(k + 1 == oscs.size());
        ImPlot::SetupAxis(ImAxis_Y1, nullptr, ImPlotAxisFlags_Opposite);
        ImPlot::SetupLegend(ImPlotLocation_NorthWest);
        std::string lab = d.label();
        if (d.type == RSI) {
            ind::Vec r = ind::rsi(s.c, d.a);
            ImPlot::SetupAxisLimits(ImAxis_Y1, 0, 100, ImPlotCond_Always);
            ImPlot::SetupFinish();
            draw_sessions();
            double lv[2] = {30, 70};
            ImPlot::SetNextLineStyle(ImVec4(1, 1, 1, 0.3f));
            ImPlot::PlotInfLines("##lv", lv, 2, ImPlotInfLinesFlags_Horizontal);
            plot_line(lab.c_str(), r, d.col);
        } else if (d.type == MACD) {
            ind::Vec line, sig, hist;
            ind::macd(s.c, d.a, d.b, d.c, line, sig, hist);
            fit_y({&line, &sig, &hist}, 0.1);
            ImPlot::SetupFinish();
            draw_sessions();
            long j0, j1;
            visible_range(j0, j1);
            if (j1 >= j0) {
                ImPlot::SetNextFillStyle(ImVec4(0.6f, 0.6f, 0.6f, 0.6f));
                ImPlot::PlotBars("hist", hist.data() + j0, static_cast<int>(j1 - j0 + 1), 0.7, s.x0 + j0);
            }
            plot_line(lab.c_str(), line, d.col);
            plot_line("signal", sig, ImVec4(1, 0.6f, 0.3f, 1), 1.0f);
        } else if (d.type == ATR) {
            ind::Vec a = ind::atr(s, d.a);
            fit_y({&a});
            ImPlot::SetupFinish();
            draw_sessions();
            plot_line(lab.c_str(), a, d.col);
        } else if (d.type == VOLUME) {
            ind::Vec v = s.v;
            v.push_back(0);
            fit_y({&s.v}, 0.05);
            ImPlot::SetupFinish();
            long j0, j1;
            visible_range(j0, j1);
            if (j1 >= j0) {
                ImPlot::SetNextFillStyle(d.col, 0.7f);
                ImPlot::PlotBars(lab.c_str(), s.v.data() + j0, static_cast<int>(j1 - j0 + 1), 0.7, s.x0 + j0);
            }
        }
        hover_tooltip();
        ImPlot::EndPlot();
    }
    ImPlot::EndSubplots();
    if (g.follow && (g.xmin != set_min || g.xmax != set_max)) g.follow = false;  // user panned or zoomed
    if (!g.follow) g.visible = std::clamp(static_cast<int>(g.xmax - g.xmin), 10, 20000);
}

// ---------------------------------------------------------------- panels
void market_order(int side, int qty) {
    if (qty <= 0) return;
    Order o;
    o.side = side, o.qty = qty, o.type = OrderType::Market, o.born = g.cursor;
    if (g.bracket && (g.broker.pos == 0 || (g.broker.pos > 0) == (side > 0)))
        o.stop_pts = g.stop_pts, o.target_pts = g.target_pts;
    g.broker.submit(o);
}

void flatten() {
    g.broker.cancel_all();
    if (g.broker.pos) market_order(g.broker.pos > 0 ? -1 : 1, std::abs(g.broker.pos));
}

void replay_panel() {
    ImGui::Text("%s NY", fmt_time(g.now(), g.hide_dates ? "%H:%M" : "%a %Y-%m-%d %H:%M").c_str());
    ImGui::SameLine();
    ImGui::TextColored(in_rth(g.m.t[g.cursor]) ? kBuy : ImVec4(0.6f, 0.6f, 0.6f, 1), in_rth(g.m.t[g.cursor]) ? "RTH" : "ETH");
    if (ImGui::Button(g.playing ? "Pause (Space)" : "Play (Space)", ImVec2(120, 0))) g.playing = !g.playing;
    ImGui::SameLine();
    if (ImGui::Button("+1 min (->)")) advance(1);
    ImGui::SameLine();
    if (ImGui::Button("bar (Shift ->)")) finish_chart_bar();
    if (ImGui::Button("to 09:30 open (N)")) skip_to_open();
    ImGui::SameLine();
    if (ImGui::Button("random day")) random_day();
    ImGui::SetNextItemWidth(170);
    ImGui::SliderInt("##speed", &g.speed_idx, 0, IM_ARRAYSIZE(kSpeeds) - 1, "");
    ImGui::SameLine();
    ImGui::Text("%d min/s (Up/Down)", kSpeeds[g.speed_idx]);
    ImGui::Checkbox("skip overnight", &g.rth_only);
    ImGui::SameLine();
    ImGui::Checkbox("hide dates", &g.hide_dates);
    ImGui::SameLine();
    ImGui::Checkbox("09:30/16:00 lines", &g.sessions);
    static char date[32] = "2024-08-05 09:25";
    ImGui::SetNextItemWidth(170);
    ImGui::InputText("##date", date, sizeof date);
    ImGui::SameLine();
    if (ImGui::Button("start here")) {
        std::tm tm{};
        if (strptime(date, "%Y-%m-%d %H:%M", &tm)) {
            int64_t minute = static_cast<int64_t>(timegm(&tm)) / 60;
            new_session(g.m.index_before(minute), "new session");
        } else {
            g.status = "date format: YYYY-MM-DD HH:MM";
        }
    }
    ImGui::SetNextItemWidth(170);
    if (ImGui::Combo("timeframe", &g.tf_idx, kTfNames, IM_ARRAYSIZE(kTfNames))) g.follow = true;
    ImGui::SetNextItemWidth(170);
    ImGui::SliderInt("bars in view", &g.visible, 20, 1000);
    ImGui::SameLine();
    if (ImGui::SmallButton(g.follow ? "following" : "follow (L)")) g.follow = true;
}

void ticket_panel() {
    Broker& b = g.broker;
    ImGui::BeginDisabled(b.pos != 0);
    ImGui::SetNextItemWidth(90);
    if (ImGui::Combo("##inst", &g.instrument, "NQ\0MNQ\0")) {
        b.mult = g.instrument ? 2.0 : 20.0;
        b.commission = g.instrument ? 0.62 : 2.15;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90);
    ImGui::InputInt("qty", &g.qty);
    g.qty = std::clamp(g.qty, 1, 100);
    ImGui::SetNextItemWidth(90);
    ImGui::InputInt("slip ticks", &b.slip_ticks);
    b.slip_ticks = std::clamp(b.slip_ticks, 0, 20);
    ImGui::SameLine();
    float fee = static_cast<float>(b.commission);
    ImGui::SetNextItemWidth(70);
    if (ImGui::InputFloat("$/side", &fee, 0, 0, "%.2f")) b.commission = std::max(0.0f, fee);
    ImGui::Checkbox("limit fills on touch", &b.limit_touch);
    ImGui::Checkbox("bracket", &g.bracket);
    ImGui::SameLine();
    ImGui::BeginDisabled(!g.bracket);
    ImGui::SetNextItemWidth(70);
    ImGui::InputFloat("stop pts", &g.stop_pts, 0, 0, "%.2f");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70);
    ImGui::InputFloat("target pts", &g.target_pts, 0, 0, "%.2f");
    g.stop_pts = std::max(0.0f, g.stop_pts), g.target_pts = std::max(0.0f, g.target_pts);
    ImGui::EndDisabled();

    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.45f, 0.25f, 1));
    if (ImGui::Button("BUY MKT (B)", ImVec2(125, 30))) market_order(1, g.qty);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.18f, 0.16f, 1));
    if (ImGui::Button("SELL MKT (S)", ImVec2(125, 30))) market_order(-1, g.qty);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (ImGui::Button("FLATTEN (X)", ImVec2(110, 30))) flatten();
    if (ImGui::Button("reverse")) {
        if (b.pos) market_order(b.pos > 0 ? -1 : 1, 2 * std::abs(b.pos));
    }
    ImGui::SameLine();
    if (ImGui::Button("cancel orders (C)")) b.cancel_all();
    if (g.ticket_price == 0) g.ticket_price = g.last();
    ImGui::SetNextItemWidth(110);
    ImGui::InputDouble("##px", &g.ticket_price, kTick, 10 * kTick, "%.2f");
    ImGui::SameLine();
    if (ImGui::SmallButton("last")) g.ticket_price = g.last();
    auto pending = [&](int side, OrderType t) {
        Order o;
        o.side = side, o.type = t, o.qty = g.qty, o.price = round_tick(g.ticket_price), o.born = g.cursor;
        o.stop_pts = g.bracket ? g.stop_pts : 0, o.target_pts = g.bracket ? g.target_pts : 0;
        b.submit(o);
    };
    if (ImGui::SmallButton("buy lmt")) pending(1, OrderType::Limit);
    ImGui::SameLine();
    if (ImGui::SmallButton("sell lmt")) pending(-1, OrderType::Limit);
    ImGui::SameLine();
    if (ImGui::SmallButton("buy stp")) pending(1, OrderType::Stop);
    ImGui::SameLine();
    if (ImGui::SmallButton("sell stp")) pending(-1, OrderType::Stop);
    ImGui::TextDisabled("Market orders fill at the next minute's open: step or play.");
}

void position_panel() {
    Broker& b = g.broker;
    double lp = g.last(), u = b.unrealized(lp), net = b.realized - b.fees;
    ImGui::Text("position %s%d  avg %.2f  last %.2f", b.pos > 0 ? "+" : "", b.pos, b.avg, lp);
    ImGui::TextColored(u >= 0 ? kBuy : kSell, "open %+.0f", u);
    ImGui::SameLine();
    ImGui::TextColored(net >= 0 ? kBuy : kSell, "  closed %+.0f", net);
    ImGui::SameLine();
    ImGui::TextColored(net + u >= 0 ? kBuy : kSell, "  total %+.0f", net + u);
    ImGui::TextDisabled("closed P&L is after $%.2f fees", b.fees);
    if (!b.orders.empty() && ImGui::BeginTable("##orders", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        int cancel = 0;
        for (const Order& o : b.orders) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextColored(o.side > 0 ? kBuy : kSell, "%s", o.side > 0 ? "BUY" : "SELL");
            ImGui::TableNextColumn();
            ImGui::Text("%s%s", o.type == OrderType::Market ? "MKT" : o.type == OrderType::Limit ? "LMT" : "STP",
                        o.reduce_only ? " (bracket)" : "");
            ImGui::TableNextColumn();
            if (o.type == OrderType::Market) ImGui::Text("%d  next open", o.qty);
            else ImGui::Text("%d @ %.2f", o.qty, o.price);
            ImGui::TableNextColumn();
            ImGui::PushID(o.id);
            if (ImGui::SmallButton("x")) cancel = o.id;
            ImGui::PopID();
        }
        ImGui::EndTable();
        if (cancel) b.cancel(cancel);
    }
}

void indicator_panel() {
    static int add = 0;
    ImGui::SetNextItemWidth(170);
    ImGui::Combo("##add", &add, kIndNames, IND_COUNT);
    ImGui::SameLine();
    if (ImGui::Button("add")) g.inds.push_back(make_ind(add, g.next_uid++));
    int remove = -1;
    for (size_t k = 0; k < g.inds.size(); ++k) {
        Ind& d = g.inds[k];
        ImGui::PushID(d.uid);
        ImGui::Checkbox("##on", &d.on);
        ImGui::SameLine();
        ImGui::ColorEdit4("##col", &d.col.x, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
        ImGui::SameLine();
        ImGui::Text("%-14s", kIndNames[d.type]);
        auto num = [](const char* id, int* v, int lo, int hi) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(42);
            ImGui::DragInt(id, v, 0.2f, lo, hi);
        };
        switch (d.type) {
            case SMA: case EMA: case RSI: case ATR: num("##a", &d.a, 2, 500); break;
            case BOLL:
                num("##a", &d.a, 2, 500);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(42);
                ImGui::DragFloat("##k", &d.k, 0.05f, 0.5f, 4.0f, "%.1f");
                break;
            case PIVOTS:
                num("##l", &d.a, 1, 50);
                num("##r", &d.b, 1, 50);
                ImGui::SameLine();
                { bool lv = d.c != 0; if (ImGui::Checkbox("levels", &lv)) d.c = lv; }
                break;
            case MACD: num("##f", &d.a, 2, 100); num("##s", &d.b, 3, 200); num("##g", &d.c, 2, 100); break;
            default: break;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) remove = static_cast<int>(k);
        ImGui::PopID();
    }
    if (remove >= 0) g.inds.erase(g.inds.begin() + remove);
    if (g.inds.empty()) ImGui::TextDisabled("No indicators. Pick one and press add.");
}

void export_csv() {
    std::filesystem::create_directories(RT_SESSIONS);
    std::time_t now = std::time(nullptr);
    char name[64];
    std::strftime(name, sizeof name, "/session_%Y%m%d_%H%M%S.csv", std::localtime(&now));
    std::string path = std::string(RT_SESSIONS) + name;
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) { g.status = "could not write " + path; return; }
    std::fprintf(f, "entry_time,exit_time,side,qty,entry,exit,pnl_usd\n");
    for (const RoundTrip& r : g.broker.trips)
        std::fprintf(f, "%s,%s,%s,%d,%.2f,%.2f,%.2f\n", fmt_time(r.t_in, "%Y-%m-%d %H:%M").c_str(),
                     fmt_time(r.t_out, "%Y-%m-%d %H:%M").c_str(), r.side > 0 ? "long" : "short", r.qty, r.p_in, r.p_out,
                     r.pnl);
    std::fclose(f);
    g.status = "saved " + path;
}

void trades_panel() {
    const auto& tr = g.broker.trips;
    int wins = 0;
    double net = 0, gw = 0, gl = 0, peak = 0, dd = 0;
    std::vector<double> xs, eq;
    for (const RoundTrip& r : tr) {
        net += r.pnl, wins += r.pnl > 0;
        (r.pnl > 0 ? gw : gl) += std::fabs(r.pnl);
        peak = std::max(peak, net), dd = std::max(dd, peak - net);
        xs.push_back(static_cast<double>(xs.size() + 1)), eq.push_back(net);
    }
    ImGui::Text("%zu trades  win %.0f%%  net %+.0f  PF %.2f  max DD %.0f", tr.size(),
                tr.empty() ? 0.0 : 100.0 * wins / tr.size(), net, gl > 0 ? gw / gl : 0.0, dd);
    if (!tr.empty() && ImPlot::BeginPlot("##eq", ImVec2(-1, 110), ImPlotFlags_NoLegend | ImPlotFlags_NoMouseText)) {
        ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_NoTickLabels, ImPlotAxisFlags_AutoFit);
        xs.insert(xs.begin(), 0), eq.insert(eq.begin(), 0);
        ImPlot::PlotLine("equity", xs.data(), eq.data(), static_cast<int>(xs.size()));
        ImPlot::EndPlot();
    }
    if (ImGui::BeginTable("##trips", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit,
                          ImVec2(0, 160))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        for (const char* h : {"in", "side", "qty", "in/out", "$"}) ImGui::TableSetupColumn(h);
        ImGui::TableHeadersRow();
        for (auto it = tr.rbegin(); it != tr.rend(); ++it) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(fmt_time(it->t_in, g.hide_dates ? "%H:%M" : "%m-%d %H:%M").c_str());
            ImGui::TableNextColumn();
            ImGui::TextColored(it->side > 0 ? kBuy : kSell, "%s", it->side > 0 ? "long" : "short");
            ImGui::TableNextColumn();
            ImGui::Text("%d", it->qty);
            ImGui::TableNextColumn();
            ImGui::Text("%.2f / %.2f", it->p_in, it->p_out);
            ImGui::TableNextColumn();
            ImGui::TextColored(it->pnl > 0 ? kBuy : kSell, "%+.0f", it->pnl);
        }
        ImGui::EndTable();
    }
    if (ImGui::Button("export CSV")) export_csv();
    ImGui::SameLine();
    if (ImGui::Button("reset P&L")) {
        size_t c = g.cursor;
        new_session(c, "P&L reset");
    }
}

void handle_keys(float dt) {
    ImGuiIO& io = ImGui::GetIO();
    if (!io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) g.playing = !g.playing;
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) io.KeyShift ? finish_chart_bar() : advance(1);
        if (ImGui::IsKeyPressed(ImGuiKey_N, false)) skip_to_open();
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) g.speed_idx = std::min(g.speed_idx + 1, IM_ARRAYSIZE(kSpeeds) - 1);
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) g.speed_idx = std::max(g.speed_idx - 1, 0);
        if (ImGui::IsKeyPressed(ImGuiKey_B, false)) market_order(1, g.qty);
        if (ImGui::IsKeyPressed(ImGuiKey_S, false)) market_order(-1, g.qty);
        if (ImGui::IsKeyPressed(ImGuiKey_X, false)) flatten();
        if (ImGui::IsKeyPressed(ImGuiKey_C, false)) g.broker.cancel_all();
        if (ImGui::IsKeyPressed(ImGuiKey_L, false)) g.follow = true;
    }
    if (g.playing) {
        g.accum += dt * kSpeeds[g.speed_idx];
        size_t n = static_cast<size_t>(g.accum);
        g.accum -= static_cast<double>(n);
        for (size_t k = 0; k < n && g.playing; ++k) {
            if (g.rth_only && g.cursor + 1 < g.m.size() && !in_rth(g.m.t[g.cursor + 1])) skip_to_open();
            advance(1);
        }
    } else {
        g.accum = 0;
    }
}

void save_bmp(const char* path, int w, int h) {
    std::vector<unsigned char> px(static_cast<size_t>(w) * h * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_BGR, GL_UNSIGNED_BYTE, px.data());
    int row = (w * 3 + 3) & ~3, size = 54 + row * h;
    unsigned char hdr[54] = {'B', 'M'};
    auto put = [&](int off, int v) { std::memcpy(hdr + off, &v, 4); };
    put(2, size), put(10, 54), put(14, 40), put(18, w), put(22, h), put(34, row * h);
    hdr[26] = 1, hdr[28] = 24;
    FILE* f = std::fopen(path, "wb");
    std::fwrite(hdr, 1, 54, f);
    std::vector<char> pad(row - w * 3, 0);
    for (int y = 0; y < h; ++y) {
        std::fwrite(px.data() + static_cast<size_t>(y) * w * 3, 1, w * 3, f);
        std::fwrite(pad.data(), 1, pad.size(), f);
    }
    std::fclose(f);
}

}  // namespace

int main(int argc, char** argv) {
    const char* shot = nullptr;
    const char* start = nullptr;
    bool demo = false;
    for (int a = 1; a < argc; ++a) {
        if (!std::strcmp(argv[a], "--screenshot") && a + 1 < argc) shot = argv[++a];
        else if (!std::strcmp(argv[a], "--start") && a + 1 < argc) start = argv[++a];
        else if (!std::strcmp(argv[a], "--demo")) demo = true;
    }
    std::string err;
    if (!g.m.load(RT_DATA, err)) {
        std::fprintf(stderr, "%s\nRun: python3 export_bars.py\n", err.c_str());
        return 1;
    }
    for (size_t i = 0; i < g.m.size(); ++i)
        if (((g.m.t[i] % 1440) + 1440) % 1440 == 570) g.opens.push_back(i);
    if (start) {
        std::tm tm{};
        strptime(start, "%Y-%m-%d %H:%M", &tm);
        new_session(g.m.index_before(static_cast<int64_t>(timegm(&tm)) / 60), "new session");
    } else {
        random_day();
    }
    if (demo) {  // a quick tour: a few indicators and a bracketed trade
        for (int t : {EMA, BOLL, PIVOTS, VWAP, RSI, MACD}) g.inds.push_back(make_ind(t, g.next_uid++));
        g.tf_idx = 1;
        advance(70);
        g.bracket = true;
        market_order(1, 1);
        advance(25);
    }

    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    GLFWwindow* win = glfwCreateWindow(1600, 950, "NQ replay trader", nullptr, nullptr);
    if (!win) return 1;
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    float scale = 1.0f;
    glfwGetWindowContentScale(win, &scale, nullptr);
    if (FILE* f = std::fopen(RT_FONT, "rb")) {
        std::fclose(f);
        io.Fonts->AddFontFromFileTTF(RT_FONT, 14.0f * scale);
        io.FontGlobalScale = 1.0f / scale;
    }
    ImGui::StyleColorsDark();
    ImPlot::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(win, true);
    ImGui_ImplOpenGL3_Init("#version 150");

    double t_prev = glfwGetTime();
    int frame = 0;
    while (!glfwWindowShouldClose(win)) {
        glfwPollEvents();
        double t_now = glfwGetTime();
        float dt = static_cast<float>(t_now - t_prev);
        t_prev = t_now;
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        handle_keys(dt);

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("##main", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                            ImGuiWindowFlags_NoBringToFrontOnFocus);
        ImGui::BeginChild("##left", ImVec2(410, 0), ImGuiChildFlags_Border);
        if (ImGui::CollapsingHeader("Replay", ImGuiTreeNodeFlags_DefaultOpen)) replay_panel();
        if (ImGui::CollapsingHeader("Order ticket", ImGuiTreeNodeFlags_DefaultOpen)) ticket_panel();
        if (ImGui::CollapsingHeader("Position & orders", ImGuiTreeNodeFlags_DefaultOpen)) position_panel();
        if (ImGui::CollapsingHeader("Indicators", ImGuiTreeNodeFlags_DefaultOpen)) indicator_panel();
        if (ImGui::CollapsingHeader("Trades", ImGuiTreeNodeFlags_DefaultOpen)) trades_panel();
        for (const std::string& l : g.broker.log) ImGui::TextDisabled("%s", l.c_str());
        if (!g.status.empty()) ImGui::TextDisabled("%s", g.status.c_str());
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("##right");
        draw_charts();
        ImGui::EndChild();
        ImGui::End();

        ImGui::Render();
        int w, h;
        glfwGetFramebufferSize(win, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.08f, 0.08f, 0.09f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        if (shot && ++frame == 6) {
            save_bmp(shot, w, h);
            break;
        }
        glfwSwapBuffers(win);
    }
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    glfwDestroyWindow(win);
    glfwTerminate();
    return 0;
}
