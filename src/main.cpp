// Replay Trader: reveal 1-minute bars one at a time, chart them on any timeframe, add indicators, and paper
// trade against the replay. Nothing after the replay cursor is ever shown or used. UI: Dear ImGui on GLFW and
// OpenGL (rendered on the GPU, at the display's refresh rate while something moves, not at all while idle).
//
// Usage: replay_trader [--api-key KEY] [--offline] [--data nq_1m.bin] [--start "YYYY-MM-DD HH:MM"]
//                      [--screenshot out.bmp] [--demo]
// Data: NQ 1-minute bars are downloaded from the karaiq API into the user's data folder (datafeed.h) with the key
// from --api-key, NQ_API_KEY, the saved key, or the data screen. --offline uses the cache as it is; --data loads
// a file written by export_bars.py instead.
// Keys:  Space play/pause, Right step 1 min, Shift+Right finish the chart bar, N skip to next 09:30 open,
//        Up/Down speed, B buy, S sell, X flatten, C cancel orders, L follow latest bar.
// Chart: Cmd/Ctrl+click places a limit at that price, Shift+click a stop; drag order lines to move them; drag to
//        pan, scroll to zoom.
// Developer: RT_FRAME_LOG=1 logs per-frame times to stderr; RT_BENCH=pan|zoom|wheel|scroll|scrollwheel replays
// synthetic scrolling in the real window and prints frame-time statistics.

#include <GLFW/glfw3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl2.h"
#include "imgui_impl_opengl3.h"
#include "app.h"
#include "chart.h"
#include "civil.h"
#include "datafeed.h"
#include "embedded_files.h"
#include "http.h"
#include "softraster.h"
#include "ui.h"

#ifdef __APPLE__
#include "mac_metal.h"
extern "C" void mac_input_init();  // src/mac_input.m
extern "C" bool mac_scroll_precise();
#endif

ImFont* g_font_bold = nullptr;
int g_cursor_request = CURSOR_NONE;

namespace {

GLFWwindow* g_win = nullptr;
enum Renderer { METAL, GL3, GL2 } g_renderer = GL3;  // Metal on macOS; OpenGL 3, or 2 on old drivers and VMs
bool g_trading = false;
const char* g_start = nullptr;
bool g_demo = false, g_show_strategy = false;
int g_handled_run = 0;          // the download run whose result the frame loop has acted on
int64_t g_from_day = -1;        // --from (testing): download only from this day
Downloader g_dl;
std::string g_load_err;
float g_ui_scale = 1, g_px_scale = 1;
std::vector<ScrollEvent> g_scroll;
std::atomic<bool> g_wake{false};
int g_frames_needed = 2;        // frames still to draw after input (ImGui settles hover and clicks over two)
bool g_anim = false;            // something asked for the next frame (keep_animating)
bool g_rescale = false;         // the window moved to a display with another scale
std::vector<char> g_chart_font;  // chart_font.ttf / RT_FONT, if there is one
bool g_loop_running = false, g_in_frame = false;

const EmbeddedFile* embedded(const char* path) {
    for (int i = 0; i < kEmbeddedFileCount; ++i)
        if (!std::strcmp(kEmbeddedFiles[i].path, path)) return &kEmbeddedFiles[i];
    return nullptr;
}

// chart_font.ttf in the data folder, else the build machine's RT_FONT (developer builds only).
void load_chart_font() {
    FILE* f = open_file(data_dir() / "chart_font.ttf", "rb");
#ifdef RT_FONT
    if (!f) f = std::fopen(RT_FONT, "rb");
#endif
    if (!f) return;
    char buf[65536];
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) g_chart_font.insert(g_chart_font.end(), buf, buf + n);
    std::fclose(f);
}

// Fonts are rasterized at their size in device pixels (crisp on Retina) and drawn at 1 / px_scale.
void build_fonts() {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    static const ImWchar ranges[] = {0x0020, 0x00FF, 0x2013, 0x2026, 0x2190, 0x21FF, 0x25A0, 0x25FF, 0};
    float raster = g_ui_scale * g_px_scale;
    auto add = [&](const void* data, size_t size, float pt) {
        ImFontConfig cfg;
        cfg.FontDataOwnedByAtlas = false;
        return io.Fonts->AddFontFromMemoryTTF(const_cast<void*>(data), static_cast<int>(size), pt * raster, &cfg, ranges);
    };
    const EmbeddedFile* regular = embedded("fonts/Inter-Regular.ttf");
    const EmbeddedFile* bold = embedded("fonts/Inter-SemiBold.ttf");
    add(regular->data, regular->size, 13.0f);
    g_font_bold = add(bold->data, bold->size, 14.5f);
    const void* cdata = g_chart_font.empty() ? static_cast<const void*>(regular->data) : g_chart_font.data();
    size_t csize = g_chart_font.empty() ? regular->size : g_chart_font.size();
    ImFont* text = add(cdata, csize, 11.0f);
    ImFont* title = add(cdata, csize, 12.1f);
    if (!text || !title) {  // an unreadable chart_font.ttf: the UI font
        g_chart_font.clear();
        return build_fonts();
    }
    io.FontGlobalScale = 1.0f / g_px_scale;
    chart_set_fonts(text, title);
}

// A dark theme after Blender's (the look of the BUI version).
void apply_style() {
    ImGuiStyle& st = ImGui::GetStyle();
    st = ImGuiStyle();
    ImGui::StyleColorsDark(&st);
    auto grey = [](float v, float a = 1) { return ImVec4(v, v, v, a); };
    const ImVec4 blue(0.278f, 0.447f, 0.702f, 1.0f);
    ImVec4* c = st.Colors;
    c[ImGuiCol_Text] = grey(0.90f);
    c[ImGuiCol_TextDisabled] = grey(0.58f);
    c[ImGuiCol_WindowBg] = grey(0.19f);
    c[ImGuiCol_ChildBg] = grey(0, 0);
    c[ImGuiCol_PopupBg] = grey(0.10f, 0.97f);
    c[ImGuiCol_Border] = grey(0.0f, 0.35f);
    c[ImGuiCol_FrameBg] = grey(0.115f);
    c[ImGuiCol_FrameBgHovered] = grey(0.14f);
    c[ImGuiCol_FrameBgActive] = grey(0.16f);
    c[ImGuiCol_Button] = grey(0.33f);
    c[ImGuiCol_ButtonHovered] = grey(0.39f);
    c[ImGuiCol_ButtonActive] = blue;
    c[ImGuiCol_Header] = grey(0.24f);
    c[ImGuiCol_HeaderHovered] = grey(0.28f);
    c[ImGuiCol_HeaderActive] = grey(0.30f);
    c[ImGuiCol_CheckMark] = blue;
    c[ImGuiCol_SliderGrab] = blue;
    c[ImGuiCol_SliderGrabActive] = blue;
    c[ImGuiCol_Separator] = grey(0.27f);
    c[ImGuiCol_ScrollbarBg] = grey(0, 0);
    c[ImGuiCol_ScrollbarGrab] = grey(0.36f);
    c[ImGuiCol_ScrollbarGrabHovered] = grey(0.42f);
    c[ImGuiCol_ScrollbarGrabActive] = grey(0.48f);
    c[ImGuiCol_PlotHistogram] = blue;
    c[ImGuiCol_TextSelectedBg] = ImVec4(blue.x, blue.y, blue.z, 0.5f);
    st.WindowPadding = ImVec2(6, 6);
    st.FramePadding = ImVec2(6, 3.5f);
    st.ItemSpacing = ImVec2(4, 4);
    st.ItemInnerSpacing = ImVec2(5, 4);
    st.FrameRounding = st.GrabRounding = st.PopupRounding = st.ChildRounding = 4;
    st.ScrollbarRounding = 6;
    st.ScrollbarSize = 10;
    st.WindowBorderSize = st.ChildBorderSize = st.FrameBorderSize = 0;
    st.PopupBorderSize = 1;
    st.WindowRounding = 0;
    st.ScaleAllSizes(g_ui_scale);
}

void start_replay() {
    g.opens.clear();
    for (size_t i = 0; i < g.m.size(); ++i)
        if (((g.m.t[i] % 1440) + 1440) % 1440 == 570) g.opens.push_back(i);
    if (!g_start || !start_at(g_start)) random_day();
    if (g_demo) {  // a quick tour: a few indicators and a bracketed trade
        std::string ids = "ema,bollinger,pivots,vwap,rsi,macd";
        if (const char* e = std::getenv("RT_SHOT_INDS")) ids = e;  // e.g. macd,pivots,my_indicator
        for (size_t a = 0; a < ids.size();) {
            size_t b = ids.find(',', a);
            if (b == std::string::npos) b = ids.size();
            if (b > a) g.inds.push_back(make_ind(ids.substr(a, b - a).c_str(), g.next_uid++));
            a = b + 1;
        }
        g.tf_idx = 1;
        if (std::getenv("RT_SHOT_AUTOTRADE")) {  // tick Auto-trade on the indicators that trade, then just replay
            for (Ind& d : g.inds)
                if (d.def() && d.def()->trades) set_ind_trade(d, true);
            advance(std::getenv("RT_SHOT_ADVANCE") ? std::atoi(std::getenv("RT_SHOT_ADVANCE")) : 390);
            return;
        }
        advance(std::getenv("RT_SHOT_ADVANCE") ? std::atoi(std::getenv("RT_SHOT_ADVANCE")) : 70);
        g.bracket = true;
        market_order(1, 1);
        advance(std::getenv("RT_SHOT_AFTER") ? std::atoi(std::getenv("RT_SHOT_AFTER")) : 25);
        pending_order(-1, OrderType::Limit, round_tick(g.last() + 15));
    }
}


bool load_market() {
    std::string err;
    std::vector<std::string> notes;
    if (!load_cached_market(g.m, err, &notes) || g.m.size() < 2) {
        g_load_err = err.empty() ? "too little data" : err;
        std::fprintf(stderr, "%s\n", g_load_err.c_str());
        return false;
    }
    for (const std::string& n : notes) g.broker.log.push_back(n);
    return true;
}

// --bench-indicators: time every indicator (default params) on the chart's largest series (60000 bars) at a few
// timeframes, ending at the last bar of the cached data. "build" includes making the Lua bars table; "calc" reuses it.
int run_indicator_bench(const char* data_file) {
    std::string err;
    if (data_file ? !g.m.load(data_file, err) : !load_market()) return std::fprintf(stderr, "%s\n", err.c_str()), 1;
    using clk = std::chrono::steady_clock;
    long key = 1000000;
    for (int tf : {1, 5, 15}) {
        const Agg& a = g.agg_for(tf);
        size_t cursor = g.m.size() - 1;
        int64_t cur = tf == 1 ? static_cast<int64_t>(cursor) : static_cast<int64_t>(a.bucket_of(cursor));
        Series s;
        build_series(g.m, a, cursor, std::max<int64_t>(0, cur - 60000), s);
        std::printf("tf %dm, %zu bars\n", tf, s.n());
        for (const ScriptDef* d : g_scripts.list(false)) {
            Params p = default_params(*d);
            IndOutput out;
            double best_build = 1e9, best_calc = 1e9;
            for (int rep = 0; rep < 5; ++rep) {
                auto t0 = clk::now();
                run_indicator(*d, p, s, tf, ++key, out);
                auto t1 = clk::now();
                run_indicator(*d, p, s, tf, key, out);
                auto t2 = clk::now();
                best_build = std::min(best_build, std::chrono::duration<double, std::milli>(t1 - t0).count());
                best_calc = std::min(best_calc, std::chrono::duration<double, std::milli>(t2 - t1).count());
            }
            // order-independent checksum of the output, to compare a rewritten script with the original
            double sum = 0;
            for (size_t k = 0; k < out.plots.size(); ++k)
                for (size_t i = 0; i < out.plots[k].size(); ++i)
                    if (std::isfinite(out.plots[k][i])) sum += out.plots[k][i] * static_cast<double>((i % 97) + k + 1);
            for (const IndMarker& m : out.markers) sum += m.price * static_cast<double>(m.i % 89 + 1) + (m.up ? 7 : 3) + m.plot;
            for (const IndSegment& g2 : out.segments)
                sum += g2.price * static_cast<double>(g2.i0 % 83 + 1) + static_cast<double>(g2.i1 % 79) * g2.alpha + g2.plot;
            for (const IndBarMark& b : out.bars) sum += static_cast<double>(b.i % 73) * b.alpha + b.plot + (b.candle ? 5 : 1);
            std::printf("  %-18s build %7.2f ms  calc %7.2f ms  (%zu markers, %zu segments, sum %.15g)%s%s\n", d->id.c_str(),
                        best_build, best_calc, out.markers.size(), out.segments.size(), sum, out.error.empty() ? "" : "  ERROR: ",
                        out.error.substr(0, out.error.find('\n')).c_str());
        }
    }
    return 0;
}

// --backtest: run a strategy over a date range and print the statistics (no window, no download).
struct CostArgs {
    std::string mnq;              // --instrument nq|mnq ("" = NQ defaults)
    double commission = -1;       // --commission $/side
    int slippage = -1;            // --slippage ticks
    std::string fees;             // --fees YEAR=$,YEAR=$,...
    bool by_year = false;         // --by-year
};

int run_backtest_cli(const char* id, const char* from_s, const char* to_s, const std::vector<std::string>& sets,
                     const char* data_file, const char* csv, const CostArgs& cost) {
    std::string err;
    if (data_file ? !g.m.load(data_file, err) : !load_market()) {
        std::fprintf(stderr, "%s\n", data_file ? err.c_str() : g_load_err.c_str());
        return 1;
    }
    const ScriptDef* d = g_scripts.find(true, id);
    if (!d || !d->error.empty()) {
        std::fprintf(stderr, "strategy %s: %s\n", id, d ? d->error.c_str() : "not found");
        for (const ScriptDef* s : g_scripts.list(true)) std::fprintf(stderr, "  available: %s\n", s->id.c_str());
        return 1;
    }
    Params p = default_params(*d);
    for (const std::string& kv : sets) {
        size_t eq = kv.find('=');
        std::string name = kv.substr(0, eq), val = eq == std::string::npos ? "" : kv.substr(eq + 1);
        bool found = false;
        for (size_t k = 0; k < d->params.size(); ++k) {
            const ParamDef& q = d->params[k];
            if (q.name != name) continue;
            found = true;
            if (q.kind == ParamDef::Bool) p[k] = val == "true" || val == "1";
            else if (q.kind == ParamDef::Choice) {
                auto it = std::find(q.options.begin(), q.options.end(), val);
                p[k] = it == q.options.end() ? q.def : static_cast<double>(it - q.options.begin());
            } else p[k] = std::atof(val.c_str());
        }
        if (!found) std::fprintf(stderr, "warning: %s has no param %s\n", id, name.c_str());
    }
    if (!cost.mnq.empty()) {
        bool micro = cost.mnq == "mnq";
        g.broker.mult = micro ? 2.0 : 20.0, g.broker.commission = micro ? 0.62 : 2.15;
    }
    if (cost.commission >= 0) g.broker.commission = cost.commission;
    if (cost.slippage >= 0) g.broker.slip_ticks = cost.slippage;
    if (!cost.fees.empty()) {  // "2010=2.01,2016=2.03,..." or nq / mnq for the built-in history
        std::string fees = cost.fees == "nq" ? kNqFeeHistory : cost.fees == "mnq" ? kMnqFeeHistory : cost.fees;
        if (!parse_fee_schedule(fees, g.broker.fee_schedule, err)) return std::fprintf(stderr, "%s\n", err.c_str()), 1;
    }
    int64_t from = g.m.t.front(), to = g.m.t.back() + 1;
    if (from_s && !parse_minutes(from_s, from)) return std::fprintf(stderr, "bad --bt-from\n"), 1;
    if (to_s) {
        if (!parse_minutes(to_s, to)) return std::fprintf(stderr, "bad --bt-to\n"), 1;
        to += 1440;  // inclusive
    }
    auto t0 = std::chrono::steady_clock::now();
    g.bt.start(*d, p, &g.m, from, to, g.broker, g.bt_qc_rolls);
    while (g.bt.running()) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    g.bt.cancel();
    BacktestResult r = g.bt.result();
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const BacktestStats& s = r.stats;
    std::printf("%s  (%s to %s, %d sessions, %.1f s%s)\n", r.params.c_str(), fmt_time(r.from, "%Y-%m-%d").c_str(),
                fmt_time(r.to, "%Y-%m-%d").c_str(), s.days, secs, r.qc_rolls ? ", QuantConnect roll schedule" : "");
    if (!r.error.empty()) std::printf("error: %s\n", r.error.c_str());
    int losses = s.trades - s.wins;
    std::printf("net $%.0f  trades %d  win %.1f%%  PF %.2f  avg win $%.0f  avg loss $%.0f  expectancy $%.1f\n", s.net, s.trades,
                100.0 * s.wins / std::max(1, s.trades), s.gross_loss > 0 ? s.gross_win / s.gross_loss : 0.0,
                s.wins ? s.gross_win / s.wins : 0.0, losses ? -s.gross_loss / losses : 0.0, s.net / std::max(1, s.trades));
    std::printf("max DD $%.0f  Sharpe %.2f  days up %.0f%%  best $%.0f  worst $%.0f  losing run %d  avg hold %.0f min  fees $%.0f\n",
                s.max_dd, s.sharpe, s.pct_days_up, s.largest_win, s.largest_loss, s.max_consec_losses, s.avg_minutes, s.fees);
    size_t shown = 0;
    for (const std::string& l : r.log)
        if (shown++ < 8) std::printf("  log: %s\n", l.c_str());
    if (r.log.size() > 8) std::printf("  log: ... %zu lines\n", r.log.size());
    for (const std::string& l : r.log)
        if (l.find("gap in the data") != std::string::npos) std::printf("  warning: %s\n", l.c_str());
    std::printf("costs: %s\n", r.costs.c_str());
    if (cost.by_year && !r.trips.empty()) {  // by the year each trade closed
        std::printf("year  trades    net $   fees $  fee/trade   win%%\n");
        auto year_of = [](int64_t t) { int y, mo, d; civil_from_days(floor_div(t, 1440), y, mo, d); return y; };
        for (size_t k = 0; k < r.trips.size();) {
            int y = year_of(r.trips[k].t_out), n = 0, w = 0;
            double net = 0, fee = 0;
            for (; k < r.trips.size() && year_of(r.trips[k].t_out) == y; ++k)
                ++n, net += r.trips[k].pnl, fee += r.trips[k].fee, w += r.trips[k].pnl > 0;
            std::printf("%d  %6d  %7.0f  %7.0f  %9.2f  %5.1f\n", y, n, net, fee, fee / n, 100.0 * w / n);
        }
    }
    if (csv) std::printf(export_trips_csv(csv, r.trips) ? "trades written to %s\n" : "could not write %s\n", csv);
    return r.error.empty() ? 0 : 1;
}

// Headless: download synchronously with progress on stderr.
bool download_blocking(const std::string& key) {
    g_dl.start(key, g_from_day);
    int last = -1;
    while (!g_dl.finished()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (g_dl.done != last) {
            last = g_dl.done;
            std::fprintf(stderr, "\r%s  %d / %d days   ", g_dl.status().c_str(), last, g_dl.total.load());
        }
    }
    g_dl.stop();
    std::fprintf(stderr, "\n%s\n", g_dl.error().empty() ? "data up to date" : g_dl.error().c_str());
    return g_dl.error().empty();
}


bool save_bmp(const char* path, const uint32_t* px, int w, int h) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    int row = (w * 3 + 3) & ~3, size = 54 + row * h;
    unsigned char hdr[54] = {'B', 'M'};
    auto put = [&](int off, int v) { std::memcpy(hdr + off, &v, 4); };
    put(2, size), put(10, 54), put(14, 40), put(18, w), put(22, -h), put(34, row * h);  // negative height: top-down
    hdr[26] = 1, hdr[28] = 24;
    std::fwrite(hdr, 1, 54, f);
    std::vector<unsigned char> line(row, 0);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint32_t p = px[static_cast<size_t>(y) * w + x];  // 0x00RRGGBB
            line[x * 3] = p & 0xFF, line[x * 3 + 1] = (p >> 8) & 0xFF, line[x * 3 + 2] = (p >> 16) & 0xFF;
        }
        std::fwrite(line.data(), 1, line.size(), f);
    }
    std::fclose(f);
    return true;
}


// Keyboard shortcuts, unless a text field has the keyboard.
void handle_keys() {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput) return;
    int mods = (io.KeyShift ? 1 : 0) | (io.KeyCtrl || io.KeySuper || io.KeyAlt ? 2 : 0);
    struct K {
        ImGuiKey key;
        const char* sym;
        bool repeat;
    };
    static const K keys[] = {{ImGuiKey_Space, "space", false}, {ImGuiKey_RightArrow, "Right", true}, {ImGuiKey_UpArrow, "Up", true},
                             {ImGuiKey_DownArrow, "Down", true}, {ImGuiKey_B, "b", false}, {ImGuiKey_S, "s", false},
                             {ImGuiKey_X, "x", false}, {ImGuiKey_C, "c", false}, {ImGuiKey_L, "l", false}, {ImGuiKey_N, "n", false},
                             {ImGuiKey_A, "a", false}, {ImGuiKey_H, "h", false}, {ImGuiKey_E, "e", false},
                             {ImGuiKey_LeftBracket, "bracketleft", true}, {ImGuiKey_RightBracket, "bracketright", true}};
    for (const K& k : keys)
        if (ImGui::IsKeyPressed(k.key, k.repeat)) hotkey(k.sym, mods);
}

// One frame of the interface (between ImGui::NewFrame and ImGui::Render).
void frame(float dt) {
    g_cursor_request = CURSOR_NONE;
    if (g_trading) {
        tick(dt);
        chart_animate(dt);
        if (g.bt.runs() != g.bt_seen && !g.bt.running()) {  // a backtest finished
            g.bt_result = g.bt.result();
            g.bt_seen = g.bt.runs();
            g.bt_sel = -1;
        }
    } else if (g_dl.finished() && g_dl.runs != g_handled_run) {  // a clean download goes straight on to the trader
        g_handled_run = g_dl.runs;
        if (g_dl.error().empty()) enter_trader();
    }
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##root", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    if (!g_trading) {
        setup_draw();
    } else {
        handle_keys();
        chart_prepare();
        panels_draw(dt);
        ImGui::SameLine(0, 0);
        ImVec2 at = ImGui::GetCursorScreenPos(), avail = ImGui::GetContentRegionAvail();
        ImGui::GetWindowDrawList()->AddRectFilled(at, ImVec2(at.x + 1.0f / g_px_scale, at.y + avail.y), IM_COL32(0, 0, 0, 90));
        ImGui::BeginChild("##chartarea", avail, 0, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        chart_draw(avail.x, avail.y);
        ImGui::EndChild();
        if (g.playing || chart_animating()) keep_animating();
    }
    ImGui::End();
}

void wake_from_worker() { poke(); }

}  // namespace

void poke() {
    g_wake = true;
    if (g_win) glfwPostEmptyEvent();
}

void keep_animating() { g_anim = true; }
std::vector<ScrollEvent>& frame_scroll() { return g_scroll; }
float ui_scale() { return g_ui_scale; }
float px_scale() { return g_px_scale; }
Downloader& downloader() { return g_dl; }

void enter_trader() {
    if (g_trading) return;
    g_dl.stop();
    if (!load_market()) {  // the data screen stays up
        setup_error("Could not open the data: " + g_load_err);
        return;
    }
    start_replay();
    g_trading = true;
    poke();
}

namespace {

// --show-strategy (documentation screenshots): the opening range breakout trading the replay, with a backtest.
void show_strategy() {
    for (const char* id : {"session_levels", "opening_range", "vwap"}) g.inds.push_back(make_ind(id, g.next_uid++));
    select_strategy("orb");
    start_strategy();
    advance(80);
    const ScriptDef* d = g_scripts.find(true, "orb");
    int64_t from = 0, to = 0;
    if (!d || !parse_minutes("2025-01-01", from) || !parse_minutes("2025-12-31", to)) return;
    g.bt.start(*d, g.strat_params, &g.m, from, to + 1440, g.broker, false);
    while (g.bt.running()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    g.bt_result = g.bt.result();
    g.bt_seen = g.bt.runs();
    if (const char* k = std::getenv("RT_SHOT_TRADE")) goto_trade(std::atoi(k) - 1);  // e.g. 5: the fifth trade
    if (const char* z = std::getenv("RT_SHOT_ZOOM")) chart_zoom_out(std::atof(z));
    panels_scroll_to_end();
}

// Off-screen: lay the interface out at 1600 x 950 units (times RT_UI_SCALE device pixels), render it on the CPU
// and write a BMP. Works without a display or GPU (CI).
int render_screenshot(const char* shot) {
    int W = 1600, H = 950;
    if (const char* sz = std::getenv("RT_SHOT_SIZE")) std::sscanf(sz, "%dx%d", &W, &H);  // e.g. 850x500
    const char* env = std::getenv("RT_UI_SCALE");
    g_px_scale = std::clamp(env ? static_cast<float>(std::atof(env)) : 1.0f, 0.5f, 4.0f), g_ui_scale = 1;
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(W, H);
    io.DisplayFramebufferScale = ImVec2(g_px_scale, g_px_scale);
    build_fonts();
    apply_style();
    unsigned char* px = nullptr;
    SoftTexture tex;
    io.Fonts->GetTexDataAsRGBA32(&px, &tex.w, &tex.h);
    tex.rgba = px;
    io.Fonts->SetTexID(static_cast<ImTextureID>(1));
    for (int pass = 0; pass < 6; ++pass) {  // layout (auto-sized boxes, scrolling) settles over the first frames
        io.DeltaTime = 1.0f / 60;
        ImGui::NewFrame();
        frame(0);
        ImGui::Render();
        g_scroll.clear();
    }
    int w = static_cast<int>(W * g_px_scale), h = static_cast<int>(H * g_px_scale);
    std::vector<uint32_t> img;
    soft_render(ImGui::GetDrawData(), tex, w, h, 0x303030, img);
    bool ok = save_bmp(shot, img.data(), w, h);
    std::printf(ok ? "wrote %s (%dx%d)\n" : "could not write %s\n", shot, w, h);
    return ok ? 0 : 1;
}

// ---------------------------------------------------------------- the window
void mark_input() { g_frames_needed = std::max(g_frames_needed, 3); }
void on_cursor_pos(GLFWwindow*, double, double) { mark_input(); }
void on_mouse_button(GLFWwindow*, int, int, int) { mark_input(); }
void on_key(GLFWwindow*, int, int, int, int) { mark_input(); }
void on_char(GLFWwindow*, unsigned int) { mark_input(); }
void on_focus(GLFWwindow*, int) { mark_input(); }
void on_enter(GLFWwindow*, int) { mark_input(); }
void draw_frame();
void on_size(GLFWwindow*, int, int) {
    mark_input();
    if (g_loop_running && !g_in_frame) draw_frame();  // live resizing (macOS runs its own loop meanwhile)
}
void on_refresh(GLFWwindow*) { mark_input(); }
void on_scale(GLFWwindow*, float, float) { g_rescale = true, mark_input(); }

// Wheel and trackpad: kept for the sidebar and the chart (ScrollEvent, in units), and passed on to ImGui (popups)
// scaled so that ImGui's own scrolling also moves 1:1 with the fingers.
void on_scroll(GLFWwindow*, double xoff, double yoff) {
#ifdef __APPLE__
    bool precise = mac_scroll_precise();  // GLFW sends a trackpad's pixel deltas times 0.1, wheel clicks as lines
    float k = precise ? 10.0f : kNotch;
    bool notch = !precise;
#else  // Windows: 1.0 per wheel click (120 units), touchpads send fractions
    float k = kNotch * g_ui_scale;
    bool notch = xoff == std::round(xoff) && yoff == std::round(yoff);
#endif
    ScrollEvent e{static_cast<float>(xoff) * k, static_cast<float>(yoff) * k, notch};
    g_scroll.push_back(e);
    ImGuiIO& io = ImGui::GetIO();
    float step = 5.0f * (io.Fonts->Fonts.Size ? io.Fonts->Fonts[0]->FontSize * io.FontGlobalScale : 13.0f);
    io.AddMouseWheelEvent(e.dx / step, e.dy / step);
    mark_input();
}

void update_scales() {
    int ww, wh, fw, fh;
    float cx = 1, cy = 1;
    glfwGetWindowSize(g_win, &ww, &wh);
    glfwGetFramebufferSize(g_win, &fw, &fh);
    glfwGetWindowContentScale(g_win, &cx, &cy);
    g_px_scale = ww > 0 && fw > 0 ? static_cast<float>(fw) / ww : 1.0f;
    g_ui_scale = std::max(0.5f, cx / g_px_scale);  // macOS: 1 (points); Windows: the display's scale
}

void rebuild_font_texture() {
#ifdef __APPLE__
    if (g_renderer == METAL) return metal_rebuild_fonts();
#endif
    if (g_renderer == GL3) ImGui_ImplOpenGL3_DestroyFontsTexture(), ImGui_ImplOpenGL3_CreateFontsTexture();
    else ImGui_ImplOpenGL2_DestroyFontsTexture(), ImGui_ImplOpenGL2_CreateFontsTexture();
}

// A window for OpenGL 3 (or, failing that, any OpenGL context and the OpenGL 2 renderer).
GLFWwindow* create_gl_window() {
    glfwDefaultWindowHints();
#ifdef __APPLE__
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#else
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
#endif
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    g_renderer = GL3;
    GLFWwindow* w = glfwCreateWindow(1600, 950, "Replay Trader", nullptr, nullptr);
    if (!w) {
        g_renderer = GL2;
        glfwDefaultWindowHints();
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
        w = glfwCreateWindow(1600, 950, "Replay Trader", nullptr, nullptr);
    }
    if (w) glfwMakeContextCurrent(w), glfwSwapInterval(1);  // vsync
    return w;
}

GLFWcursor* standard_cursor(int shape) {
    static GLFWcursor* cache[8] = {};
    static const int shapes[] = {GLFW_ARROW_CURSOR, GLFW_IBEAM_CURSOR, GLFW_CROSSHAIR_CURSOR, GLFW_RESIZE_NS_CURSOR,
                                 GLFW_RESIZE_ALL_CURSOR, GLFW_RESIZE_EW_CURSOR, GLFW_POINTING_HAND_CURSOR, GLFW_NOT_ALLOWED_CURSOR};
    int i = static_cast<int>(std::find(std::begin(shapes), std::end(shapes), shape) - std::begin(shapes));
    if (i >= 8) i = 0;
    if (!cache[i]) cache[i] = glfwCreateStandardCursor(shapes[i]);
    return cache[i];
}

// The chart's cursor, else ImGui's (ImGui doesn't have a crosshair, so the app sets cursors itself).
void update_cursor() {
    int shape = GLFW_ARROW_CURSOR;
    switch (g_cursor_request) {
        case CURSOR_CROSSHAIR: shape = GLFW_CROSSHAIR_CURSOR; break;
        case CURSOR_NS: shape = GLFW_RESIZE_NS_CURSOR; break;
        case CURSOR_MOVE: shape = GLFW_RESIZE_ALL_CURSOR; break;
        case CURSOR_ARROW: break;
        default:
            switch (ImGui::GetMouseCursor()) {
                case ImGuiMouseCursor_TextInput: shape = GLFW_IBEAM_CURSOR; break;
                case ImGuiMouseCursor_ResizeAll: shape = GLFW_RESIZE_ALL_CURSOR; break;
                case ImGuiMouseCursor_ResizeNS: shape = GLFW_RESIZE_NS_CURSOR; break;
                case ImGuiMouseCursor_ResizeEW: shape = GLFW_RESIZE_EW_CURSOR; break;
                case ImGuiMouseCursor_Hand: shape = GLFW_POINTING_HAND_CURSOR; break;
                case ImGuiMouseCursor_NotAllowed: shape = GLFW_NOT_ALLOWED_CURSOR; break;
                default: break;
            }
    }
    static int current = -1;
    if (shape != current) glfwSetCursor(g_win, standard_cursor(current = shape));
}

// Frame timing (RT_FRAME_LOG, RT_BENCH): CPU time to build and submit a frame, time in glfwSwapBuffers (vsync
// wait and presentation) and the interval between frames.
struct FrameStats {
    std::vector<double> cpu, swap, interval;
    void add(double c, double s, double i) { cpu.push_back(c), swap.push_back(s), interval.push_back(i); }
    static double pct(std::vector<double> v, double p) {
        if (v.empty()) return 0;
        std::sort(v.begin(), v.end());
        return v[std::min(v.size() - 1, static_cast<size_t>(p * v.size()))];
    }
    static double mean(const std::vector<double>& v) {
        double s = 0;
        for (double x : v) s += x;
        return v.empty() ? 0 : s / v.size();
    }
    void print(const char* what) const {
        std::fprintf(stderr, "%s: %zu frames  cpu mean %.2f p50 %.2f p95 %.2f max %.2f ms  swap mean %.2f ms  interval mean %.2f p95 %.2f ms (%.0f fps)\n",
                     what, cpu.size(), mean(cpu), pct(cpu, 0.5), pct(cpu, 0.95), pct(cpu, 1.0), mean(swap), mean(interval),
                     pct(interval, 0.95), interval.empty() ? 0.0 : 1000.0 / mean(interval));
    }
};

// RT_BENCH: synthetic input through the same path as real scrolling, over the chart or the sidebar.
struct Bench {
    std::string mode;
    int frame = 0;
    size_t unused = 0;
    FrameStats stats;
    static constexpr int kWarm = 60, kFrames = 480;
    bool on() const { return !mode.empty(); }
    void inject() {
        if (!on()) return;
        keep_animating();
        if (frame == 0)  // RT_BENCH_ZOOM=20: start zoomed out that many times (lots of bars and indicator shapes)
            if (const char* z = std::getenv("RT_BENCH_ZOOM")) chart_zoom_out(std::atof(z));
        if (frame < kWarm) return;
        int k = frame - kWarm;
        if (mode == "play") {  // the replay advancing a minute every frame: indicators recompute each time
            if (k % 120 == 0) std::fprintf(stderr, "bench play: %.0f bars visible, series %zu bars\n", g.xmax - g.xmin, g.s.n());
            advance(1);
            return;
        }
        ImGuiIO& io = ImGui::GetIO();
        bool sidebar = mode == "scroll" || mode == "scrollwheel";
        io.AddMousePosEvent(sidebar ? 200 * g_ui_scale : io.DisplaySize.x * 0.6f, io.DisplaySize.y * 0.5f);
        float dir = (k / 120) % 2 ? -1.0f : 1.0f;
        if (k % 60 == 0) std::fprintf(stderr, "bench %s: view %.1f..%.1f bars, %zu events left unused last frame\n", mode.c_str(), g.xmin, g.xmax, unused);
        if (mode == "pan") g_scroll.push_back({6.0f * dir, 0, false});
        else if (mode == "zoom") g_scroll.push_back({0, 4.0f * dir, false});
        else if (mode == "scroll") g_scroll.push_back({0, -8.0f * dir, false});
        else if ((mode == "wheel" || mode == "scrollwheel") && k % 12 == 0) g_scroll.push_back({0, (mode == "wheel" ? 1 : -1) * kNotch * dir, true});
        keep_animating();
    }
    bool done() const { return on() && frame >= kWarm + kFrames; }
};

struct {
    bool frame_log = false, was_idle = true;
    double t_prev = 0, t_frame_prev = 0;
    Bench bench;
    FrameStats all;
} W;

// Draws and presents one frame (from the loop, and from the resize callback while macOS resizes the window).
void draw_frame() {
    g_in_frame = true;
    auto c0 = std::chrono::steady_clock::now();
    if (g_rescale) {
        g_rescale = false;
        update_scales();
        build_fonts();
        apply_style();
        rebuild_font_texture();
    }
    double now = glfwGetTime();
    double dt = W.was_idle ? 1.0 / 120 : std::min(0.1, now - W.t_prev);
    W.t_prev = now;
    W.was_idle = false;
    double vsync_wait = 0;
    switch (g_renderer) {
#ifdef __APPLE__
        case METAL: {
            int fw, fh;
            glfwGetFramebufferSize(g_win, &fw, &fh);
            vsync_wait = metal_new_frame(fw, fh);
            break;
        }
#endif
        case GL3: ImGui_ImplOpenGL3_NewFrame(); break;
        default: ImGui_ImplOpenGL2_NewFrame(); break;
    }
    ImGui_ImplGlfw_NewFrame();
    W.bench.inject();  // after the backend's own mouse position
    ImGui::NewFrame();
    frame(static_cast<float>(dt));
    ImGui::Render();
    int fw, fh;
    glfwGetFramebufferSize(g_win, &fw, &fh);
    update_cursor();
    bool scrolled = !g_scroll.empty();
    W.bench.unused = std::count_if(g_scroll.begin(), g_scroll.end(), [](const ScrollEvent& e) { return e.dx != 0 || e.dy != 0; });
    g_scroll.clear();
    auto c1 = std::chrono::steady_clock::now(), c2 = c1;
    if (g_renderer == METAL) {
#ifdef __APPLE__
        metal_render(0.19f, 0.19f, 0.19f);
        c1 = c2 = std::chrono::steady_clock::now();
#endif
    } else {
        glViewport(0, 0, fw, fh);
        glClearColor(0.19f, 0.19f, 0.19f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        if (g_renderer == GL3) ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        else ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
        c1 = std::chrono::steady_clock::now();
        glfwSwapBuffers(g_win);
        c2 = std::chrono::steady_clock::now();
    }
    double swap = std::chrono::duration<double, std::milli>(c2 - c1).count() + vsync_wait;
    double cpu = std::chrono::duration<double, std::milli>(c1 - c0).count() - vsync_wait;
    double t_frame = glfwGetTime(), interval = W.t_frame_prev > 0 ? (t_frame - W.t_frame_prev) * 1000 : 0;
    W.t_frame_prev = t_frame;
    if (W.frame_log) {
        std::fprintf(stderr, "frame cpu %.2f ms  swap %.2f ms  interval %.2f ms%s\n", cpu, swap, interval, scrolled ? "  scroll" : "");
        if (scrolled && interval > 0 && interval < 100) W.all.add(cpu, swap, interval);
    }
    if (W.bench.on()) {
        if (W.bench.frame >= Bench::kWarm && interval > 0) W.bench.stats.add(cpu, swap, interval);
        if (++W.bench.frame, W.bench.done()) {
            W.bench.stats.print(("bench " + W.bench.mode).c_str());
            glfwSetWindowShouldClose(g_win, GLFW_TRUE);
        }
    }
    g_in_frame = false;
}

int run_window() {
    glfwSetErrorCallback([](int code, const char* msg) { std::fprintf(stderr, "GLFW error %d: %s\n", code, msg); });
    if (!glfwInit()) {
        std::fprintf(stderr, "could not open a window (GLFW init failed)\n");
        return 1;
    }
#ifdef __APPLE__  // Metal: a window without an OpenGL context
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    g_win = glfwCreateWindow(1600, 950, "Replay Trader", nullptr, nullptr);
    if (g_win && metal_init(g_win)) {
        g_renderer = METAL;
    } else if (g_win) {
        glfwDestroyWindow(g_win);
        g_win = nullptr;
    }
#endif
    if (!g_win) g_win = create_gl_window();
    if (!g_win) {
        std::fprintf(stderr, "could not open a window\n");
        glfwTerminate();
        return 1;
    }
#ifdef __APPLE__
    mac_input_init();
#endif
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    // the app's own callbacks run after ImGui's (it chains them): they only note that input arrived
    glfwSetCursorPosCallback(g_win, on_cursor_pos);
    glfwSetMouseButtonCallback(g_win, on_mouse_button);
    glfwSetKeyCallback(g_win, on_key);
    glfwSetCharCallback(g_win, on_char);
    glfwSetWindowFocusCallback(g_win, on_focus);
    glfwSetCursorEnterCallback(g_win, on_enter);
    glfwSetWindowSizeCallback(g_win, on_size);
    glfwSetWindowRefreshCallback(g_win, on_refresh);
    glfwSetWindowContentScaleCallback(g_win, on_scale);
    if (g_renderer == METAL) ImGui_ImplGlfw_InitForOther(g_win, true);
    else ImGui_ImplGlfw_InitForOpenGL(g_win, true);
    glfwSetScrollCallback(g_win, on_scroll);  // replaces ImGui's: see on_scroll
    if (g_renderer == GL3 && !ImGui_ImplOpenGL3_Init(nullptr)) g_renderer = GL2;
    if (g_renderer == GL2) ImGui_ImplOpenGL2_Init();
    update_scales();
    build_fonts();
    apply_style();

    g_dl.on_progress = wake_from_worker;
    g.bt.on_progress = wake_from_worker;
    chart_set_async(true);
    glfwShowWindow(g_win);

    const char* log_env = std::getenv("RT_FRAME_LOG");
    W.frame_log = log_env && *log_env && std::strcmp(log_env, "0");
    if (const char* b = std::getenv("RT_BENCH")) W.bench.mode = b;
    if (const char* e = std::getenv("RT_IND_SYNC"); e && *e && std::strcmp(e, "0")) chart_set_async(false);
    double last_poll = glfwGetTime();
    W.t_prev = last_poll;
    g_loop_running = true;
    while (!glfwWindowShouldClose(g_win)) {
        bool busy = g_anim || g_frames_needed > 0;
        if (busy) {
            glfwPollEvents();
        } else {  // idle: sleep until input, a wake-up from a worker, or the next once-a-second script check
            double timeout = 1.0 - (glfwGetTime() - last_poll);
            if (io.WantTextInput) timeout = std::min(timeout, 0.5);  // the text cursor blinks
            glfwWaitEventsTimeout(std::max(0.001, timeout));
            W.was_idle = true;
        }
        if (g_wake.exchange(false)) g_frames_needed = std::max(g_frames_needed, 2);
        double now = glfwGetTime();
        if (now - last_poll >= 1.0) {  // pick up edited user scripts
            last_poll = now;
            if (g_scripts.poll_changes()) {
                g.broker.log.push_back("scripts reloaded");
                g_frames_needed = std::max(g_frames_needed, 2);
            }
            if (io.WantTextInput) g_frames_needed = std::max(g_frames_needed, 1);
        }
        if (now - W.t_prev >= 0.5 && io.WantTextInput) g_frames_needed = std::max(g_frames_needed, 1);
        if (!g_anim && g_frames_needed <= 0) continue;
        g_anim = false;
        --g_frames_needed;
        draw_frame();
    }
    g_loop_running = false;
    if (W.frame_log && !W.all.cpu.empty()) W.all.print("frames with scrolling input");
    g_dl.on_progress = nullptr;
    g.bt.on_progress = nullptr;
    switch (g_renderer) {
#ifdef __APPLE__
        case METAL: metal_shutdown(); break;
#endif
        case GL3: ImGui_ImplOpenGL3_Shutdown(); break;
        default: ImGui_ImplOpenGL2_Shutdown(); break;
    }
    ImGui_ImplGlfw_Shutdown();
    glfwDestroyWindow(g_win);
    g_win = nullptr;
    glfwTerminate();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const char* shot = nullptr;
    const char* cli_key = nullptr;
    const char* data_file = nullptr;
    bool offline = false, show_setup = false;
    const char *bt_id = nullptr, *bt_from = nullptr, *bt_to = nullptr, *bt_csv = nullptr;
    std::vector<std::string> bt_sets;  // --set name=value (backtest params)
    CostArgs cost;
    bool bench = false;
    for (int a = 1; a < argc; ++a) {
        if (!std::strcmp(argv[a], "--screenshot") && a + 1 < argc) shot = argv[++a];
        else if (!std::strcmp(argv[a], "--start") && a + 1 < argc) g_start = argv[++a];
        else if (!std::strcmp(argv[a], "--api-key") && a + 1 < argc) cli_key = argv[++a];
        else if (!std::strcmp(argv[a], "--data") && a + 1 < argc) data_file = argv[++a];
        else if (!std::strcmp(argv[a], "--offline")) offline = true;
        else if (!std::strcmp(argv[a], "--backtest") && a + 1 < argc) bt_id = argv[++a];
        else if (!std::strcmp(argv[a], "--bt-from") && a + 1 < argc) bt_from = argv[++a];
        else if (!std::strcmp(argv[a], "--bt-to") && a + 1 < argc) bt_to = argv[++a];
        else if (!std::strcmp(argv[a], "--set") && a + 1 < argc) bt_sets.push_back(argv[++a]);
        else if (!std::strcmp(argv[a], "--bt-csv") && a + 1 < argc) bt_csv = argv[++a];
        else if (!std::strcmp(argv[a], "--qc-rolls")) g.bt_qc_rolls = true;
        else if (!std::strcmp(argv[a], "--instrument") && a + 1 < argc) cost.mnq = argv[++a];
        else if (!std::strcmp(argv[a], "--commission") && a + 1 < argc) cost.commission = std::atof(argv[++a]);
        else if (!std::strcmp(argv[a], "--slippage") && a + 1 < argc) cost.slippage = std::atoi(argv[++a]);
        else if (!std::strcmp(argv[a], "--fees") && a + 1 < argc) cost.fees = argv[++a];
        else if (!std::strcmp(argv[a], "--by-year")) cost.by_year = true;
        else if (!std::strcmp(argv[a], "--list-rolls")) {  // the cached market's roll points, then exit
            std::string err;
            std::vector<std::string> notes;
            if (!load_cached_market(g.m, err, &notes)) return std::fprintf(stderr, "%s\n", err.c_str()), 1;
            for (size_t i = 1; i < g.m.size(); ++i)
                if (g.m.adj[i] != g.m.adj[i - 1])
                    std::printf("%s %+.2f\n", fmt_time(g.m.t[i], "%Y-%m-%d %H:%M").c_str(), g.m.adj[i] - g.m.adj[i - 1]);
            for (const std::string& n : notes) std::printf("# %s\n", n.c_str());
            return 0;
        }
        else if (!std::strcmp(argv[a], "--from") && a + 1 < argc) {
            int64_t m;
            if (parse_minutes(argv[++a], m)) g_from_day = m / 1440;
        }
        else if (!std::strcmp(argv[a], "--demo")) g_demo = true;
        else if (!std::strcmp(argv[a], "--bench-indicators")) bench = true;
        else if (!std::strcmp(argv[a], "--show-setup")) show_setup = true;  // screenshot the data screen
        else if (!std::strcmp(argv[a], "--show-strategy")) g_show_strategy = true;  // screenshot the Strategy panel
        else if (!std::strcmp(argv[a], "--help") || !std::strcmp(argv[a], "-h")) {
            std::printf("usage: replay_trader [--api-key KEY] [--offline] [--data nq_1m.bin] [--start \"YYYY-MM-DD HH:MM\"]\n"
                        "                     [--screenshot out.bmp] [--demo]\n"
                        "       replay_trader --backtest STRATEGY [--bt-from YYYY-MM-DD] [--bt-to YYYY-MM-DD]\n"
                        "                     [--set name=value ...] [--bt-csv trades.csv] [--qc-rolls] [--data file | --offline]\n"
                        "                     [--instrument nq|mnq] [--commission $/side] [--slippage ticks]\n"
                        "                     [--fees nq|mnq|YEAR=$/side,YEAR=$/side,...] [--by-year]\n"
                        "API key: --api-key, NQ_API_KEY, or the one saved from the data screen.\n"
                        "Data folder: %s\n", data_dir().string().c_str());
            return 0;
        }
    }
    http_global_init();
    std::string script_err;
    if (!g_scripts.init(script_err)) std::fprintf(stderr, "scripts: %s\n", script_err.c_str());
    for (const std::string& m : g_scripts.messages) std::fprintf(stderr, "scripts: %s\n", m.c_str());
    std::string key_source;
    std::string key = resolve_api_key(cli_key, key_source);
    bool headless = shot != nullptr;

    std::string err;
    if (bench) return run_indicator_bench(data_file);
    if (bt_id) return run_backtest_cli(bt_id, bt_from, bt_to, bt_sets, data_file, bt_csv, cost);
    if (data_file) {  // a file from export_bars.py: no download
        if (!g.m.load(data_file, err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 1;
        }
        start_replay();
        g_trading = true;
    } else if ((headless || offline) && !show_setup) {
        if (!offline && !key.empty()) download_blocking(key);  // on failure, fall back to whatever is cached
        if (!load_market()) {
            std::fprintf(stderr, key.empty() && !offline ? "No API key: pass --api-key KEY or set NQ_API_KEY.\n" : "");
            return 1;
        }
        start_replay();
        g_trading = true;
    } else {
        setup_init(key, key_source);
        if (!key.empty() && !headless) {  // fetch what's new straight away; the trader opens when it's done
            setup_mark_started();
            g_dl.start(key, g_from_day);
        }
    }
    if (g_trading && g_show_strategy) show_strategy();

    load_chart_font();
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // no imgui.ini next to the app
    io.LogFilename = nullptr;
    io.ConfigDragClickToInputText = false;
    int rc = shot ? render_screenshot(shot) : run_window();
    g_dl.on_progress = nullptr;
    g_dl.stop();
    g.bt.on_progress = nullptr;
    g.bt.cancel();
    ImGui::DestroyContext();
    return rc;
}
