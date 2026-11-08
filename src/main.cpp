// NQ replay trader: reveal 1-minute bars one at a time, chart them on any timeframe, add indicators, and paper
// trade against the replay. Nothing after the replay cursor is ever shown or used. UI: BUI (Blender's interface
// as a C library): a widget tree for the panels and a custom-drawn node for the chart.
//
// Usage: replay_trader [--start "YYYY-MM-DD HH:MM"] [--screenshot out.bmp] [--demo]
// Keys:  Space play/pause, Right step 1 min, Shift+Right finish the chart bar, N skip to next 09:30 open,
//        Up/Down speed, B buy, S sell, X flatten, C cancel orders, L follow latest bar.
// Chart: Cmd/Ctrl+click places a limit at that price, Shift+click a stop; drag order lines to move them; drag to
//        pan, scroll to zoom.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "BUI_kit.h"
#include "GPU_raster.h"
#include "app.h"
#include "chart.h"
#include "ui.h"

namespace {

BKWindow* g_win = nullptr;
std::chrono::steady_clock::time_point g_last_frame = std::chrono::steady_clock::now();

struct Key {
    const char* sym;
    int mods;
};
Key kKeys[] = {{"space", 0}, {"Right", 0}, {"Right", BK_SHIFT}, {"Up", 0}, {"Down", 0}, {"b", 0},
               {"s", 0},     {"x", 0},     {"c", 0},            {"l", 0},  {"n", 0}};

void sync() {
    chart_prepare();
    panels_sync();
}

void on_emit(void*, BKNode* node, int kind, void* ref, double, double) {
    if (kind == BK_EV_CALL && ref) {
        const Key* k = static_cast<const Key*>(ref);
        hotkey(k->sym, k->mods);
    } else if (node) {
        panels_event(node, kind);
    }
    poke();
}

void on_frame(void*, BKWindow* win) {
    auto now = std::chrono::steady_clock::now();
    double dt = std::min(0.1, std::chrono::duration<double>(now - g_last_frame).count());
    g_last_frame = now;
    tick(dt);
    sync();
    if (g.playing) BK_window_request_frame(win, 1);
}

int on_close(void*, BKWindow*) {
    BK_window_quit();
    return 1;
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

}  // namespace

void poke() {
    if (g_win) BK_window_request_frame(g_win, 0);
}

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
    if (!start || !start_at(start)) random_day();
    if (demo) {  // a quick tour: a few indicators and a bracketed trade
        for (int t : {EMA, BOLL, PIVOTS, VWAP, RSI, MACD}) g.inds.push_back(make_ind(t, g.next_uid++));
        g.tf_idx = 1;
        advance(70);
        g.bracket = true;
        market_order(1, 1);
        advance(25);
        pending_order(-1, OrderType::Limit, round_tick(g.last() + 15));
    }

    chart_load_font(RT_FONT);
    BKNode* root = BK_node_new("frame");
    BKNode* left = panels_new();
    BK_node_add(root, left, -1);
    BK_geom_set_manager(left, BK_GEOM_PACK);
    BK_geom_set(left, "side", 2);  // left
    BK_geom_set(left, "fill", 2);  // y
    BKNode* chart = chart_new();
    BK_node_add(root, chart, -1);
    BK_geom_set_manager(chart, BK_GEOM_PACK);
    BK_geom_set(chart, "side", 2);
    BK_geom_set(chart, "expand", 1);
    BK_geom_set(chart, "fill", 3);  // both

    BKContext* ctx = BK_context_new(root);
    BKHost host{};
    host.emit = on_emit;
    BK_context_set_host(ctx, &host);
    for (Key& k : kKeys) {
        std::string combo = std::string(k.mods & BK_SHIFT ? "shift+" : "") + k.sym;
        BK_bind(ctx, combo.c_str(), &k);
    }

    const int W = 1600, H = 950;
    if (shot || !BK_window_supported()) {  // off-screen render (no window)
        const char* env = std::getenv("BUI_UI_SCALE");
        BK_set_scale(env ? static_cast<float>(std::atof(env)) : 1.0f);
        int w = static_cast<int>(W * BK_scale()), h = static_cast<int>(H * BK_scale());
        BuiRaster* r = BUI_raster_create(w, h);
        for (int pass = 0; pass < 3; ++pass) {  // layout settles over the first renders
            sync();
            BK_render(ctx, r, w, h, 1);
        }
        bool ok = shot && save_bmp(shot, BUI_raster_pixels(r), w, h);
        if (shot) std::printf(ok ? "wrote %s (%dx%d)\n" : "could not write %s\n", shot, w, h);
        BUI_raster_free(r);
    } else {
        g_win = BK_window_new(ctx, "NQ replay trader", W, H);
        BKWindowCallbacks cb{};
        cb.on_frame = on_frame;
        cb.on_close = on_close;
        BK_window_set_callbacks(g_win, &cb);
        sync();
        BK_window_show(g_win);
        poke();
        BK_window_run();
        BK_window_free(g_win);
        g_win = nullptr;
    }
    BK_context_free(ctx);
    BK_node_free(root);
    return 0;
}
