// NQ replay trader: reveal 1-minute bars one at a time, chart them on any timeframe, add indicators, and paper
// trade against the replay. Nothing after the replay cursor is ever shown or used. UI: BUI (Blender's interface
// as a C library): a widget tree for the panels and a custom-drawn node for the chart.
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

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "BUI_kit.h"
#include "GPU_raster.h"
#include "app.h"
#include "chart.h"
#include "civil.h"
#include "datafeed.h"
#include "http.h"
#include "ui.h"

namespace {

BKWindow* g_win = nullptr;
BKNode* g_root = nullptr;
BKNode* g_setup = nullptr;  // the data screen, until the trader replaces it
bool g_trading = false;
const char* g_start = nullptr;
bool g_demo = false;
int g_handled_run = 0;
int64_t g_from_day = -1;  // --from (testing): download only from this day  // the download run whose result on_frame has acted on
Downloader g_dl;
std::string g_load_err;
std::chrono::steady_clock::time_point g_last_frame = std::chrono::steady_clock::now();

struct Key {
    const char* sym;
    int mods;
};
Key kKeys[] = {{"space", 0}, {"Right", 0}, {"Right", BK_SHIFT}, {"Up", 0}, {"Down", 0}, {"b", 0},
               {"s", 0},     {"x", 0},     {"c", 0},            {"l", 0},  {"n", 0}};

void sync() {
    if (!g_trading) {
        if (g_setup) setup_sync();
        return;
    }
    chart_prepare();
    panels_sync();
}

void on_emit(void*, BKNode* node, int kind, void* ref, double, double) {
    if (!g_trading) {
        if (node) setup_event(node, kind);
    } else if (kind == BK_EV_CALL && ref) {
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
    if (g_trading) tick(dt);
    // a download that finished cleanly goes straight on to the trader
    else if (g_dl.finished() && g_dl.runs != g_handled_run) {
        g_handled_run = g_dl.runs;
        if (g_dl.error().empty()) enter_trader();
    }
    sync();
    if (g_trading && g.playing) BK_window_request_frame(win, 1);
}

void on_wake() { poke(); }

void start_replay() {
    g.opens.clear();
    for (size_t i = 0; i < g.m.size(); ++i)
        if (((g.m.t[i] % 1440) + 1440) % 1440 == 570) g.opens.push_back(i);
    if (!g_start || !start_at(g_start)) random_day();
    if (g_demo) {  // a quick tour: a few indicators and a bracketed trade
        for (int t : {EMA, BOLL, PIVOTS, VWAP, RSI, MACD}) g.inds.push_back(make_ind(t, g.next_uid++));
        g.tf_idx = 1;
        advance(70);
        g.bracket = true;
        market_order(1, 1);
        advance(25);
        pending_order(-1, OrderType::Limit, round_tick(g.last() + 15));
    }
}

void build_trader() {
    BKNode* left = panels_new();
    BK_node_add(g_root, left, -1);
    BK_geom_set_manager(left, BK_GEOM_PACK);
    BK_geom_set(left, "side", 2);  // left
    BK_geom_set(left, "fill", 2);  // y
    BKNode* chart = chart_new();
    BK_node_add(g_root, chart, -1);
    BK_geom_set_manager(chart, BK_GEOM_PACK);
    BK_geom_set(chart, "side", 2);
    BK_geom_set(chart, "expand", 1);
    BK_geom_set(chart, "fill", 3);  // both
    g_trading = true;
}

// Loads the market; on failure reports it (data screen or stderr) and returns false.
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

Downloader& downloader() { return g_dl; }

void enter_trader() {
    if (g_trading) return;
    g_dl.stop();
    if (!load_market()) {  // the data screen stays up
        setup_error("Could not open the data: " + g_load_err);
        return;
    }
    start_replay();
    if (g_setup) {
        BK_node_remove(g_root, g_setup);
        BK_node_free(g_setup);
        g_setup = nullptr;
        setup_free();
    }
    build_trader();
    BK_node_relayout(g_root);
    poke();
}

int main(int argc, char** argv) {
    const char* shot = nullptr;
    const char* cli_key = nullptr;
    const char* data_file = nullptr;
    bool offline = false, show_setup = false;
    for (int a = 1; a < argc; ++a) {
        if (!std::strcmp(argv[a], "--screenshot") && a + 1 < argc) shot = argv[++a];
        else if (!std::strcmp(argv[a], "--start") && a + 1 < argc) g_start = argv[++a];
        else if (!std::strcmp(argv[a], "--api-key") && a + 1 < argc) cli_key = argv[++a];
        else if (!std::strcmp(argv[a], "--data") && a + 1 < argc) data_file = argv[++a];
        else if (!std::strcmp(argv[a], "--offline")) offline = true;
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
        else if (!std::strcmp(argv[a], "--show-setup")) show_setup = true;  // screenshot the data screen
        else if (!std::strcmp(argv[a], "--help") || !std::strcmp(argv[a], "-h")) {
            std::printf("usage: replay_trader [--api-key KEY] [--offline] [--data nq_1m.bin] [--start \"YYYY-MM-DD HH:MM\"]\n"
                        "                     [--screenshot out.bmp] [--demo]\n"
                        "API key: --api-key, NQ_API_KEY, or the one saved from the data screen.\n"
                        "Data folder: %s\n", data_dir().string().c_str());
            return 0;
        }
    }
    http_global_init();
    std::string key_source;
    std::string key = resolve_api_key(cli_key, key_source);
    bool headless = shot || !BK_window_supported();

    g_root = BK_node_new("frame");
    std::string err;
    if (data_file) {  // a file from export_bars.py: no download
        if (!g.m.load(data_file, err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 1;
        }
        start_replay();
        build_trader();
    } else if ((headless || offline) && !show_setup) {
        if (!offline && !key.empty()) download_blocking(key);  // on failure, fall back to whatever is cached
        if (!load_market()) {
            std::fprintf(stderr, key.empty() && !offline ? "No API key: pass --api-key KEY or set NQ_API_KEY.\n" : "");
            return 1;
        }
        start_replay();
        build_trader();
    } else {
        g_setup = setup_new(key, key_source);
        BK_node_add(g_root, g_setup, -1);
        BK_geom_set_manager(g_setup, BK_GEOM_PACK);  // centred at its natural size
        BK_geom_set(g_setup, "expand", 1);
        BK_geom_set(g_setup, "anchor", 0);
        if (!key.empty() && !headless) {  // fetch what's new straight away; the trader opens when it's done
            setup_mark_started();
            g_dl.start(key, g_from_day);
        }
    }

    chart_load_font();
    BKContext* ctx = BK_context_new(g_root);
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
        BK_window_wake_init(on_wake);
        g_dl.on_progress = [] { BK_window_wake(); };
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
    g_dl.on_progress = nullptr;
    g_dl.stop();
    BK_context_free(ctx);
    BK_node_free(g_root);
    return 0;
}
