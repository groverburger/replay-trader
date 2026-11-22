// The data screen shown before the trader: asks for an API key when there is none, shows download progress, and
// offers the cached data when the download can't finish (offline, bad key).

#include <cmath>
#include <cstdio>
#include <string>

#include "imgui.h"
#include "app.h"
#include "datafeed.h"
#include "ui.h"

namespace {

struct {
    char key[256] = "";
    bool remember = true;
    std::string key_source, error;
    bool started = false, cached = false, have_saved = false;
    int state = -1;  // last busy state seen; the cache is re-read when it changes
    std::string cache;
} S;

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

void start() {
    std::string key = S.key;
    while (!key.empty() && (key.back() == ' ' || key.back() == '\n')) key.pop_back();
    if (key.empty()) {
        S.error = "Enter your API key first.";
        return;
    }
    if (S.remember) save_api_key(key);
    else if (S.key_source == "saved") forget_api_key();
    S.error.clear();
    S.started = true;
    downloader().start(key, -1);
    poke();
}

void separator(float w) {
    ImGui::Spacing();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + w, p.y + std::max(1.0f, 1.0f / px_scale())), ImGui::GetColorU32(ImGuiCol_Separator));
    ImGui::Dummy(ImVec2(w, 1));
    ImGui::Spacing();
}

}  // namespace

void setup_init(const std::string& key, const std::string& source) {
    S.key_source = source;
    std::snprintf(S.key, sizeof S.key, "%s", key.c_str());
    S.remember = source == "saved" || source.empty();
    S.have_saved = source == "saved";
}

void setup_draw() {
    Downloader& d = downloader();
    bool busy = d.running();
    if (S.state != busy) {
        S.state = busy;
        S.cache = cache_text();
        S.cached = cache_summary().bars > 0;
    }
    float s = ui_scale(), w = std::round(ImGui::CalcTextSize("takes about an hour. It runs newest first: stop whenever you like and trade").x + 4 * s);
    ImGuiStyle& st = ImGui::GetStyle();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float h = 16 * ImGui::GetFrameHeightWithSpacing();  // about the screen's height: centred at its natural size
    ImGui::SetCursorPos(ImVec2(std::floor((avail.x - w) / 2), std::max(st.WindowPadding.y, std::floor((avail.y - h) / 2))));
    ImGui::BeginGroup();
    ImGui::PushItemWidth(w);
    ImGui::PushFont(g_font_bold);
    ImGui::TextUnformatted("Replay Trader");
    ImGui::PopFont();
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    for (const char* line : {"NQ futures 1-minute bars (Dec 2009 to today) download from the karaiq",
                             "Futures Data API (api.karaiq.com) with your API key. The full history is",
                             "~4,400 requests and the API answers one at a time, so the first download",
                             "takes about an hour. It runs newest first: stop whenever you like and trade",
                             "what's there; the rest resumes next time. Later starts take seconds."})
        ImGui::TextUnformatted(line);
    ImGui::PopStyleColor();
    separator(w);

    ImGui::BeginDisabled(busy);
    bool enter = ImGui::InputTextWithHint("##key", "API key: X-API-Key for api.karaiq.com", S.key, sizeof S.key,
                                          ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::EndDisabled();
    ImGui::Checkbox("Remember the key on this computer", &S.remember);
    float bw = std::floor((w - 2 * st.ItemSpacing.x) / 3);
    ImGui::BeginDisabled(busy);
    if (ImGui::Button("Download data", ImVec2(bw, 0)) || (enter && !busy)) start();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!busy);
    if (ImGui::Button("Stop", ImVec2(bw, 0))) d.cancel();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(busy || !S.cached);
    if (ImGui::Button("Use cached data", ImVec2(bw, 0))) enter_trader();
    ImGui::EndDisabled();

    int done = d.done, total = d.total;
    float f = total > 0 ? static_cast<float>(done) / total : (S.started && d.finished() ? 1.0f : 0.0f);
    ImGui::ProgressBar(f, ImVec2(w, 0), "");
    std::string p;
    if (busy) {
        p = d.status();
        if (total > 0) p += "   " + with_commas(done) + " / " + with_commas(total) + " days";
    } else if (S.started && d.finished()) {
        p = d.error().empty() ? "Download complete." : d.error() == "cancelled" ? "Stopped." : "Download stopped.";
    }
    ImGui::TextUnformatted(p.empty() ? " " : p.c_str());
    if (S.started && d.finished() && !d.error().empty() && d.error() != "cancelled") {
        std::string e = d.error();
        if (e.find("rejected the key") != std::string::npos) e += ": check it and try again.";
        else if (S.cached) e += ". Press Download to retry, or use the cached data.";
        S.error = e;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(kSell.r, kSell.g, kSell.b, 1));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
    ImGui::TextWrapped("%s", S.error.empty() ? " " : S.error.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    separator(w);
    ImGui::TextDisabled("%s", S.cache.c_str());
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("Data folder: %s", data_dir().string().c_str());
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
    ImGui::BeginDisabled(!S.have_saved);
    if (ImGui::Button("Forget saved key", ImVec2(w, 0))) {
        forget_api_key();
        S.key_source.clear();
        S.key[0] = 0;
        S.have_saved = false;
    }
    ImGui::EndDisabled();
    ImGui::PopItemWidth();
    ImGui::EndGroup();
}

void setup_mark_started() { S.started = true; }

void setup_error(const std::string& text) { S.error = text; }
