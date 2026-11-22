// The data screen shown before the trader: asks for an API key when there is none, shows download progress, and
// offers the cached data when the download can't finish (offline, bad key).

#include <cstdio>
#include <string>

#include "BUI_kit.h"
#include "app.h"
#include "datafeed.h"
#include "ui.h"

namespace {

struct {
    BKNode *root = nullptr, *stop, *key, *remember, *download, *use_cache, *progress, *bar, *error, *cache, *forget;
    std::string key_source;
    bool started = false, cached = false;
    int state = -1;  // last busy state seen by setup_sync; the cache is re-read when it changes
} S;

BKNode* add(BKNode* parent, const char* type) {
    BKNode* n = BK_node_new(type);
    BK_node_add(parent, n, -1);
    return n;
}

BKNode* label(BKNode* parent, const std::string& text, bool dim = false) {
    BKNode* n = add(parent, "label");
    BK_set_str(n, "text", text.c_str());
    if (dim) {
        float c[4];
        if (BK_theme_color("text", c)) c[3] = 0.55f, BK_set_color(n, "text_color", c);
    }
    return n;
}

std::string with_commas(long v) {
    std::string s = std::to_string(v);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<size_t>(i), ",");
    return s;
}

std::string cache_text() {
    CacheSummary c = cache_summary();
    if (!c.bars) return "Nothing downloaded yet.";
    return "Cached: " + with_commas(static_cast<long>(c.bars)) + " bars, " + fmt_time(c.first, "%Y-%m-%d") + " to " +
           fmt_time(c.last, "%Y-%m-%d %H:%M") + " NY.";
}

void set_text(BKNode* n, const std::string& s) {
    if (s != BK_get_str(n, "text")) BK_set_str(n, "text", s.c_str());
}

}  // namespace

void setup_start() {
    std::string key = BK_get_str(S.key, "value");
    while (!key.empty() && (key.back() == ' ' || key.back() == '\n')) key.pop_back();
    if (key.empty()) {
        set_text(S.error, "Enter your API key first.");
        return;
    }
    if (BK_get_num(S.remember, "value") != 0) save_api_key(key);
    else if (S.key_source == "saved") forget_api_key();
    set_text(S.error, "");
    S.started = true;
    downloader().start(key, -1);
    poke();
}

BKNode* setup_new(const std::string& key, const std::string& source) {
    S.key_source = source;
    S.root = BK_node_new("column");
    BK_set_num(S.root, "padding", 14);
    BK_set_num(S.root, "spacing", 6);

    BKNode* title = label(S.root, "NQ replay trader");
    BK_set_num(title, "bold", 1);
    label(S.root, "NQ futures 1-minute bars (Dec 2009 to today) download from the karaiq", true);
    label(S.root, "Futures Data API (api.karaiq.com) with your API key. The full history is", true);
    label(S.root, "~4,400 requests and the API answers one at a time, so the first download", true);
    label(S.root, "takes about an hour. It runs newest first: stop whenever you like and trade", true);
    label(S.root, "what's there; the rest resumes next time. Later starts take seconds.", true);
    add(S.root, "separator");

    S.key = add(S.root, "text");
    BK_set_str(S.key, "label", "API key");
    BK_set_str(S.key, "placeholder", "X-API-Key for api.karaiq.com");
    BK_set_str(S.key, "mask", "\xE2\x80\xA2");  // U+2022 bullet
    BK_set_str(S.key, "value", key.c_str());
    S.remember = add(S.root, "checkbox");
    BK_set_str(S.remember, "label", "Remember the key on this computer");
    BK_set_num(S.remember, "value", source == "saved" || source.empty() ? 1 : 0);

    BKNode* r = add(S.root, "row");
    S.download = add(r, "button");
    BK_set_str(S.download, "text", "Download data");
    if (BK_icon_id("IMPORT")) BK_set_str(S.download, "icon", "IMPORT");
    S.stop = add(r, "button");
    BK_set_str(S.stop, "text", "Stop");
    S.use_cache = add(r, "button");
    BK_set_str(S.use_cache, "text", "Use cached data");

    S.bar = add(S.root, "progress");
    S.progress = label(S.root, "");
    S.error = label(S.root, "");
    float red[4] = {kSell.r, kSell.g, kSell.b, 1};
    BK_set_color(S.error, "text_color", red);
    add(S.root, "separator");
    S.cache = label(S.root, cache_text(), true);
    label(S.root, "Data folder: " + data_dir().string(), true);
    S.forget = add(S.root, "button");
    BK_set_str(S.forget, "text", "Forget saved key");
    BK_set_num(S.forget, "enabled", source == "saved");
    return S.root;
}

void setup_sync() {
    Downloader& d = downloader();
    bool busy = d.running();
    if (S.state != busy) {
        S.state = busy;
        S.cached = cache_summary().bars > 0;
        set_text(S.cache, cache_text());
        BK_set_num(S.download, "enabled", !busy);
        BK_set_num(S.key, "enabled", !busy);
        BK_set_num(S.use_cache, "enabled", !busy && S.cached);
        BK_set_num(S.stop, "enabled", busy);
    }
    bool cached = S.cached;
    int done = d.done, total = d.total;
    double f = total > 0 ? static_cast<double>(done) / total : (S.started && d.finished() ? 1.0 : 0.0);
    if (BK_get_num(S.bar, "factor") != f) BK_set_num(S.bar, "factor", f);
    std::string p;
    if (busy) {
        p = d.status();
        if (total > 0) p += "   " + with_commas(done) + " / " + with_commas(total) + " days";
    } else if (S.started && d.finished()) {
        p = d.error().empty() ? "Download complete." : d.error() == "cancelled" ? "Stopped." : "Download stopped.";
    }
    set_text(S.progress, p);
    if (S.started && d.finished() && !d.error().empty() && d.error() != "cancelled") {
        std::string e = d.error();
        if (e.find("rejected the key") != std::string::npos) e += ": check it and try again.";
        else if (cached) e += ". Press Download to retry, or use the cached data.";
        set_text(S.error, e);
    }
}

bool setup_event(BKNode* node, int kind) {
    if (!S.root) return false;
    if ((node == S.download && kind == BK_EV_CLICK) || (node == S.key && kind == BK_EV_SUBMIT)) {
        if (!downloader().running()) setup_start();
        return true;
    }
    if (node == S.stop && kind == BK_EV_CLICK) {
        downloader().cancel();
        return true;
    }
    if (node == S.use_cache && kind == BK_EV_CLICK) {
        enter_trader();
        return true;
    }
    if (node == S.forget && kind == BK_EV_CLICK) {
        forget_api_key();
        S.key_source.clear();
        BK_set_str(S.key, "value", "");
        BK_set_num(S.forget, "enabled", 0);
        return true;
    }
    return node == S.key || node == S.remember;
}

void setup_mark_started() { S.started = true; }

void setup_free() { S = {}; }

void setup_error(const std::string& text) {
    if (S.root) set_text(S.error, text);
}
