// Chart drawing and interaction: panes, axes, candles, indicators, trading overlays, hover tooltip, pan and
// zoom, order placement and dragging, drawn with ImGui draw lists. Coordinates are interface units relative to
// the chart's top-left corner; L.s is the interface scale and L.hair one device pixel.

#include "chart.h"
#include "datafeed.h"

#include <algorithm>
#include <cfloat>
#include <climits>
#include <map>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"
#include "app.h"
#include "script.h"
#include "ui.h"

namespace {

ImFont* g_font = nullptr;        // axis labels, legend, tags (nullptr: the UI font)
ImFont* g_font_title = nullptr;  // the "NQ 5m" title

struct Box {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool in(float x, float y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
    float w() const { return x1 - x0; }
    float h() const { return y1 - y0; }
};

Rgba style_color(ImGuiCol c) {
    ImVec4 v = ImGui::GetStyleColorVec4(c);
    return {v.x, v.y, v.z, v.w};
}

ImU32 u32(Rgba c) { return ImGui::ColorConvertFloat4ToU32(ImVec4(c.r, c.g, c.b, c.a)); }

const float kFont = 11.0f;

ImFont* font_for(float size) {
    ImFont* f = size > kFont * ui_scale() * 1.05f && g_font_title ? g_font_title : g_font;
    return f ? f : ImGui::GetFont();
}

// ---------------------------------------------------------------- drawing helpers over an ImDrawList
struct Painter {
    ImDrawList* dl;
    float ox, oy;  // chart origin on screen

    // Filled rectangle with its edges on device pixels (candles keep their width while the chart pans).
    void fill(float x, float y, float w, float h, Rgba c) const {
        float x0 = snap(ox + x), y0 = snap(oy + y), x1 = snap(ox + x + w), y1 = snap(oy + y + h);
        float px = 1.0f / px_scale();
        if (x1 <= x0) x1 = x0 + px;
        if (y1 <= y0) y1 = y0 + px;
        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), u32(c));
    }
    // Rectangle clipped to `b`.
    void rect(float x0, float y0, float x1, float y1, Rgba c, const Box& b) const {
        if (x0 > x1) std::swap(x0, x1);
        if (y0 > y1) std::swap(y0, y1);
        x0 = std::max(x0, b.x0), x1 = std::min(x1, b.x1), y0 = std::max(y0, b.y0), y1 = std::min(y1, b.y1);
        if (x1 > x0 && y1 > y0) fill(x0, y0, x1 - x0, y1 - y0, c);
    }
    // Line of width w clipped to `b` (Liang-Barsky). Horizontal and vertical lines are pixel-aligned rectangles.
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
        float ax = x0 + t0 * dx, ay = y0 + t0 * dy, bx = x0 + t1 * dx, by = y0 + t1 * dy;
        if (ay == by || ax == bx) {
            float px = 1.0f / px_scale(), half = std::max(w, px) / 2;
            if (ay == by) {
                float y = snap(oy + ay - half);
                dl->AddRectFilled(ImVec2(snap(ox + std::min(ax, bx)), y), ImVec2(snap(ox + std::max(ax, bx)), y + std::max(snap(2 * half), px)), u32(c));
            } else {
                float x = snap(ox + ax - half);
                dl->AddRectFilled(ImVec2(x, snap(oy + std::min(ay, by))), ImVec2(x + std::max(snap(2 * half), px), snap(oy + std::max(ay, by))), u32(c));
            }
            return;
        }
        dl->AddLine(ImVec2(ox + ax, oy + ay), ImVec2(ox + bx, oy + by), u32(c), w);
    }
    // Polyline through points (local units), clipped to `b`.
    void polyline(const std::vector<ImVec2>& pts, float w, Rgba c, const Box& b) const {
        if (pts.size() < 2) return;
        std::vector<ImVec2> v(pts.size());
        for (size_t k = 0; k < pts.size(); ++k) v[k] = ImVec2(ox + pts[k].x, oy + pts[k].y);
        dl->PushClipRect(ImVec2(ox + b.x0, oy + b.y0), ImVec2(ox + b.x1, oy + b.y1), true);
        dl->AddPolyline(v.data(), static_cast<int>(v.size()), u32(c), ImDrawFlags_None, w);
        dl->PopClipRect();
    }
    void dashed_v(float x, float y0, float y1, Rgba c, const Box& b, float dash, float gap) const {
        for (float y = y0; y < y1; y += dash + gap) line(x, y, x, std::min(y + dash, y1), hair(), c, b);
    }
    void dashed_h(float y, float x0, float x1, Rgba c, const Box& b, float dash, float gap) const {
        for (float x = x0; x < x1; x += dash + gap) line(x, y, std::min(x + dash, x1), y, hair(), c, b);
    }
    static float hair() { return 1.0f / px_scale(); }
    void tri(float ax, float ay, float bx, float by, float cx, float cy, Rgba c) const {
        dl->AddTriangleFilled(ImVec2(ox + ax, oy + ay), ImVec2(ox + bx, oy + by), ImVec2(ox + cx, oy + cy), u32(c));
    }
    void round_rect(float x, float y, float w, float h, float rad, Rgba c) const {
        dl->AddRectFilled(ImVec2(snap(ox + x), snap(oy + y)), ImVec2(snap(ox + x + w), snap(oy + y + h)), u32(c), rad);
    }
    // `y` is the text's vertical centre.
    void text(float x, float y, const std::string& s, float size, Rgba c) const {
        if (s.empty()) return;
        dl->AddText(font_for(size), size, ImVec2(snap(ox + x), snap(oy + y - size * 0.55f)), u32(c), s.c_str(), s.c_str() + s.size());
    }
    // Quads (the band fills): four corners each, in local units, one colour; clipped to `b`.
    void quads(const std::vector<ImVec2>& v, ImU32 col, const Box& b) const {
        if (v.size() < 4) return;
        dl->PushClipRect(ImVec2(ox + b.x0, oy + b.y0), ImVec2(ox + b.x1, oy + b.y1), true);
        int nq = static_cast<int>(v.size() / 4);
        dl->PrimReserve(nq * 6, nq * 4);
        ImVec2 uv = dl->_Data->TexUvWhitePixel;
        for (int q = 0; q < nq; ++q) {
            ImDrawIdx k = static_cast<ImDrawIdx>(dl->_VtxCurrentIdx);
            for (int t = 0; t < 4; ++t) dl->PrimWriteVtx(ImVec2(ox + v[q * 4 + t].x, oy + v[q * 4 + t].y), uv, col);
            for (int t : {0, 1, 2, 2, 1, 3}) dl->PrimWriteIdx(static_cast<ImDrawIdx>(k + t));
        }
        dl->PopClipRect();
    }
};

float text_w(const std::string& s, float size) {
    if (s.empty()) return 0;
    return font_for(size)->CalcTextSizeA(size, FLT_MAX, 0, s.c_str(), s.c_str() + s.size()).x;
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
    if (std::fabs(v) < step * 1e-6) v = 0;  // no "-0" from accumulated rounding
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
    bool hover = false;
    float mx = 0, my = 0;
    enum { NONE, PAN, ORDER } drag = NONE;
    int order_id = 0;
    float press_x = 0;
    double xmin0 = 0, xmax0 = 0;
} C;

// ---------------------------------------------------------------- drawing
void draw_series_line(const Painter& P, const Layout& L, const Pane& pn, const Vec& y, Rgba c, float w) {
    long j0, j1;
    visible_range(j0, j1);
    j0 = std::max(0L, j0 - 1), j1 = std::min(static_cast<long>(y.size()) - 1, j1 + 1);
    long step = std::max(1L, static_cast<long>(1.0 / std::max(1e-9, L.ppb() * px_scale())));  // about one point per device pixel
    std::vector<ImVec2> run;
    for (long j = j0; j <= j1; j += step) {
        if (!std::isfinite(y[j])) {
            P.polyline(run, w * L.s, c, pn.box);
            run.clear();
            continue;
        }
        run.push_back(ImVec2(L.px(static_cast<double>(g.s.x0 + j)), static_cast<float>(pn.py(y[j]))));
    }
    P.polyline(run, w * L.s, c, pn.box);
}

// Histogram bars from zero; by_bar colours each by its candle's direction (alpha from c).
void draw_bars(const Painter& P, const Layout& L, const Pane& pn, const Vec& y, Rgba c, bool by_bar = false) {
    long j0, j1;
    visible_range(j0, j1);
    float half = std::max(0.5f / px_scale(), static_cast<float>(L.ppb() * 0.35)), base = pn.py(std::clamp(0.0, pn.ymin, pn.ymax));
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
    if (ppb * px_scale() < 2.0f) {  // under 2 device pixels a bar: a close line
        draw_series_line(P, L, pn, s.c, {0.85f, 0.85f, 0.85f, 1}, 1.0f);
        return;
    }
    float px = 1.0f / px_scale(), half = std::max(px, ppb * 0.36f), wick = std::max(px, std::floor(L.s * px_scale()) * px);
    for (long j = j0; j <= j1; ++j) {
        Rgba c = s.c[j] >= s.o[j] ? kUp : kDown;
        if (j == static_cast<long>(s.n()) - 1 && s.last_partial) c.a = 0.69f;  // forming
        float x = L.px(static_cast<double>(s.x0 + j));
        P.rect(x - wick / 2, pn.py(s.h[j]), x + wick / 2, pn.py(s.l[j]), c, pn.box);
        float top = std::min(pn.py(s.o[j]), pn.py(s.c[j])), bot = std::max(pn.py(s.o[j]), pn.py(s.c[j]));
        if (bot - top < px) bot = top + px;
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
        P.line(pn.box.x0, y, pn.box.x1, y, P.hair(), grid, pn.box);
        if (y > pn.box.y0 + 6 * L.s && y < pn.box.y1 - 6 * L.s)
            P.text(L.axis_x + 6 * L.s, y, axis_number(v, step), kFont * L.s, text);
    }
    if (xticks)
        for (double x : *xticks) {
            float px = L.px(x);
            P.line(px, pn.box.y0, px, pn.box.y1, P.hair(), grid, pn.box);
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
        for (double lv : r.def->levels) P.line(pn.box.x0, pn.py(lv), pn.box.x1, pn.py(lv), P.hair(), {1, 1, 1, 0.3f}, pn.box);
    for (const FillDef& f : r.def->fills) {
        const Vec &a = r.out.plots[f.a], &b = r.out.plots[f.b];
        std::vector<ImVec2> v;
        ImU32 col = u32(r.plot_color(f.a).alpha(f.alpha));
        for (long j = std::max(0L, j0 - 1); j <= j1 && j + 1 < static_cast<long>(a.size()); ++j) {
            if (!std::isfinite(a[j]) || !std::isfinite(a[j + 1]) || !std::isfinite(b[j]) || !std::isfinite(b[j + 1])) continue;
            float xa = L.px(s.x0 + j), xb = L.px(s.x0 + j + 1);
            v.push_back({xa, pn.py(a[j])});
            v.push_back({xa, pn.py(b[j])});
            v.push_back({xb, pn.py(a[j + 1])});
            v.push_back({xb, pn.py(b[j + 1])});
        }
        P.quads(v, col, pn.box);
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
        P.line(L.px(static_cast<double>(s.x0 + sg.i0)), y, L.px(static_cast<double>(s.x0 + sg.i1)), y, P.hair(), c, pn.box);
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
    Rgba bg = style_color(ImGuiCol_PopupBg);
    bg.a = 0.95f;
    P.round_rect(bx, by, bw, bh, 4 * s, bg);
    float y = by + 5 * s + lh / 2;
    for (auto& l : lines) P.text(bx + 8 * s, y, l.first, size, l.second), y += lh;
}

void draw_chart(const Painter& P, const Layout& L) {
    Rgba region = style_color(ImGuiCol_WindowBg), text = style_color(ImGuiCol_Text);
    Rgba plot_bg{region.r * 0.72f, region.g * 0.72f, region.b * 0.72f, 1}, grid = text.alpha(0.06f);
    if (g.s.n() == 0 || M.n != g.s.n() || L.panes.empty()) {  // not prepared for this series yet
        P.fill(0, 0, L.w, L.h, region);
        return;
    }
    // the background around the panes only: the panes paint their own (filling everything twice cost ~1 ms)
    P.fill(0, 0, L.w, L.plot.y0, region);
    P.fill(0, L.plot.y1, L.w, L.h - L.plot.y1, region);
    P.fill(0, L.plot.y0, L.plot.x0, L.plot.h(), region);
    P.fill(L.plot.x1, L.plot.y0, L.w - L.plot.x1, L.plot.h(), region);
    for (size_t k = 1; k < L.panes.size(); ++k)  // gaps between panes
        P.fill(L.plot.x0, L.panes[k - 1].box.y1, L.plot.w(), L.panes[k].box.y0 - L.panes[k - 1].box.y1, region);

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

// Pointer input for this frame: presses, drags, hover and the wheel / trackpad. `active` is the chart's
// invisible button holding the mouse, `hovered` the pointer over it with nothing in the way.
void chart_input(const Layout& L, ImVec2 origin, bool hovered, bool pressed) {
    ImGuiIO& io = ImGui::GetIO();
    float mx = io.MousePos.x - origin.x, my = io.MousePos.y - origin.y;
    const Pane* pn = L.pane_at(mx, my);
    const Pane* price = L.panes.empty() ? nullptr : &L.panes[0];
    bool ctrl = io.KeyCtrl || io.KeySuper;  // Cmd on macOS
    if (pressed && pn) {
        if (pn->pm->price && (ctrl || io.KeyShift)) {  // Cmd/Ctrl+click: limit, Shift+click: stop
            click_order(pn->value(my), ctrl);
        } else if (const Order* o = pn->pm->price ? order_near(L, *pn, my) : nullptr) {
            C.drag = ChartState::ORDER, C.order_id = o->id;
        } else {
            C.drag = ChartState::PAN, C.press_x = mx, C.xmin0 = g.xmin, C.xmax0 = g.xmax;
        }
    }
    if (C.drag != ChartState::NONE && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) C.drag = ChartState::NONE;
    C.hover = hovered || C.drag != ChartState::NONE;
    if (!C.hover) return;
    bool moved = mx != C.mx || my != C.my;
    C.mx = mx, C.my = my;
    if (moved && C.drag == ChartState::ORDER && price) {
        for (Order& o : g.broker.orders)
            if (o.id == C.order_id) o.price = round_tick(price->value(my));
    } else if (moved && C.drag == ChartState::PAN) {
        double dx = (mx - C.press_x) / L.ppb();
        if (!(g.follow && std::fabs(mx - C.press_x) < 3 * L.s)) {
            g.follow = false;
            Z.on = false, g_pan_left = 0;
            g.xmin = C.xmin0 - dx, g.xmax = C.xmax0 - dx;
            clamp_view();
        }
    }
    int cur = CURSOR_CROSSHAIR;
    if (C.drag == ChartState::ORDER || (C.drag == ChartState::NONE && pn && pn->pm->price && order_near(L, *pn, my)))
        cur = CURSOR_NS;
    else if (C.drag == ChartState::PAN) cur = CURSOR_MOVE;
    else if (!pn) cur = CURSOR_ARROW;
    g_cursor_request = cur;
    if (!hovered) return;
    // Vertical scrolling zooms, sideways scrolling pans (Shift turns a mouse wheel sideways). Trackpad deltas
    // apply at once, so the chart follows the fingers; mouse-wheel notches glide.
    for (ScrollEvent& e : frame_scroll()) {
        double h = e.dx, v = e.dy;
        if (io.KeyShift && h == 0) h = v, v = 0;
        if (h != 0) pan(-h * L.s / L.ppb(), e.notch);
        if (v != 0) zoom(L, std::pow(kZoomStep, v / kNotch), mx, e.notch);
        e.dx = e.dy = 0;
    }
}

// ---------------------------------------------------------------- equity curves
// Cumulative $ after each trade, for the replay's trades and for backtests.
void draw_curve(float h, const std::vector<double>& eq_in, const char* empty) {
    ImVec2 o = ImGui::GetCursorScreenPos();
    float s = ui_scale(), w = ImGui::GetContentRegionAvail().x;
    ImGui::Dummy(ImVec2(w, h));
    Painter P{ImGui::GetWindowDrawList(), o.x, o.y};
    Rgba region = style_color(ImGuiCol_WindowBg), text = style_color(ImGuiCol_Text);
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
    P.line(b.x0, py(0), b.x1, py(0), P.hair(), text.alpha(0.2f), b);
    size_t step = std::max<size_t>(1, eq.size() / static_cast<size_t>(std::max(1.0f, b.w() * px_scale())));
    std::vector<ImVec2> pts;
    for (size_t i = 0; i < eq.size(); i += step) pts.push_back(ImVec2(px(i), py(eq[i])));
    if ((eq.size() - 1) % step) pts.push_back(ImVec2(px(eq.size() - 1), py(eq.back())));
    P.polyline(pts, 1.5f * s, eq.back() >= 0 ? kBuy : kSell, b);
    P.text(b.x0 + 2 * s, b.y0 + 6 * s, fmt("%+.0f", eq.back()), kFont * s, text.alpha(0.7f));
}

}  // namespace

void chart_set_fonts(ImFont* text, ImFont* title) { g_font = text, g_font_title = title; }

void chart_draw(float w, float h) {
    ImVec2 o = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##chart", ImVec2(std::max(w, 1.0f), std::max(h, 1.0f)));
    bool hovered = ImGui::IsItemHovered(), pressed = ImGui::IsItemActivated();
    Layout L = layout(w, h);
    if (g.s.n() && M.n == g.s.n() && !L.panes.empty()) chart_input(L, o, hovered, pressed);
    Painter P{ImGui::GetWindowDrawList(), o.x, o.y};
    P.dl->PushClipRect(o, ImVec2(o.x + w, o.y + h), true);
    draw_chart(P, layout(w, h));  // after input: pans and zooms show in this frame
    P.dl->PopClipRect();
}

void equity_draw(float h) {
    std::vector<double> eq;
    double v = 0;
    for (const RoundTrip& t : g.broker.trips) eq.push_back(v += t.pnl);
    draw_curve(h, eq, "equity curve: no closed trades yet");
}

void backtest_curve_draw(float h) { draw_curve(h, g.bt_result.equity, "backtest equity: run a backtest"); }

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
}

