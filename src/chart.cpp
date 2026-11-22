// Chart drawing and interaction, replacing ImPlot: panes, axes, candles, indicators, trading overlays, hover
// tooltip, pan and zoom, order placement and dragging. Everything is in raster pixels.

#include "chart.h"
#include "datafeed.h"

#include <algorithm>
#include <climits>
#include <map>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "BUI_kit.h"
#include "GPU_raster.h"
#include "app.h"
#include "script.h"
#include "ui.h"

namespace {

const char* g_family = nullptr;  // chart font family, nullptr = the UI font

struct Box {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool in(float x, float y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
    float w() const { return x1 - x0; }
    float h() const { return y1 - y0; }
};

float ui_scale() { return BK_unit() / 20.0f; }

Rgba theme(const char* key, Rgba fallback) {
    float c[4];
    return BK_theme_color(key, c) ? Rgba{c[0], c[1], c[2], c[3]} : fallback;
}

// ---------------------------------------------------------------- drawing helpers over BuiRaster
struct Painter {
    BuiRaster* r;
    float ox, oy;  // node origin in the raster

    void fill(float x, float y, float w, float h, Rgba c) const {
        float col[4] = {c.r, c.g, c.b, c.a};
        BUI_raster_fill_rect(r, ox + x, oy + y, w, h, col);
    }
    // Rectangle clipped to `b`.
    void rect(float x0, float y0, float x1, float y1, Rgba c, const Box& b) const {
        if (x0 > x1) std::swap(x0, x1);
        if (y0 > y1) std::swap(y0, y1);
        x0 = std::max(x0, b.x0), x1 = std::min(x1, b.x1), y0 = std::max(y0, b.y0), y1 = std::min(y1, b.y1);
        if (x1 > x0 && y1 > y0) fill(x0, y0, x1 - x0, y1 - y0, c);
    }
    // Anti-aliased line clipped to `b` (Liang-Barsky).
    void line(float x0, float y0, float x1, float y1, float w, Rgba c, const Box& b) const {
        float t0 = 0, t1 = 1, dx = x1 - x0, dy = y1 - y0;
        auto edge = [&](float p, float q) {
            if (p == 0) return q >= 0;
            float t = q / p;
            if (p < 0) {
                if (t > t1) return false;
                t0 = std::max(t0, t);
            } else {
                if (t < t0) return false;
                t1 = std::min(t1, t);
            }
            return true;
        };
        if (!edge(-dx, x0 - b.x0) || !edge(dx, b.x1 - x0) || !edge(-dy, y0 - b.y0) || !edge(dy, b.y1 - y0)) return;
        float col[4] = {c.r, c.g, c.b, c.a};
        BUI_raster_line(r, ox + x0 + t0 * dx, oy + y0 + t0 * dy, ox + x0 + t1 * dx, oy + y0 + t1 * dy, w, col);
    }
    void dashed_v(float x, float y0, float y1, Rgba c, const Box& b, float dash, float gap) const {
        for (float y = y0; y < y1; y += dash + gap) line(x, y, x, std::min(y + dash, y1), 1, c, b);
    }
    void dashed_h(float y, float x0, float x1, Rgba c, const Box& b, float dash, float gap) const {
        for (float x = x0; x < x1; x += dash + gap) line(x, y, std::min(x + dash, x1), y, 1, c, b);
    }
    void tri(float ax, float ay, float bx, float by, float cx, float cy, Rgba c) const {
        float p[3][2] = {{ox + ax, oy + ay}, {ox + bx, oy + by}, {ox + cx, oy + cy}};
        float col[4] = {c.r, c.g, c.b, c.a};
        BUI_raster_polygon(r, p, 3, col);
    }
    void round_rect(float x, float y, float w, float h, float rad, Rgba c) const {
        float col[4] = {c.r, c.g, c.b, c.a};
        BUI_raster_round_rect(r, ox + x, oy + y, w, h, rad, col);
    }
    // `y` is the text's vertical centre.
    void text(float x, float y, const std::string& s, float size, Rgba c) const {
        if (s.empty()) return;
        float col[4] = {c.r, c.g, c.b, c.a};
        BUI_raster_set_font(r, g_family, 0);
        BUI_raster_text(r, ox + x, oy + y + size * 0.36f, s.c_str(), static_cast<int>(s.size()), size, col);
        BUI_raster_set_font(r, nullptr, 0);
    }
    // Triangle mesh (the Bollinger fill), scissored to `b`.
    void mesh(const std::vector<BuiMeshVertex>& v, const std::vector<uint32_t>& idx, const Box& b) const {
        if (idx.empty()) return;
        float clip[4] = {b.x0, b.y0, b.x1, b.y1};
        BUI_raster_mesh(r, v.data(), static_cast<int>(v.size()), idx.data(), static_cast<int>(idx.size()),
                        BUI_MESH_INDEX32, 0, clip, ox, oy);
    }
};

float text_w(const std::string& s, float size) {
    float w = 0;
    if (!s.empty() && BUI_text_measure(g_family, 0, size, s.c_str(), static_cast<int>(s.size()), &w)) return w;
    return size * 0.6f * s.size();
}

uint32_t pack(Rgba c) {
    auto b = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return b(c.r) | b(c.g) << 8 | b(c.b) << 16 | b(c.a) << 24;
}

// ---------------------------------------------------------------- model: computed once per frame in chart_prepare
using Vec = std::vector<double>;

// An indicator's output for the current series, with what's needed to draw it.
struct IndResult {
    int uid = 0;
    const ScriptDef* def = nullptr;
    std::string label;
    Rgba col{1, 1, 1, 1};
    IndOutput out;
    Rgba plot_color(size_t k) const {
        const PlotDef& p = def->plots[k];
        Rgba c = p.has_color ? p.color : col;
        c.a *= p.alpha;
        return c;
    }
};

struct PaneModel {
    bool price = true;
    int result = -1;                 // oscillator panes: index into Model::results
    double ymin = 0, ymax = 1;
};

struct Model {
    std::vector<IndResult> results;
    std::vector<int> overlays;       // results drawn on the price pane
    std::vector<PaneModel> panes;
    size_t n = 0;                    // series length the results were computed for
} M;

// Indicator results are recomputed only when the series, the params or the scripts change.
struct CachedResult {
    long series = -1;
    int gen = -1;
    std::string script;
    Params params;
    IndOutput out;
};
std::map<int, CachedResult> g_cache;   // by indicator uid
std::map<int, std::string> g_errors;   // last error reported per indicator

void fit(double& lo, double& hi, std::initializer_list<const Vec*> ys, double pad_frac) {
    long j0, j1;
    visible_range(j0, j1);
    lo = INFINITY, hi = -INFINITY;
    for (auto* y : ys)
        for (long j = j0; j <= j1 && j < static_cast<long>(y->size()); ++j)
            if (std::isfinite((*y)[j])) lo = std::min(lo, (*y)[j]), hi = std::max(hi, (*y)[j]);
    if (!std::isfinite(lo)) {
        lo = 0, hi = 1;
        return;
    }
    double pad = std::max((hi - lo) * pad_frac, 1e-9);
    lo -= pad, hi += pad;
}

// ---------------------------------------------------------------- layout (local pixels)
struct Pane {
    Box box;
    const PaneModel* pm;
    double ymin, ymax;
    float py(double y) const { return box.y1 - static_cast<float>((y - ymin) / (ymax - ymin)) * box.h(); }
    double value(float py) const { return ymin + (box.y1 - py) / box.h() * (ymax - ymin); }
};

struct Layout {
    float s, w, h;
    Box plot;                        // union of the panes' x span, full height
    float axis_x;                    // left edge of the price axis
    std::vector<Pane> panes;
    float px(double x) const { return plot.x0 + static_cast<float>((x - g.xmin) / (g.xmax - g.xmin)) * plot.w(); }
    double xval(float px) const { return g.xmin + (px - plot.x0) / plot.w() * (g.xmax - g.xmin); }
    double ppb() const { return plot.w() / (g.xmax - g.xmin); }  // pixels per bar
    const Pane* pane_at(float x, float y) const {
        for (const Pane& p : panes)
            if (p.box.in(x, y)) return &p;
        return nullptr;
    }
};

Layout layout(float w, float h) {
    Layout L;
    L.s = ui_scale(), L.w = w, L.h = h;
    float s = L.s, pad = 6 * s, axis_w = 78 * s, xlab = 22 * s, gap = 8 * s;
    L.plot = {pad, pad, std::max(pad + 10, w - axis_w), std::max(pad + 10, h - xlab)};
    L.axis_x = L.plot.x1;
    float total = 0;
    for (const PaneModel& pm : M.panes) total += pm.price ? 5.0f : 1.3f;
    float avail = L.plot.h() - gap * std::max<size_t>(0, M.panes.size() - 1), y = L.plot.y0;
    for (const PaneModel& pm : M.panes) {
        float ph = avail * (pm.price ? 5.0f : 1.3f) / total;
        L.panes.push_back({{L.plot.x0, y, L.plot.x1, y + ph}, &pm, pm.ymin, pm.ymax});
        y += ph + gap;
    }
    return L;
}

double nice_step(double range, double n) {
    double raw = range / std::max(1.0, n), mag = std::pow(10.0, std::floor(std::log10(raw))), f = raw / mag;
    return (f < 1.5 ? 1 : f < 3 ? 2 : f < 7 ? 5 : 10) * mag;
}

std::string fmt(const char* f, double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, f, v);
    return buf;
}

std::string axis_number(double v, double step) {
    if (std::fabs(v) >= 1e6) return fmt("%.1fM", v / 1e6);
    if (std::fabs(v) >= 1e4 && step >= 1000) return fmt("%.0fk", v / 1e3);
    return fmt(step >= 1 ? "%.0f" : step >= 0.1 ? "%.1f" : "%.2f", v);
}

std::string bar_time(long j, const char* f_daily_dates, const char* f_intraday_dates) {
    const char* f = g.tf() >= 1440 ? (g.hide_dates ? "%a" : f_daily_dates) : (g.hide_dates ? "%H:%M" : f_intraday_dates);
    return fmt_time(static_cast<int64_t>(g.s.t[j]), f);
}

// ---------------------------------------------------------------- chart interaction state
struct ChartState {
    BKNode* node = nullptr;
    bool hover = false;
    float mx = 0, my = 0;
    enum { NONE, PAN, ORDER } drag = NONE;
    int order_id = 0;
    float press_x = 0;
    double xmin0 = 0, xmax0 = 0;
} C;

const float kFont = 11.0f;

// ---------------------------------------------------------------- drawing
void draw_series_line(const Painter& P, const Layout& L, const Pane& pn, const Vec& y, Rgba c, float w) {
    long j0, j1;
    visible_range(j0, j1);
    j0 = std::max(0L, j0 - 1), j1 = std::min(static_cast<long>(y.size()) - 1, j1 + 1);
    long step = std::max(1L, static_cast<long>(1.0 / std::max(1e-9, L.ppb())));  // about one point per pixel
    bool have = false;
    float px0 = 0, py0 = 0;
    for (long j = j0; j <= j1; j += step) {
        if (!std::isfinite(y[j])) {
            have = false;
            continue;
        }
        float px = L.px(static_cast<double>(g.s.x0 + j)), py = pn.py(y[j]);
        if (have) P.line(px0, py0, px, py, w * L.s, c, pn.box);
        px0 = px, py0 = py, have = true;
    }
}

// Histogram bars from zero; by_bar colours each by its candle's direction (alpha from c).
void draw_bars(const Painter& P, const Layout& L, const Pane& pn, const Vec& y, Rgba c, bool by_bar = false) {
    long j0, j1;
    visible_range(j0, j1);
    float half = std::max(0.5f, static_cast<float>(L.ppb() * 0.35)), base = pn.py(std::clamp(0.0, pn.ymin, pn.ymax));
    for (long j = j0; j <= j1 && j < static_cast<long>(y.size()); ++j)
        if (std::isfinite(y[j])) {
            float px = L.px(static_cast<double>(g.s.x0 + j));
            Rgba k = by_bar ? (g.s.c[j] >= g.s.o[j] ? kUp : kDown).alpha(c.a) : c;
            P.rect(px - half, base, px + half, pn.py(y[j]), k, pn.box);
        }
}

// Horizontal steps: each bar's value across its own width (levels that change from day to day).
void draw_steps(const Painter& P, const Layout& L, const Pane& pn, const Vec& y, Rgba c, float w) {
    long j0, j1;
    visible_range(j0, j1);
    for (long j = std::max(0L, j0 - 1); j <= j1 + 1 && j < static_cast<long>(y.size()); ++j) {
        if (!std::isfinite(y[j])) continue;
        long k = j;  // extend over the run of equal values
        while (k + 1 < static_cast<long>(y.size()) && k + 1 <= j1 + 1 && y[k + 1] == y[j]) ++k;
        float py = pn.py(y[j]);
        P.line(L.px(g.s.x0 + j - 0.5), py, L.px(g.s.x0 + k + 0.5), py, w * L.s, c, pn.box);
        j = k;
    }
}

void draw_candles(const Painter& P, const Layout& L, const Pane& pn) {
    const Series& s = g.s;
    long j0, j1;
    visible_range(j0, j1);
    if (j1 < j0) return;
    float ppb = static_cast<float>(L.ppb());
    if (ppb < 2.0f) {
        draw_series_line(P, L, pn, s.c, {0.85f, 0.85f, 0.85f, 1}, 1.0f);
        return;
    }
    float half = std::max(1.0f, ppb * 0.36f), wick = std::max(1.0f, std::floor(L.s));
    for (long j = j0; j <= j1; ++j) {
        Rgba c = s.c[j] >= s.o[j] ? kUp : kDown;
        if (j == static_cast<long>(s.n()) - 1 && s.last_partial) c.a = 0.69f;  // forming
        float x = L.px(static_cast<double>(s.x0 + j));
        P.rect(x - wick / 2, pn.py(s.h[j]), x + wick / 2, pn.py(s.l[j]), c, pn.box);
        float top = std::min(pn.py(s.o[j]), pn.py(s.c[j])), bot = std::max(pn.py(s.o[j]), pn.py(s.c[j]));
        if (bot - top < 1.0f) bot = top + 1.0f;
        P.rect(x - half, top, x + half, bot, c, pn.box);
    }
}

// Dashed lines at 09:30 and 16:00 on intraday charts, placed inside the bar that contains them.
void draw_sessions(const Painter& P, const Layout& L, const Pane& pn) {
    if (!g.sessions || g.tf() >= 1440) return;
    long j0, j1;
    visible_range(j0, j1);
    if (j1 - j0 > 3000) return;
    for (long j = j0; j <= j1; ++j) {
        int64_t t = static_cast<int64_t>(g.s.t[j]), tod = ((t % 1440) + 1440) % 1440;
        for (int mark : {570, 960}) {
            if (tod > mark || tod + g.tf() <= mark) continue;
            float x = L.px(g.s.x0 + j - 0.5 + static_cast<double>(mark - tod) / g.tf());
            Rgba c = mark == 570 ? Rgba{0.45f, 0.85f, 0.55f, 0.5f} : Rgba{0.95f, 0.55f, 0.45f, 0.5f};
            P.dashed_v(x, pn.box.y0, pn.box.y1, c, pn.box, 4 * L.s, 4 * L.s);
        }
    }
}

void draw_grid(const Painter& P, const Layout& L, const Pane& pn, Rgba grid, Rgba text, std::vector<double>* xticks) {
    double step = nice_step(pn.ymax - pn.ymin, pn.box.h() / (38 * L.s));
    for (double v = std::ceil(pn.ymin / step) * step; v <= pn.ymax; v += step) {
        float y = pn.py(v);
        P.line(pn.box.x0, y, pn.box.x1, y, 1, grid, pn.box);
        if (y > pn.box.y0 + 6 * L.s && y < pn.box.y1 - 6 * L.s)
            P.text(L.axis_x + 6 * L.s, y, axis_number(v, step), kFont * L.s, text);
    }
    if (xticks)
        for (double x : *xticks) {
            float px = L.px(x);
            P.line(px, pn.box.y0, px, pn.box.y1, 1, grid, pn.box);
        }
}

// Bar positions for the time axis, at round clock times (intraday) or every few sessions (daily), about 110 px
// apart: a bar gets a tick when it starts a new period of `step` bars' worth of time.
std::vector<double> x_ticks(const Layout& L) {
    std::vector<double> out;
    // intraday: minutes of clock time; daily: bars
    static const long minutes[] = {1, 2, 5, 10, 15, 30, 60, 120, 240, 480, 1440, 2880, 7200, 14400, 28800, 57600, 115200};
    static const long days[] = {1, 2, 5, 10, 20, 40, 60, 120, 250, 500, 1000, 2500};
    bool daily = g.tf() >= 1440;
    double need = 110 * L.s / std::max(1e-9, L.ppb()) * (daily ? 1 : g.tf());
    long period = 0;
    for (long p : daily ? std::vector<long>(std::begin(days), std::end(days)) : std::vector<long>(std::begin(minutes), std::end(minutes)))
        if ((period = p) >= need && (daily || p % g.tf() == 0)) break;
    long step = period;
    long j0, j1;
    visible_range(j0, j1);
    long prev = LONG_MIN;
    float last_px = -1e9f;
    for (long j = std::max(0L, j0 - 1); j <= j1; ++j) {
        long t = static_cast<long>(g.s.t[j]), key = daily ? (g.s.x0 + j) / step : (t - (t % period + period) % period) / period;
        bool tick = key != prev && prev != LONG_MIN;
        prev = key;
        float px = L.px(static_cast<double>(g.s.x0 + j));
        if (tick && j >= j0 && px - last_px >= 70 * L.s) out.push_back(static_cast<double>(g.s.x0 + j)), last_px = px;
    }
    return out;
}

void tag(const Painter& P, const Layout& L, const Pane& pn, double v, Rgba bg, const std::string& s) {
    float y = pn.py(v), hh = 8 * L.s;
    if (y < pn.box.y0 || y > pn.box.y1) return;
    float w = std::max(L.w - L.axis_x - 3 * L.s, text_w(s, kFont * L.s) + 10 * L.s);
    P.round_rect(L.axis_x + 1 * L.s, y - hh, w, 2 * hh, 2 * L.s, bg);
    float lum = 0.3f * bg.r + 0.59f * bg.g + 0.11f * bg.b;
    P.text(L.axis_x + 5 * L.s, y, s, kFont * L.s, lum > 0.5f ? Rgba{0.05f, 0.05f, 0.05f, 1} : Rgba{1, 1, 1, 1});
}

// One indicator's output: fills, then plots in order, then level segments and markers.
void draw_result(const Painter& P, const Layout& L, const Pane& pn, const IndResult& r) {
    if (!r.def || !r.out.error.empty()) return;
    const Series& s = g.s;
    long j0, j1;
    visible_range(j0, j1);
    if (!pn.pm->price)
        for (double lv : r.def->levels) P.line(pn.box.x0, pn.py(lv), pn.box.x1, pn.py(lv), 1, {1, 1, 1, 0.3f}, pn.box);
    for (const FillDef& f : r.def->fills) {
        const Vec &a = r.out.plots[f.a], &b = r.out.plots[f.b];
        std::vector<BuiMeshVertex> v;
        std::vector<uint32_t> idx;
        uint32_t col = pack(r.plot_color(f.a).alpha(f.alpha));
        for (long j = std::max(0L, j0 - 1); j <= j1 && j + 1 < static_cast<long>(a.size()); ++j) {
            if (!std::isfinite(a[j]) || !std::isfinite(a[j + 1]) || !std::isfinite(b[j]) || !std::isfinite(b[j + 1])) continue;
            float xa = L.px(s.x0 + j), xb = L.px(s.x0 + j + 1);
            uint32_t k = static_cast<uint32_t>(v.size());
            v.push_back({xa, pn.py(a[j]), 0, 0, col});
            v.push_back({xa, pn.py(b[j]), 0, 0, col});
            v.push_back({xb, pn.py(a[j + 1]), 0, 0, col});
            v.push_back({xb, pn.py(b[j + 1]), 0, 0, col});
            for (uint32_t t : {0u, 1u, 2u, 2u, 1u, 3u}) idx.push_back(k + t);
        }
        P.mesh(v, idx, pn.box);
    }
    for (size_t k = 0; k < r.def->plots.size(); ++k) {
        const PlotDef& p = r.def->plots[k];
        Rgba c = r.plot_color(k);
        switch (p.style) {
            case PlotDef::Line: draw_series_line(P, L, pn, r.out.plots[k], c, p.width); break;
            case PlotDef::Step: draw_steps(P, L, pn, r.out.plots[k], c, p.width); break;
            case PlotDef::Histogram: draw_bars(P, L, pn, r.out.plots[k], c, p.color_by_bar); break;
            case PlotDef::Markers: break;
        }
    }
    for (const IndSegment& sg : r.out.segments) {
        if (sg.i1 < j0 || sg.i0 > j1) continue;
        float y = pn.py(sg.price);
        Rgba c = r.plot_color(static_cast<size_t>(sg.plot));
        c.a *= sg.alpha;
        P.line(L.px(static_cast<double>(s.x0 + sg.i0)), y, L.px(static_cast<double>(s.x0 + sg.i1)), y, 1, c, pn.box);
    }
    float sz = 4 * L.s, off = 4 * L.s;
    for (const IndMarker& mk : r.out.markers) {
        if (mk.i < j0 || mk.i > j1) continue;
        float x = L.px(static_cast<double>(s.x0 + mk.i)), y = pn.py(mk.price);
        if (!pn.box.in(x, y)) continue;
        Rgba c = r.plot_color(static_cast<size_t>(mk.plot));
        if (mk.up) P.tri(x, y + off, x - sz, y + off + 7 * L.s, x + sz, y + off + 7 * L.s, c);
        else P.tri(x, y - off, x - sz, y - off - 7 * L.s, x + sz, y - off - 7 * L.s, c);
    }
}

// Fills, position, working orders and last price on the price pane.
void draw_trading(const Painter& P, const Layout& L, const Pane& pn) {
    const Broker& b = g.broker;
    const Agg& a = g.agg();
    for (const Fill& f : b.fills) {
        double x = static_cast<double>(a.bucket_of(g.m.index_before(f.time + 1)));
        float px = L.px(x), py = pn.py(f.price), s = L.s;
        if (!pn.box.in(px, py)) continue;
        if (f.side > 0) P.tri(px, py, px - 6 * s, py + 10 * s, px + 6 * s, py + 10 * s, kBuy);
        else P.tri(px, py, px - 6 * s, py - 10 * s, px + 6 * s, py - 10 * s, kSell);
    }
    double lp = g.last();
    P.dashed_h(pn.py(lp), pn.box.x0, pn.box.x1, {0.8f, 0.8f, 0.8f, 0.35f}, pn.box, 2 * L.s, 3 * L.s);
    tag(P, L, pn, lp, {0.8f, 0.8f, 0.8f, 1}, fmt("%.2f", lp));
    if (b.pos) {
        Rgba c = b.pos > 0 ? kBuy : kSell;
        float y = pn.py(b.avg);
        P.line(pn.box.x0, y, pn.box.x1, y, 1.5f * L.s, c.alpha(0.8f), pn.box);
        double u = b.unrealized(lp);
        char buf[64];
        std::snprintf(buf, sizeof buf, "%s %d %s$%.0f", b.pos > 0 ? "LONG" : "SHORT", std::abs(b.pos), u < 0 ? "-" : "+",
                      std::fabs(u));
        tag(P, L, pn, b.avg, c, buf);
    }
    for (const Order& o : b.orders) {
        if (o.type == OrderType::Market) continue;
        Rgba c = (o.side > 0 ? kBuy : kSell).alpha(o.type == OrderType::Stop ? 0.75f : 1.0f);
        float y = pn.py(o.price);
        bool active = C.drag == ChartState::ORDER && C.order_id == o.id;
        if (o.type == OrderType::Stop) P.dashed_h(y, pn.box.x0, pn.box.x1, c, pn.box, 6 * L.s, 4 * L.s);
        else P.line(pn.box.x0, y, pn.box.x1, y, (active ? 2.4f : 1.6f) * L.s, c, pn.box);
        char buf[64];
        std::snprintf(buf, sizeof buf, "%s %s %d", o.side > 0 ? "BUY" : "SELL", o.type == OrderType::Stop ? "STP" : "LMT",
                      o.qty);
        tag(P, L, pn, o.price, c, buf);
    }
}

void draw_legend(const Painter& P, const Layout& L, const Pane& pn, Rgba text) {
    float s = L.s, x = pn.box.x0 + 8 * s, y = pn.box.y0 + 10 * s, size = kFont * s;
    auto entry = [&](const std::string& t, Rgba c) {
        P.fill(x, y - 1.5f * s, 10 * s, 3 * s, c);
        P.text(x + 14 * s, y, t, size, text);
        y += 15 * s;
    };
    auto result_entries = [&](const IndResult& r, bool all_plots) {
        if (!r.out.error.empty()) {
            entry(r.label + "  (script error, see log)", kSell);
            return;
        }
        entry(r.label, r.col);
        if (all_plots && r.def)  // oscillators: name the plots that have colours of their own
            for (size_t k = 0; k < r.def->plots.size(); ++k)
                if (r.def->plots[k].has_color && r.def->plots[k].style == PlotDef::Line) entry(r.def->plots[k].name, r.plot_color(k));
    };
    if (pn.pm->price) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "NQ %s", kTfNames[g.tf_idx]);
        P.text(x, y, buf, size * 1.1f, text);
        y += 16 * s;
        for (int k : M.overlays) result_entries(M.results[k], false);
    } else if (pn.pm->result >= 0) {
        result_entries(M.results[pn.pm->result], true);
    }
}

void draw_hover(const Painter& P, const Layout& L, Rgba text) {
    if (!C.hover || C.drag == ChartState::PAN) return;
    const Pane* pn = L.pane_at(C.mx, C.my);
    if (!pn) return;
    float s = L.s;
    long j = std::lround(L.xval(C.mx)) - static_cast<long>(g.s.x0);
    Rgba cross{1, 1, 1, 0.25f};
    for (const Pane& p : L.panes) P.dashed_v(C.mx, p.box.y0, p.box.y1, cross, p.box, 3 * s, 3 * s);
    P.dashed_h(C.my, pn->box.x0, pn->box.x1, cross, pn->box, 3 * s, 3 * s);
    double v = pn->value(C.my);
    tag(P, L, *pn, v, {0.25f, 0.27f, 0.32f, 1}, pn->pm->price ? fmt("%.2f", round_tick(v)) : fmt("%.2f", v));
    if (j < 0 || j >= static_cast<long>(g.s.n())) return;

    const Series& sr = g.s;
    std::string t = bar_time(j, "%Y-%m-%d", "%a %Y-%m-%d %H:%M");
    float tw = text_w(t, kFont * s) + 12 * s, tx = std::clamp(L.px(sr.x0 + j) - tw / 2, L.plot.x0, L.plot.x1 - tw);
    P.round_rect(tx, L.plot.y1 + 3 * s, tw, 16 * s, 2 * s, {0.25f, 0.27f, 0.32f, 1});
    P.text(tx + 6 * s, L.plot.y1 + 11 * s, t, kFont * s, {1, 1, 1, 1});

    std::vector<std::pair<std::string, Rgba>> lines;
    lines.push_back({t + (j == static_cast<long>(sr.n()) - 1 && sr.last_partial ? "  (forming)" : ""), text});
    char buf[128];
    std::snprintf(buf, sizeof buf, "O %.2f  H %.2f  L %.2f  C %.2f", sr.o[j], sr.h[j], sr.l[j], sr.c[j]);
    lines.push_back({buf, text});
    std::snprintf(buf, sizeof buf, "V %.0f   %+.2f", sr.v[j], sr.c[j] - sr.o[j]);
    lines.push_back({buf, sr.c[j] >= sr.o[j] ? kUp : kDown});
    if (pn->pm->price) lines.push_back({"Cmd+click limit   Shift+click stop", text.alpha(0.5f)});
    float bw = 0, lh = 15 * s, size = kFont * s;
    for (auto& l : lines) bw = std::max(bw, text_w(l.first, size));
    bw += 16 * s;
    float bh = lines.size() * lh + 10 * s;
    float bx = C.mx + 16 * s, by = C.my + 16 * s;
    if (bx + bw > L.plot.x1) bx = C.mx - 16 * s - bw;
    if (by + bh > L.h) by = C.my - 16 * s - bh;
    Rgba bg = theme("tooltip.inner", {0.1f, 0.1f, 0.1f, 1});
    bg.a = 0.95f;
    P.round_rect(bx, by, bw, bh, 4 * s, bg);
    float y = by + 5 * s + lh / 2;
    for (auto& l : lines) P.text(bx + 8 * s, y, l.first, size, l.second), y += lh;
}

void chart_draw(void*, BKNode*, BuiRaster* r, const int rect[4]) {
    Painter P{r, static_cast<float>(rect[0]), static_cast<float>(rect[1])};
    Layout L = layout(static_cast<float>(rect[2]), static_cast<float>(rect[3]));
    Rgba region = theme("region_bg", {0.19f, 0.19f, 0.19f, 1}), text = theme("text", {0.9f, 0.9f, 0.9f, 1});
    Rgba plot_bg{region.r * 0.72f, region.g * 0.72f, region.b * 0.72f, 1}, grid = text.alpha(0.06f);
    P.fill(0, 0, L.w, L.h, region);
    if (g.s.n() == 0 || M.n != g.s.n()) return;  // not prepared for this series yet

    std::vector<double> xt = x_ticks(L);
    for (const Pane& pn : L.panes) {
        P.fill(pn.box.x0, pn.box.y0, pn.box.w(), pn.box.h(), plot_bg);
        draw_grid(P, L, pn, grid, text.alpha(0.7f), &xt);
        draw_sessions(P, L, pn);
        if (pn.pm->price) {
            draw_candles(P, L, pn);
            for (int k : M.overlays) draw_result(P, L, pn, M.results[k]);
            draw_trading(P, L, pn);
        } else if (pn.pm->result >= 0) {
            draw_result(P, L, pn, M.results[pn.pm->result]);
        }
        draw_legend(P, L, pn, text.alpha(0.85f));
    }
    for (double x : xt) {
        long j = static_cast<long>(x) - static_cast<long>(g.s.x0);
        std::string t = bar_time(j, "%y-%m-%d", "%m-%d %H:%M");
        float px = L.px(x), tw = text_w(t, kFont * L.s);
        if (px - tw / 2 >= L.plot.x0 && px + tw / 2 <= L.plot.x1)
            P.text(px - tw / 2, L.plot.y1 + 11 * L.s, t, kFont * L.s, text.alpha(0.7f));
    }
    draw_hover(P, L, text);
}

// ---------------------------------------------------------------- input
const Order* order_near(const Layout& L, const Pane& pn, float y) {
    const Order* best = nullptr;
    float best_d = 6 * L.s;
    for (const Order& o : g.broker.orders) {
        if (o.type == OrderType::Market) continue;
        float d = std::fabs(pn.py(o.price) - y);
        if (d <= best_d) best = &o, best_d = d;
    }
    return best;
}

constexpr double kMinSpan = 10, kMaxSpan = 20000;  // bars in view (refresh_series keeps 60,000 buckets loaded)
constexpr double kZoomStep = 0.88;                 // per wheel notch (a trackpad sends fractions of one)

// Keeps a scrolled-back view on the data: at most half the view before the first bar or after the cursor, and
// never further back than the bars refresh_series loads.
void clamp_view() {
    double span = g.xmax - g.xmin;
    double cur = static_cast<double>(g.agg().bucket_of(g.cursor));
    double lo = std::max(-0.5 * span, cur - 60000 + 1), hi = cur - 0.5 * span;
    g.xmin = std::clamp(g.xmin, lo, std::max(lo, hi));
    g.xmax = g.xmin + span;
}

// Zoom eases toward a target span, in log space, so wheel notches (whole steps, several per frame from a
// trackpad) become one continuous motion. The bar under the pointer stays put; when following, the right edge
// stays on the latest bar instead.
struct ZoomAnim {
    bool on = false;
    double target = 0;         // span we're heading for
    double anchor_x = 0;       // bar under the pointer when the gesture last moved
    double anchor_frac = 1;    // ... and where it sits across the plot (0 left, 1 right)
} Z;

double current_span() { return g.follow ? g.visible : g.xmax - g.xmin; }

void set_span(double span) {
    if (g.follow) {
        g.visible = span;  // update_view places xmin/xmax
        return;
    }
    g.visible = span;
    g.xmin = Z.anchor_x - Z.anchor_frac * span, g.xmax = g.xmin + span;
    clamp_view();
}

// eased: whole mouse-wheel notches glide; immediate: trackpad fractions already arrive continuously
void zoom(const Layout& L, double factor, float anchor_px, bool eased) {
    if (!Z.on) Z.target = current_span();
    Z.target = std::clamp(Z.target * factor, kMinSpan, kMaxSpan);
    Z.anchor_frac = std::clamp((anchor_px - L.plot.x0) / L.plot.w(), 0.0f, 1.0f);
    Z.anchor_x = g.xmin + Z.anchor_frac * (g.xmax - g.xmin);
    Z.on = true;
    if (!eased) {
        set_span(Z.target);
        Z.on = false;
    }
}

double g_pan_left = 0;  // bars of a wheel pan still to glide through

void pan_now(double bars) {
    g.follow = false;
    double before = g.xmin;
    g.xmin += bars, g.xmax += bars;
    clamp_view();
    Z.anchor_x += g.xmin - before;  // a zoom easing at the same time (diagonal swipe) keeps the pan
}

void pan(double bars, bool eased) {  // eased for mouse notches, immediate (1:1) for a trackpad
    if (g.follow) Z.on = false;  // leaving follow mode: the zoom anchor below is about to move
    g.follow = false;
    if (eased) g_pan_left += bars;
    else pan_now(bars);
}

int chart_event(void*, BKNode* node, const BKCustomEvent* e) {
    int rc[4];
    BK_node_rect(node, rc);
    Layout L = layout(static_cast<float>(rc[2]), static_cast<float>(rc[3]));
    const Pane* pn = L.pane_at(e->x, e->y);
    const Pane* price = L.panes.empty() ? nullptr : &L.panes[0];
    switch (e->type) {
        case BK_CUSTOM_PRESS:
            if (e->button != 1 || !pn) return 0;
            if (pn->pm->price && (e->mods & (BK_CTRL | BK_SHIFT))) {  // Cmd/Ctrl+click: limit, Shift+click: stop
                click_order(pn->value(e->y), e->mods & BK_CTRL);
            } else if (const Order* o = pn->pm->price ? order_near(L, *pn, e->y) : nullptr) {
                C.drag = ChartState::ORDER, C.order_id = o->id;
            } else {
                C.drag = ChartState::PAN, C.press_x = e->x, C.xmin0 = g.xmin, C.xmax0 = g.xmax;
            }
            break;
        case BK_CUSTOM_RELEASE:
            if (e->button == 1) C.drag = ChartState::NONE;
            break;
        case BK_CUSTOM_MOTION:
            C.hover = true, C.mx = e->x, C.my = e->y;
            if (C.drag == ChartState::ORDER && price) {
                for (Order& o : g.broker.orders)
                    if (o.id == C.order_id) o.price = round_tick(price->value(e->y));
            } else if (C.drag == ChartState::PAN) {
                double dx = (e->x - C.press_x) / L.ppb();
                if (g.follow && std::fabs(e->x - C.press_x) < 3) break;
                g.follow = false;
                Z.on = false, g_pan_left = 0;
                g.xmin = C.xmin0 - dx, g.xmax = C.xmax0 - dx;
                clamp_view();
            }
            {
                int cur = BK_CURSOR_CROSSHAIR;
                if (C.drag == ChartState::ORDER || (C.drag == ChartState::NONE && pn && pn->pm->price && order_near(L, *pn, e->y)))
                    cur = BK_CURSOR_NS;
                else if (C.drag == ChartState::PAN) cur = BK_CURSOR_MOVE;
                else if (!pn) cur = BK_CURSOR_DEFAULT;
                BK_custom_set_cursor(node, cur);
            }
            break;
        case BK_CUSTOM_LEAVE: C.hover = false; break;
        case BK_CUSTOM_WHEEL: {
            // Vertical scrolling zooms, sideways scrolling pans. The macOS host turns trackpad scrolling into
            // whole notches and sends a sideways swipe as Shift + vertical notches, so Shift means sideways.
            // A notch is one wheel click (2 widget units of scrolling); trackpads send fractions of one.
            double h = e->wheel_x, v = e->wheel_y;
            if ((e->mods & BK_SHIFT) && h == 0) h = v, v = 0;
            auto whole = [](double d) { return d == std::round(d); };
            if (h != 0) pan(-h * 2.0 * BK_unit() / L.ppb(), whole(h));  // the chart follows the fingers
            if (v != 0) zoom(L, std::pow(kZoomStep, v), e->x, whole(v));
            break;
        }
        case BK_CUSTOM_KEY:
            if (!e->keysym || !hotkey(e->keysym, e->mods & (BK_SHIFT | BK_CTRL | BK_ALT))) return 0;
            break;
        default: return 0;
    }
    BK_node_invalidate(node);  // hover crosshair, drags: chart_prepare only sees state changes
    poke();
    return 1;
}

// ---------------------------------------------------------------- equity curves
// Cumulative $ after each trade, for the replay's trades and for backtests.
void draw_curve(BuiRaster* r, const int rect[4], const std::vector<double>& eq_in, const char* empty) {
    Painter P{r, static_cast<float>(rect[0]), static_cast<float>(rect[1])};
    float s = ui_scale(), w = static_cast<float>(rect[2]), h = static_cast<float>(rect[3]);
    Rgba region = theme("region_bg", {0.19f, 0.19f, 0.19f, 1}), text = theme("text", {0.9f, 0.9f, 0.9f, 1});
    P.round_rect(0, 0, w, h, 3 * s, {region.r * 0.72f, region.g * 0.72f, region.b * 0.72f, 1});
    if (eq_in.empty()) {
        P.text(8 * s, h / 2, empty, kFont * s, text.alpha(0.45f));
        return;
    }
    std::vector<double> eq{0};
    eq.insert(eq.end(), eq_in.begin(), eq_in.end());
    double lo = std::min(0.0, *std::min_element(eq.begin(), eq.end())), hi = std::max(0.0, *std::max_element(eq.begin(), eq.end()));
    if (hi - lo < 1) hi = lo + 1;
    Box b{4 * s, 4 * s, w - 4 * s, h - 4 * s};
    auto px = [&](size_t i) { return b.x0 + b.w() * static_cast<float>(i) / std::max<size_t>(1, eq.size() - 1); };
    auto py = [&](double v) { return b.y1 - static_cast<float>((v - lo) / (hi - lo)) * b.h(); };
    P.line(b.x0, py(0), b.x1, py(0), 1, text.alpha(0.2f), b);
    size_t step = std::max<size_t>(1, eq.size() / static_cast<size_t>(std::max(1.0f, b.w())));
    for (size_t i = step; i < eq.size(); i += step) P.line(px(i - step), py(eq[i - step]), px(i), py(eq[i]), 1.5f * s, eq.back() >= 0 ? kBuy : kSell, b);
    P.text(b.x0 + 2 * s, b.y0 + 6 * s, fmt("%+.0f", eq.back()), kFont * s, text.alpha(0.7f));
}

void equity_draw(void*, BKNode*, BuiRaster* r, const int rect[4]) {
    std::vector<double> eq;
    double v = 0;
    for (const RoundTrip& t : g.broker.trips) eq.push_back(v += t.pnl);
    draw_curve(r, rect, eq, "equity curve: no closed trades yet");
}

void bt_curve_draw(void*, BKNode*, BuiRaster* r, const int rect[4]) {
    draw_curve(r, rect, g.bt_result.equity, "backtest equity: run a backtest");
}

}  // namespace

static void equity_size();
static BKNode* g_equity;

void chart_load_font() {
    FILE* f = open_file(data_dir() / "chart_font.ttf", "rb");
#ifdef RT_FONT
    if (!f) f = std::fopen(RT_FONT, "rb");
#endif
    if (!f) return;
    std::vector<char> data;
    char buf[65536];
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) data.insert(data.end(), buf, buf + n);
    std::fclose(f);
    if (BUI_text_add_font("Berkeley Mono", 0, data.data(), static_cast<int>(data.size()))) g_family = "Berkeley Mono";
}

BKNode* chart_new() {
    static BKCustomClass cls{};
    cls.event = chart_event;
    cls.draw = chart_draw;
    BKNode* n = BK_node_new("custom");
    BK_custom_set(n, &cls, nullptr);
    C.node = n;
    return n;
}

// Price-axis easing: the fitted range jumps whenever a high or low scrolls in or out of view; the axis glides
// there instead. It snaps on a new session, a timeframe change or the first frame.
struct AxisAnim {
    bool init = false, moving = false;
    double lo = 0, hi = 1;
    int tf = -1;
    size_t cursor = 0;
    double dt = 0;
} Y;

bool chart_animate(double dt) {
    Y.dt = dt;
    if (Z.on) {
        double cur = current_span(), a = 1 - std::exp(-dt * 22.0);  // ~90% of the way in 0.1 s
        double next = std::exp(std::log(cur) + (std::log(Z.target) - std::log(cur)) * a);
        if (std::fabs(std::log(Z.target / next)) < 0.002) next = Z.target, Z.on = false;
        set_span(next);
    }
    if (g_pan_left != 0) {
        double step = g_pan_left * (1 - std::exp(-dt * 22.0));
        if (std::fabs(g_pan_left - step) < 0.01) step = g_pan_left;
        g_pan_left -= step;
        double before = g.xmin;
        pan_now(step);
        if (g.xmin == before) g_pan_left = 0;  // against the end of the data
    }
    return chart_animating();
}

bool chart_animating() { return Z.on || Y.moving || g_pan_left != 0; }

void chart_prepare() {
    update_view();
    const Series& s = g.s;
    M.overlays.clear();
    M.panes.clear();
    M.results.clear();
    M.n = s.n();
    PaneModel price;
    fit(price.ymin, price.ymax, {&s.l, &s.h}, 0.0);
    double pad = std::max((price.ymax - price.ymin) * 0.08, 2.0);
    price.ymin -= pad, price.ymax += pad;
    bool jump = g.cursor + 1 < Y.cursor || g.cursor > Y.cursor + 2000;  // new session / start here
    if (!Y.init || Y.tf != g.tf() || jump || Y.dt <= 0) {
        Y.lo = price.ymin, Y.hi = price.ymax, Y.moving = false;
    } else {
        double a = 1 - std::exp(-Y.dt * 16.0), range = price.ymax - price.ymin;
        Y.lo += (price.ymin - Y.lo) * a, Y.hi += (price.ymax - Y.hi) * a;
        Y.moving = std::fabs(Y.lo - price.ymin) + std::fabs(Y.hi - price.ymax) > range * 0.002;
        if (!Y.moving) Y.lo = price.ymin, Y.hi = price.ymax;
    }
    Y.init = true, Y.tf = g.tf(), Y.cursor = g.cursor, Y.dt = 0;
    price.ymin = Y.lo, price.ymax = Y.hi;
    M.panes.push_back(price);
    for (const Ind& d : g.inds) {
        if (!d.on) continue;
        IndResult r;
        r.uid = d.uid, r.def = d.def(), r.label = d.label(), r.col = d.col;
        if (!r.def) {
            r.out.error = "script " + d.script + " not found";
        } else {
            CachedResult& c = g_cache[d.uid];
            if (c.series != g.s_version || c.gen != g_scripts.generation() || c.script != d.script || c.params != d.params) {
                run_indicator(*r.def, d.params, s, g.tf(), g.s_version, c.out);
                c.series = g.s_version, c.gen = g_scripts.generation(), c.script = d.script, c.params = d.params;
            }
            r.out = c.out;
            if (r.out.error.empty() && !r.def->error.empty()) r.out.error = r.def->error;
        }
        if (!r.out.error.empty() && g_errors[d.uid] != r.out.error) {  // report each new error once
            g_errors[d.uid] = r.out.error;
            g.broker.log.push_back(r.label + ": " + r.out.error.substr(0, r.out.error.find('\n')));
            g_scripts.messages.push_back(r.out.error);
        }
        M.results.push_back(std::move(r));
        int idx = static_cast<int>(M.results.size()) - 1;
        const IndResult& res = M.results.back();
        if (!res.def || res.def->overlay) {
            M.overlays.push_back(idx);
            continue;
        }
        PaneModel pm;
        pm.price = false, pm.result = idx;
        if (res.def->has_range) {
            pm.ymin = res.def->range_lo, pm.ymax = res.def->range_hi;
        } else {
            std::vector<const Vec*> ys;
            for (size_t k = 0; k < res.def->plots.size(); ++k)
                if (res.def->plots[k].style != PlotDef::Markers && k < res.out.plots.size()) ys.push_back(&res.out.plots[k]);
            double lo = INFINITY, hi = -INFINITY;
            for (const Vec* y : ys) {
                double a, b;
                fit(a, b, {y}, 0.0);
                if (a != 0 || b != 1) lo = std::min(lo, a), hi = std::max(hi, b);
            }
            if (!std::isfinite(lo)) lo = 0, hi = 1;
            if (res.def->zero_based) lo = std::min(lo, 0.0);
            double pad = std::max((hi - lo) * 0.08, 1e-9);
            pm.ymin = res.def->zero_based && lo == 0 ? 0 : lo - pad, pm.ymax = hi + pad;
        }
        M.panes.push_back(pm);
    }
    // Redraw only when something drawn changed: the chart is the most expensive thing to paint, and frames
    // also run for the panels (scrolling the sidebar must not repaint it).
    std::vector<double> sig = {static_cast<double>(g.cursor), static_cast<double>(g.tf_idx), g.xmin, g.xmax,
                               price.ymin, price.ymax, static_cast<double>(g.s_version), static_cast<double>(g_scripts.generation()),
                               static_cast<double>(g.hide_dates), static_cast<double>(g.sessions), static_cast<double>(g.broker.pos),
                               g.broker.avg, static_cast<double>(g.broker.fills.size())};
    for (const Order& o : g.broker.orders) sig.push_back(o.id), sig.push_back(o.price);
    for (const Ind& d : g.inds) {
        sig.push_back(d.uid), sig.push_back(d.on), sig.push_back(d.col.r), sig.push_back(d.col.g), sig.push_back(d.col.b);
        sig.insert(sig.end(), d.params.begin(), d.params.end());
    }
    for (const PaneModel& pm : M.panes) sig.push_back(pm.ymin), sig.push_back(pm.ymax);
    static std::vector<double> last_sig;
    if (sig != last_sig) {
        last_sig = std::move(sig);
        if (C.node) BK_node_invalidate(C.node);
    }
    equity_size();
    static size_t last_trips = SIZE_MAX;
    if (g_equity && g.broker.trips.size() != last_trips) last_trips = g.broker.trips.size(), BK_node_invalidate(g_equity);
}

// The equity curve's natural height is in raster pixels, so it is set again when the UI scale changes (Retina).
static int g_equity_unit = 0;

static void equity_size() {
    if (!g_equity || BK_unit() == g_equity_unit) return;
    g_equity_unit = BK_unit();
    BKCustomClass cls{};
    cls.draw = equity_draw;
    cls.natural_h = static_cast<int>(4.5 * g_equity_unit);
    BK_custom_set(g_equity, &cls, nullptr);
    BK_node_relayout(g_equity);
}

BKNode* equity_new() {
    g_equity = BK_node_new("custom");
    equity_size();
    return g_equity;
}

static BKNode* g_bt_curve = nullptr;

BKNode* backtest_curve_new() {
    g_bt_curve = BK_node_new("custom");
    static BKCustomClass cls{};
    cls.draw = bt_curve_draw;
    cls.natural_h = static_cast<int>(4.5 * std::max(1, BK_unit()));
    BK_custom_set(g_bt_curve, &cls, nullptr);
    return g_bt_curve;
}

void backtest_curve_changed() {
    if (g_bt_curve) BK_node_invalidate(g_bt_curve);
}
