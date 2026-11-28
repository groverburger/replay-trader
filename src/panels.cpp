// The left-hand column: Replay, Order Ticket, Position & Orders, Indicators, Trades and Strategy panels, drawn
// every frame from the app state (Dear ImGui, immediate mode). Also the widgets they share: Blender-style number
// fields (drag to scrub, click to type an expression), rows of evenly split widgets, and smooth scrolling.

#include <algorithm>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"
#include "app.h"
#include "chart.h"
#include "civil.h"
#include "datafeed.h"
#include "ui.h"

namespace {

ImVec4 iv(Rgba c) { return ImVec4(c.r, c.g, c.b, c.a); }

// ---------------------------------------------------------------- smooth scrolling
// A scroll region's position is kept as a float here (ImGui rounds its own to whole units), moved 1:1 by
// trackpad deltas and eased toward the target by wheel notches.
struct SmoothScroll {
    ImGuiWindow* win = nullptr;
    float pos = 0, target = 0, last = -1;
};
std::vector<std::pair<std::string, SmoothScroll>> g_scrolls;
bool g_scroll_end = false;

SmoothScroll* find_scroll(const ImGuiWindow* w) {
    for (auto& s : g_scrolls)
        if (s.second.win == w) return &s.second;
    return nullptr;
}

// The region a scroll of dy should move: the innermost one under the pointer that can still move that way.
ImGuiWindow* scroll_target(float dy) {
    for (ImGuiWindow* w = ImGui::GetCurrentContext()->HoveredWindow; w; w = w->ParentWindow) {
        if (SmoothScroll* s = find_scroll(w)) {
            float max = w->ScrollMax.y;
            if ((dy > 0 && s->target > 0.5f) || (dy < 0 && s->target < max - 0.5f)) return w;
        }
        if (!(w->Flags & ImGuiWindowFlags_ChildWindow)) break;
    }
    return nullptr;
}

// ---------------------------------------------------------------- widget helpers
// A row of widgets sharing the available width by weight.
struct Row {
    std::vector<float> w;
    size_t i = 0;
    Row(std::initializer_list<float> weights) {
        float sum = 0, sp = ImGui::GetStyle().ItemSpacing.x, avail = ImGui::GetContentRegionAvail().x;
        for (float x : weights) sum += x;
        float room = avail - sp * (weights.size() - 1);
        for (float x : weights) w.push_back(std::floor(room * x / sum));
    }
    float next() {
        if (i) ImGui::SameLine();
        return w[std::min(i++, w.size() - 1)];
    }
};

bool button(const char* text, float w, float h = 0) { return ImGui::Button(text, ImVec2(w, h)); }

// A button that looks pressed while `on` (toggles, the timeframe row).
bool toggle_button(const char* text, bool on, float w) {
    if (on) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_CheckMark));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_CheckMark));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    }
    bool r = ImGui::Button(text, ImVec2(w, 0));
    if (on) ImGui::PopStyleColor(3);
    return r;
}

// Small frameless "x" button.
bool close_button(const char* id) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushID(id);
    bool r = ImGui::Button("\xC3\x97", ImVec2(ImGui::GetFrameHeight(), 0));  // U+00D7 multiplication sign
    ImGui::PopID();
    ImGui::PopStyleColor();
    return r;
}

void dim_text(const std::string& s) { ImGui::TextDisabled("%s", s.c_str()); }
void dim_wrapped(const std::string& s) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", s.c_str());
    ImGui::PopStyleColor();
}
void color_text(Rgba c, const std::string& s) { ImGui::TextColored(iv(c), "%s", s.c_str()); }

// Arithmetic for typed numbers: + - * / and parentheses ("20*1.5").
struct Expr {
    const char* p;
    bool ok = true;
    void ws() {
        while (*p == ' ') ++p;
    }
    double atom() {
        ws();
        if (*p == '(') {
            ++p;
            double v = sum();
            ws();
            if (*p == ')') ++p;
            else ok = false;
            return v;
        }
        if (*p == '-') return ++p, -atom();
        if (*p == '+') return ++p, atom();
        char* end = nullptr;
        double v = std::strtod(p, &end);
        if (end == p) ok = false;
        p = end;
        return v;
    }
    double product() {
        double v = atom();
        for (ws(); *p == '*' || *p == '/'; ws()) {
            char op = *p++;
            double r = atom();
            v = op == '*' ? v * r : v / r;
        }
        return v;
    }
    double sum() {
        double v = product();
        for (ws(); *p == '+' || *p == '-'; ws()) {
            char op = *p++;
            double r = product();
            v = op == '+' ? v + r : v - r;
        }
        return v;
    }
};
bool eval_expr(const char* s, double& out) {
    Expr e{s};
    double v = e.sum();
    e.ws();
    if (!e.ok || *e.p || !std::isfinite(v)) return false;
    out = v;
    return true;
}

struct {
    ImGuiID id = 0;
    char buf[64] = "";
    bool focus = false;
} g_edit;  // the number field being typed into

// A Blender-style number field: label on the left, value on the right; drag to scrub, click to type a value
// or an expression (Enter applies, Esc cancels).
bool number(const char* label, double& v, double lo, double hi, double step, int prec, float width,
            const char* suffix = "") {
    ImGui::PushID(label);
    ImGuiID id = ImGui::GetID("##num");
    bool changed = false;
    if (g_edit.id == id) {
        ImGui::SetNextItemWidth(width);
        if (g_edit.focus) ImGui::SetKeyboardFocusHere(), g_edit.focus = false;
        bool enter = ImGui::InputText("##edit", g_edit.buf, sizeof g_edit.buf,
                                      ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            g_edit.id = 0;
        } else if (enter || ImGui::IsItemDeactivated()) {
            double x;
            if (eval_expr(g_edit.buf, x)) {
                double m = std::pow(10.0, prec);
                v = std::clamp(std::round(x * m) / m, lo, hi);
                changed = true;
            }
            g_edit.id = 0;
        }
    } else {
        char fmt[16];
        std::snprintf(fmt, sizeof fmt, "%%.%df", prec);
        double speed = prec == 0 && hi - lo <= 200 ? 0.1 : std::max(step, prec == 0 ? 0.25 : step);
        ImGui::SetNextItemWidth(width);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, 0));  // drawn below: label left, value right
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGui::GetStyleColorVec4(ImGuiCol_Button));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
        changed = ImGui::DragScalar("##drag", ImGuiDataType_Double, &v, static_cast<float>(speed), &lo, &hi, fmt,
                                    ImGuiSliderFlags_NoInput | ImGuiSliderFlags_AlwaysClamp);
        ImGui::PopStyleColor(4);
        float thr = ImGui::GetIO().MouseDragThreshold;
        if (ImGui::IsItemDeactivated() && ImGui::GetIO().MouseDragMaxDistanceSqr[0] < thr * thr) {
            g_edit.id = id, g_edit.focus = true;
            std::snprintf(g_edit.buf, sizeof g_edit.buf, fmt, v);
        }
        ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        char val[64];
        std::snprintf(val, sizeof val, fmt, v);
        std::string right = std::string(val) + suffix;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
        float pad = ImGui::GetStyle().FramePadding.x * 2, ty = a.y + ImGui::GetStyle().FramePadding.y;
        float rw = ImGui::CalcTextSize(right.c_str()).x;
        ImGui::PushClipRect(a, b, true);
        dl->AddText(ImVec2(a.x + pad, ty), col, label);
        dl->AddText(ImVec2(b.x - pad - rw, ty), col, right.c_str());
        ImGui::PopClipRect();
    }
    ImGui::PopID();
    return changed;
}

bool number_int(const char* label, int& v, int lo, int hi, float width, const char* suffix = "") {
    double d = v;
    if (!number(label, d, lo, hi, 1, 0, width, suffix)) return false;
    v = static_cast<int>(d);
    return true;
}

// Dropdown showing "Prefix: item".
bool combo(const char* id, const char* prefix, int& v, const std::vector<std::string>& items, float width) {
    if (items.empty()) return false;
    v = std::clamp(v, 0, static_cast<int>(items.size()) - 1);
    std::string preview = prefix && *prefix ? std::string(prefix) + ": " + items[v] : items[v];
    bool changed = false;
    ImGui::SetNextItemWidth(width);
    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));  // the arrow: just a chevron
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_FrameBgHovered));
    bool open = ImGui::BeginCombo(id, preview.c_str(), ImGuiComboFlags_HeightLarge);
    ImGui::PopStyleColor(2);
    if (open) {
        for (int k = 0; k < static_cast<int>(items.size()); ++k) {
            ImGui::PushID(k);
            if (ImGui::Selectable(items[k].c_str(), k == v)) v = k, changed = true;
            if (k == v) ImGui::SetItemDefaultFocus();
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    return changed;
}

// A text field with a dim label inside it, on the left ("From  2025-01-01").
bool text_field(const char* label, char* buf, size_t size, float width, const char* hint, bool enter_returns = false) {
    ImGui::PushID(label);
    ImVec2 at = ImGui::GetCursorScreenPos();
    float lw = *label ? ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2 : 0;
    ImGui::SetNextItemWidth(width);
    if (lw > 0) ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x + lw, ImGui::GetStyle().FramePadding.y));
    bool r = ImGui::InputTextWithHint("##t", hint, buf, size, enter_returns ? ImGuiInputTextFlags_EnterReturnsTrue : 0);
    if (lw > 0) {
        ImGui::PopStyleVar();
        ImGui::GetWindowDrawList()->AddText(ImVec2(at.x + ImGui::GetStyle().FramePadding.x * 2, at.y + ImGui::GetStyle().FramePadding.y),
                                            ImGui::GetColorU32(ImGuiCol_TextDisabled), label);
    }
    ImGui::PopID();
    return r;
}

std::string fmt_str(const char* f, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

std::string money(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%+.0f", v);
    return buf;
}

std::string money_k(double v) {  // $12,345
    char buf[32];
    std::snprintf(buf, sizeof buf, "%s$%.0f", v < 0 ? "-" : "", std::fabs(v));
    std::string s = buf;
    size_t start = v < 0 ? 2 : 1;
    for (int i = static_cast<int>(s.size()) - 3; i > static_cast<int>(start); i -= 3) s.insert(static_cast<size_t>(i), ",");
    return s;
}

// ---------------------------------------------------------------- panel state
struct {
    char date[32] = "2024-08-05 09:25";
    char bt_from[16] = "", bt_to[16] = "";
    bool bt_dates = false;
    int bt_shown_sel = -2;   // the selection the trade list last scrolled to
    int add_index = 0;
    int strat_gen = -1;
} P;

// Number / checkbox / dropdown widgets for a script's params, two to a row.
void param_widgets(const ScriptDef& d, Params& p) {
    if (p.size() != d.params.size()) p = default_params(d);
    int in_row = 0;
    float sp = ImGui::GetStyle().ItemSpacing.x, half = std::floor((ImGui::GetContentRegionAvail().x - sp) / 2);
    for (size_t k = 0; k < d.params.size(); ++k) {
        const ParamDef& q = d.params[k];
        ImGui::PushID(static_cast<int>(k));
        if (q.kind == ParamDef::Bool) {
            if (in_row == 1) ImGui::SameLine();  // beside a lone number
            bool on = p[k] != 0;
            if (ImGui::Checkbox(q.label.c_str(), &on)) p[k] = on;
            in_row = 0;
            ImGui::PopID();
            continue;
        }
        if (in_row == 2) in_row = 0;
        if (in_row == 1) ImGui::SameLine();
        ++in_row;
        if (q.kind == ParamDef::Choice) {
            int v = static_cast<int>(p[k]);
            if (combo("##c", q.label.c_str(), v, q.options, half)) p[k] = v;
        } else {
            number(q.label.c_str(), p[k], q.min, q.max, q.step, q.precision, half);
        }
        ImGui::PopID();
    }
}

// ---------------------------------------------------------------- panels
// Session colours: one row per band (on, name, start, end, colour, remove), New York time.
void sessions_editor() {
    if (!ImGui::TreeNode("Sessions (New York time)")) return;
    auto time_field = [](const char* id, int& minutes, float w) {
        char buf[8];
        std::snprintf(buf, sizeof buf, "%02d:%02d", minutes / 60, minutes % 60);
        ImGui::SetNextItemWidth(w);
        if (ImGui::InputText(id, buf, sizeof buf, ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_AutoSelectAll)) {
            int h, m;
            if (std::sscanf(buf, "%d:%d", &h, &m) == 2 && h >= 0 && h <= 24 && m >= 0 && m < 60)
                minutes = std::min(24 * 60, h * 60 + m) % (24 * 60);
            else if (std::sscanf(buf, "%d", &h) == 1 && h >= 100 && h <= 2400 && h % 100 < 60)  // 0930
                minutes = (h / 100 * 60 + h % 100) % (24 * 60);
        }
    };
    int remove = -1;
    float s = ImGui::GetStyle().ItemSpacing.x, fh = ImGui::GetFrameHeight();
    for (size_t k = 0; k < g.bands.size(); ++k) {
        Band& b = g.bands[k];
        ImGui::PushID(static_cast<int>(k));
        float avail = ImGui::GetContentRegionAvail().x, tw = std::round(fh * 2.3f);
        ImGui::Checkbox("##on", &b.on);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(avail - (fh + s) * 3 - (tw + s) * 2 - ImGui::CalcTextSize("-").x - s);
        ImGui::InputText("##name", b.name, sizeof b.name);
        ImGui::SameLine();
        time_field("##start", b.start, tw);
        ImGui::SameLine(0, s * 0.5f);
        ImGui::TextUnformatted("-");
        ImGui::SameLine(0, s * 0.5f);
        time_field("##end", b.end, tw);
        ImGui::SameLine();
        float c[4] = {b.col.r, b.col.g, b.col.b, b.col.a};
        if (ImGui::ColorEdit4("##col", c, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf))
            b.col = {c[0], c[1], c[2], c[3]};
        ImGui::SameLine();
        if (close_button("x")) remove = static_cast<int>(k);
        ImGui::PopID();
    }
    if (remove >= 0) g.bands.erase(g.bands.begin() + remove);
    Row r{1, 1};
    if (button("+ Add session", r.next())) {
        Band b;
        std::snprintf(b.name, sizeof b.name, "Session %zu", g.bands.size() + 1);
        b.start = 10 * 60, b.end = 11 * 60, b.col = {0.3f, 0.8f, 0.6f, 0.06f};
        g.bands.push_back(b);
    }
    if (button("Defaults", r.next())) g.bands = default_bands();
    dim_wrapped("A bar takes the colour of the first ticked session holding its start time. The colour swatch sets "
                "the opacity too.");
    ImGui::TreePop();
}

void replay_panel() {
    {
        std::string clock = fmt_time(g.now(), g.hide_dates ? "%H:%M NY" : "%a %Y-%m-%d %H:%M NY");
        ImGui::TextUnformatted(clock.c_str());
        bool rth = in_rth(g.m.t[g.cursor]);
        const char* sess = rth ? "RTH" : "ETH";
        ImGui::SameLine(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(sess).x - ImGui::GetStyle().FramePadding.x);
        if (rth) color_text(kBuy, sess);
        else dim_text(sess);
    }
    Row r{1, 1, 1};
    if (button(g.playing ? "\xE2\x80\x96  Pause (Space)" : "\xE2\x96\xB6  Play (Space)", r.next())) g.playing = !g.playing;
    if (button("+1 min", r.next())) advance(1);
    if (button("Finish bar", r.next())) finish_chart_bar();
    Row r2{1, 1};
    if (button("To 09:30 open", r2.next())) skip_to_open();
    if (button("\xE2\x86\xBB  Random day", r2.next())) random_day();

    static std::vector<std::string> speeds;
    if (speeds.empty())
        for (int s : kSpeeds) speeds.push_back(std::to_string(s) + " min/s");
    combo("##speed", "Speed", g.speed_idx, speeds, ImGui::GetContentRegionAvail().x);

    ImGui::Checkbox("Skip overnight", &g.rth_only);
    ImGui::Checkbox("Hide dates", &g.hide_dates);
    ImGui::Checkbox("09:30 / 16:00 lines", &g.sessions);
    ImGui::SameLine();
    ImGui::Checkbox("Session colours", &g.bands_on);
    if (g.bands_on) sessions_editor();

    Row r3{1, 0.6f};
    if (text_field("", P.date, sizeof P.date, r3.next(), "YYYY-MM-DD HH:MM", true)) start_at(P.date);
    if (button("Start here", r3.next())) start_at(P.date);

    Row tf{1, 1, 1, 1, 1, 1, 1};
    for (int k = 0; k < kNumTfs; ++k) {
        ImGui::PushID(k);
        if (toggle_button(kTfNames[k], g.tf_idx == k, tf.next())) g.tf_idx = k, g.follow = true;
        ImGui::PopID();
    }
    Row r4{1, 0.7f};
    double vis = std::round(g.visible);
    if (number("Bars in view", vis, 10, 20000, 1, 0, r4.next())) {
        g.visible = vis;
        if (!g.follow) g.xmin = g.xmax - g.visible;
    }
    if (toggle_button("Follow (L)", g.follow, r4.next())) g.follow = !g.follow;
}

void ticket_panel() {
    Broker& b = g.broker;
    Row r{1, 1};
    static const std::vector<std::string> insts = {"NQ  $20/pt", "MNQ  $2/pt"};
    ImGui::BeginDisabled(b.pos != 0);
    if (combo("##inst", "", g.instrument, insts, r.next())) {
        b.mult = g.instrument ? 2.0 : 20.0;
        b.commission = g.instrument ? 0.62 : 2.15;
    }
    ImGui::EndDisabled();
    if (g.risk_size) {
        double risk = g.risk_usd;
        if (number("Risk", risk, 1, 1e6, 10, 0, r.next(), " $")) g.risk_usd = risk;
    } else {
        number_int("Qty", g.qty, 1, 100, r.next());
    }
    Row r2{1, 1};
    number_int("Slippage", b.slip_ticks, 0, 20, r2.next(), " ticks");
    number("Fee", b.commission, 0, 20, 0.01, 2, r2.next(), " $/side");
    ImGui::Checkbox("Limit fills on touch", &b.limit_touch);
    ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.5f + ImGui::GetCursorPosX() * 0.5f);
    ImGui::Checkbox("Size from risk $", &g.risk_size);

    // bracket: a stop and up to two targets, in points or ATRs
    ImGui::Checkbox("Bracket", &g.bracket);
    ImGui::SameLine();
    {
        float avail = ImGui::GetContentRegionAvail().x, sp = ImGui::GetStyle().ItemSpacing.x;
        float bw = std::floor((avail - sp) / 2);
        if (toggle_button("Points", g.br_unit == 0, bw)) g.br_unit = 0;
        ImGui::SameLine();
        if (toggle_button("ATR", g.br_unit == 1, bw)) g.br_unit = 1;
    }
    ImGui::BeginDisabled(!g.bracket);
    double atr = atr_pts();
    if (g.br_unit == 1) {
        static std::vector<std::string> tfs;
        if (tfs.empty())
            for (int k = 0; k < kNumTfs - 1; ++k) tfs.push_back(kTfNames[k]);  // intraday ATRs and 1D
        Row ra{1, 1};
        g.atr_tf_idx = std::min(g.atr_tf_idx, kNumTfs - 2);
        combo("##atrtf", "ATR on", g.atr_tf_idx, tfs, ra.next());
        number_int("Length", g.atr_len, 2, 200, ra.next());
        Row r3{1, 1, 1};
        number("Stop", g.stop_atr, 0, 50, 0.05, 2, r3.next(), " ATR");
        number("Tgt 1", g.t1_atr, 0, 50, 0.05, 2, r3.next(), " ATR");
        number("Tgt 2", g.t2_atr, 0, 50, 0.05, 2, r3.next(), " ATR");
    } else {
        Row r3{1, 1, 1};
        number("Stop", g.stop_pts, 0, 1000, 0.25, 2, r3.next(), " pt");
        number("Tgt 1", g.target_pts, 0, 1000, 0.25, 2, r3.next(), " pt");
        number("Tgt 2", g.target2_pts, 0, 1000, 0.25, 2, r3.next(), " pt");
    }
    double stop, t1, t2;
    bracket_now(stop, t1, t2);
    int q = order_qty();
    ImGui::BeginDisabled(t2 <= 0 || t1 <= 0);
    {
        Row r4{1, 1};
        number_int("Tgt 1 takes", g.t1_pct, 1, 99, r4.next(), " %");
        int q1 = t2 > 0 && t1 > 0 ? std::clamp(static_cast<int>(std::lround(q * g.t1_pct / 100.0)), 0, q) : q;
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        dim_text(t2 > 0 && t1 > 0 ? fmt_str("of %d: %d at Tgt 1, %d at Tgt 2", q, q1, q - q1) : "Tgt 2 off: Tgt 1 takes all");
        (void)r4;
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    {
        std::string s;
        if (g.br_unit == 1)
            s = atr > 0 ? fmt_str("ATR(%d, %s) %.2f pt.  ", g.atr_len, kTfNames[g.atr_tf_idx], atr) : "ATR: not enough bars yet.  ";
        if (g.bracket && stop > 0) s += fmt_str("Stop %.2f pt = $%.0f risk on %d.", stop, stop * b.mult * q, q);
        else if (g.bracket) s += "No stop.";
        else s += "No bracket: entries go in naked.";
        if (g.bracket && (t1 > 0 || t2 > 0)) s += fmt_str("  Tgt %.2f%s pt.", t1, t2 > 0 ? fmt_str(" / %.2f", t2).c_str() : "");
        dim_wrapped(s);
    }

    Row r5{1, 1, 1};
    float tall = std::round(ImGui::GetFrameHeight() * 1.4f);
    std::string buy = fmt_str("\xE2\x96\xB2  Buy %d  (B)", q), sell = fmt_str("\xE2\x96\xBC  Sell %d  (S)", q);
    if (button(buy.c_str(), r5.next(), tall)) market_order(1, q);
    if (button(sell.c_str(), r5.next(), tall)) market_order(-1, q);
    if (button("Flatten (X)", r5.next(), tall)) flatten();
    Row r6{1, 1};
    if (button("\xE2\x86\x94  Reverse", r6.next())) reverse();
    if (button("Cancel orders (C)", r6.next())) b.cancel_all();

    if (g.ticket_price == 0) g.ticket_price = g.last();
    Row r7{1, 0.3f};
    if (number("Price", g.ticket_price, 0, 1e6, kTick, 2, r7.next())) g.ticket_price = round_tick(g.ticket_price);
    if (button("Last", r7.next())) g.ticket_price = g.last();
    Row r8{1, 1, 1, 1};
    if (button("Buy Lmt", r8.next())) pending_order(1, OrderType::Limit, g.ticket_price);
    if (button("Sell Lmt", r8.next())) pending_order(-1, OrderType::Limit, g.ticket_price);
    if (button("Buy Stp", r8.next())) pending_order(1, OrderType::Stop, g.ticket_price);
    if (button("Sell Stp", r8.next())) pending_order(-1, OrderType::Stop, g.ticket_price);
    dim_wrapped("Or right-click the chart at a price. Market orders fill at the next minute's open.");
}

std::string order_text(const Order& o) {
    const Broker& b = g.broker;
    const char* type = o.type == OrderType::Market ? "MKT" : o.type == OrderType::Limit ? "LMT" : "STP";
    if (o.reduce_only && b.pos && o.side == (b.pos > 0 ? -1 : 1) && o.type != OrderType::Market) {
        double pnl = (o.price - b.avg) * (b.pos > 0 ? 1 : -1) * o.qty * b.mult;
        return fmt_str("%s %d @ %.2f  %s", o.type == OrderType::Stop ? "STOP" : "TARGET", o.qty, o.price, money_k(pnl).c_str());
    }
    if (o.type == OrderType::Market)
        return fmt_str("%s %d  next open%s", type, o.qty, o.reduce_only ? "  (exit)" : "");
    return fmt_str("%s %d @ %.2f", type, o.qty, o.price);
}

void position_panel() {
    const Broker& b = g.broker;
    double lp = g.last(), u = b.unrealized(lp), net = b.realized - b.fees;
    char buf[128];
    std::snprintf(buf, sizeof buf, "Position %s%d   avg %.2f   last %.2f", b.pos > 0 ? "+" : "", b.pos, b.avg, lp);
    ImGui::TextUnformatted(buf);
    float third = ImGui::GetContentRegionAvail().x / 3, x0 = ImGui::GetCursorPosX();
    color_text(u >= 0 ? kBuy : kSell, "Open " + money(u));
    ImGui::SameLine(x0 + third);
    color_text(net >= 0 ? kBuy : kSell, "Closed " + money(net));
    ImGui::SameLine(x0 + 2 * third);
    color_text(net + u >= 0 ? kBuy : kSell, "Total " + money(net + u));
    std::snprintf(buf, sizeof buf, "Closed P&L is after $%.2f fees.", b.fees);
    dim_text(buf);
    if (b.pos) {  // managing the position: scale in and out, move the stop
        int n = std::abs(b.pos), q = order_qty();
        Row r{1, 1, 1};
        std::string add = fmt_str("+ Add %d (A)", q), half = fmt_str("- Close %d (H)", std::max(1, n / 2));
        if (button(add.c_str(), r.next())) add_to_position();
        if (button(half.c_str(), r.next())) scale_out(std::max(1, n / 2));
        if (button("- Close 1", r.next())) scale_out(1);
        Row r2{1, 1};
        if (button("Stop to breakeven (E)", r2.next())) stop_to_breakeven();
        if (button("Flatten (X)##position", r2.next())) flatten();
        double risk = 0, reward = 0;
        bool stop = false;
        for (const Order& o : b.orders) {
            if (!o.reduce_only || o.type == OrderType::Market || o.side != (b.pos > 0 ? -1 : 1)) continue;
            double v = (o.price - b.avg) * (b.pos > 0 ? 1 : -1) * o.qty * b.mult;
            if (o.type == OrderType::Stop) risk += v, stop = true;
            else reward += v;
        }
        dim_text(stop ? fmt_str("At the stop %s.   At the targets %s.", money_k(risk).c_str(), money_k(reward).c_str())
                      : "No stop on this position: right-click the chart to set one.");
    }
    int cancel = 0;
    for (const Order& o : g.broker.orders) {
        if (o.managed && o.qty <= 0) continue;
        ImGui::PushID(o.id);
        float x = ImGui::GetCursorPosX(), w = ImGui::GetContentRegionAvail().x;
        ImGui::AlignTextToFramePadding();
        color_text(o.side > 0 ? kBuy : kSell, o.side > 0 ? "BUY" : "SELL");
        ImGui::SameLine(x + w * 0.16f);
        ImGui::TextUnformatted(order_text(o).c_str());
        ImGui::SameLine(x + w - ImGui::GetFrameHeight());
        if (close_button("x")) cancel = o.id;
        ImGui::PopID();
    }
    if (cancel) g.broker.cancel(cancel);
}

void indicators_panel() {
    std::vector<std::string> names, ids;
    for (const ScriptDef* d : g_scripts.list(false)) {
        names.push_back(d->error.empty() ? d->name : d->id + " (error)");
        ids.push_back(d->id);
    }
    if (names.empty()) {
        dim_text("No indicator scripts.");
    } else {
        Row r{1, 0.4f};
        combo("##add", "", P.add_index, names, r.next());
        if (button("+  Add", r.next()) && P.add_index < static_cast<int>(ids.size()))
            g.inds.push_back(make_ind(ids[P.add_index], g.next_uid++));
    }
    if (g.inds.empty()) dim_text("No indicators. Pick one and press Add.");
    int remove = -1;
    for (Ind& d : g.inds) {
        ImGui::PushID(d.uid);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.155f, 0.155f, 0.155f, 1));
        ImGui::BeginChild("##box", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        const ScriptDef* def = d.def();
        std::string title = def ? def->name : d.script + " (missing)";
        float w = ImGui::GetContentRegionAvail().x, x = ImGui::GetCursorPosX();
        ImGui::Checkbox(title.c_str(), &d.on);
        float sw = std::floor(w * 0.25f);
        ImGui::SameLine(x + w - sw - ImGui::GetFrameHeight() - ImGui::GetStyle().ItemSpacing.x);
        ImVec4 c = iv(d.col);
        if (ImGui::ColorButton("##sw", c, ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoAlpha, ImVec2(sw, ImGui::GetFrameHeight())))
            ImGui::OpenPopup("##picker");
        if (ImGui::BeginPopup("##picker")) {
            float v[3] = {d.col.r, d.col.g, d.col.b};
            if (ImGui::ColorPicker3("##p", v, ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoAlpha)) d.col = {v[0], v[1], v[2], 1};
            ImGui::EndPopup();
        }
        ImGui::SameLine(x + w - ImGui::GetFrameHeight());
        if (close_button("x")) remove = d.uid;
        if (def && !def->error.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, iv(kSell));
            ImGui::TextWrapped("%s", def->error.substr(0, def->error.find('\n')).c_str());
            ImGui::PopStyleColor();
        } else if (def) {
            param_widgets(*def, d.params);
            if (def->trades) {  // auto-trading: the indicator's entry order, on the timeframe it was ticked on
                bool on = d.trade;
                int tfk = 0;
                for (int k = 0; k < kNumTfs; ++k)
                    if (kTfs[k] == d.trade_tf) tfk = k;
                Row r{1, 1};
                ImGui::SetNextItemWidth(r.w[0]);
                if (ImGui::Checkbox("Auto-trade", &on)) set_ind_trade(d, on);
                static std::vector<std::string> tfs;
                if (tfs.empty())
                    for (int k = 0; k < kNumTfs - 1; ++k) tfs.push_back(std::string(kTfNames[k]) + " bars");
                ImGui::SameLine(ImGui::GetCursorPosX() + r.w[0] + ImGui::GetStyle().ItemSpacing.x);
                tfk = std::min(tfk, kNumTfs - 2);
                if (combo("##tradetf", "on", tfk, tfs, r.w[1])) {
                    d.trade_tf = kTfs[tfk];
                    if (d.trade && d.order_id) g.broker.cancel(d.order_id), d.order_id = 0;
                    if (d.trade) ind_trading();
                }
                if (d.trade) {
                    const Order* mine = nullptr;
                    for (const Order& o : g.broker.orders)
                        if (o.id == d.order_id) mine = &o;
                    if (mine) color_text(mine->side > 0 ? kBuy : kSell, "Working: " + order_text(*mine));
                    else if (g.broker.pos) dim_text("In a position: signals don't enter until flat.");
                    else dim_text("Waiting for a setup.");
                    if (d.trade_tf != g.tf()) {
                        ImGui::PushStyleColor(ImGuiCol_Text, iv(kSell));
                        ImGui::TextWrapped("Trading %s bars but the chart shows %s: the coloured candles won't match the trades.",
                                           kTfNames[tfk], kTfNames[g.tf_idx]);
                        ImGui::PopStyleColor();
                    }
                }
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopID();
    }
    if (remove >= 0)
        g.inds.erase(std::remove_if(g.inds.begin(), g.inds.end(), [remove](const Ind& i) { return i.uid == remove; }), g.inds.end());
    Row r{1, 1};
    if (button("Scripts folder", r.next())) open_scripts_folder();
    if (button("Reload scripts", r.next())) {
        g_scripts.reload();
        g.status = "scripts reloaded";
    }
    dim_text("Indicators are Lua scripts: add your own to the scripts folder.");
}

void trades_panel(float dt) {
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
    ImGui::TextUnformatted(buf);
    equity_draw(std::round(90 * ui_scale()));
    const float cols[] = {0.24f, 0.14f, 0.1f, 0.34f, 0.18f};  // column starts by weight
    auto table_row = [&](const char* const* cells, const Rgba* colors) {
        float x = ImGui::GetCursorPosX(), w = ImGui::GetContentRegionAvail().x, at = 0;
        for (int k = 0; k < 5; ++k) {
            if (k) ImGui::SameLine(x + at);
            ImGui::PushClipRect(ImGui::GetCursorScreenPos(), ImVec2(ImGui::GetCursorScreenPos().x + w * cols[k] - 4, ImGui::GetCursorScreenPos().y + 100), true);
            if (colors && colors[k].a > 0) color_text(colors[k], cells[k]);
            else ImGui::TextUnformatted(cells[k]);
            ImGui::PopClipRect();
            at += w * cols[k];
        }
    };
    static const char* head[] = {"In", "Side", "Qty", "Entry / exit", "$"};
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    table_row(head, nullptr);
    ImGui::PopStyleColor();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    if (begin_smooth_child("##trips", 0, std::round(140 * ui_scale()), 0, 0, dt)) {
        int shown = 0;
        for (auto it = tr.rbegin(); it != tr.rend() && shown < 200; ++it, ++shown) {
            std::string t = fmt_time(it->t_in, g.hide_dates ? "%H:%M" : "%m-%d %H:%M"), q = std::to_string(it->qty), m = money(it->pnl);
            char px[64];
            std::snprintf(px, sizeof px, "%.2f / %.2f", it->p_in, it->p_out);
            const char* cells[] = {t.c_str(), it->side > 0 ? "long" : "short", q.c_str(), px, m.c_str()};
            Rgba none{0, 0, 0, 0};
            Rgba colors[] = {none, it->side > 0 ? kBuy : kSell, none, none, it->pnl > 0 ? kBuy : kSell};
            table_row(cells, colors);
        }
        if (tr.empty()) dim_text("No closed trades.");
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    Row r{1, 1};
    if (button("Export CSV", r.next())) export_csv();
    if (button("Reset P&L", r.next())) new_session(g.cursor, "P&L reset");
}

// Net P&L, fees and win rate by the year each trade closed.
void by_year_table(const std::vector<RoundTrip>& trips) {
    auto year_of = [](int64_t t) { int y, mo, d; civil_from_days(floor_div(t, 1440), y, mo, d); return y; };
    struct Year { int year, n = 0, wins = 0; double net = 0, fee = 0; };
    std::vector<Year> ys;
    for (const RoundTrip& t : trips) {
        int y = year_of(t.t_out);
        if (ys.empty() || ys.back().year != y) ys.push_back({y});
        Year& e = ys.back();
        ++e.n, e.wins += t.pnl > 0, e.net += t.pnl, e.fee += t.fee;
    }
    float row_h = ImGui::GetTextLineHeightWithSpacing();
    float h = row_h * (std::min<int>(static_cast<int>(ys.size()), 12) + 1) + 6;
    ImGuiTableFlags tf = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("##bt_years", 5, tf, ImVec2(-1, h))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    for (const char* c : {"Year", "Trades", "Net", "Fees", "Win"}) ImGui::TableSetupColumn(c);
    ImGui::TableHeadersRow();
    for (const Year& e : ys) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(), ImGui::Text("%d", e.year);
        ImGui::TableNextColumn(), ImGui::Text("%d", e.n);
        ImGui::TableNextColumn(), color_text(e.net >= 0 ? kBuy : kSell, money_k(e.net));
        ImGui::TableNextColumn(), ImGui::TextUnformatted(money_k(e.fee).c_str());
        ImGui::TableNextColumn(), ImGui::Text("%.0f%%", 100.0 * e.wins / e.n);
    }
    ImGui::EndTable();
}

void strategy_panel() {
    std::vector<std::string> names, ids;
    for (const ScriptDef* d : g_scripts.list(true)) {
        names.push_back(d->error.empty() ? d->name : d->id + " (error)");
        ids.push_back(d->id);
    }
    if (P.strat_gen != g_scripts.generation()) {  // scripts (re)loaded
        bool first = P.strat_gen == -1;
        P.strat_gen = g_scripts.generation();
        if (!ids.empty() && (g.strat_id.empty() || !g_scripts.find(true, g.strat_id))) select_strategy(ids[0]);
        if (g.auto_trade && !first) start_strategy();  // the script changed: restart it with the new code
    }
    if (ids.empty()) {
        dim_text("No strategy scripts.");
    } else {
        int sel = static_cast<int>(std::find(ids.begin(), ids.end(), g.strat_id) - ids.begin());
        if (combo("##strategy", "Strategy", sel, names, ImGui::GetContentRegionAvail().x)) select_strategy(ids[sel]);
    }
    const ScriptDef* d = g_scripts.find(true, g.strat_id);
    if (d) {
        std::string desc = d->description;
        if (d->timeframe > 1) desc = std::to_string(d->timeframe) + "-minute bars. " + desc;
        if (!desc.empty()) dim_wrapped(desc);
        if (!d->error.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, iv(kSell));
            ImGui::TextWrapped("%s", d->error.substr(0, d->error.find('\n')).c_str());
            ImGui::PopStyleColor();
        } else {
            ImGui::PushID("sp");
            param_widgets(*d, g.strat_params);
            ImGui::PopID();
        }
    }
    bool on = g.auto_trade;
    if (ImGui::Checkbox("Auto-trade the replay", &on)) {
        if (on) start_strategy();
        else stop_strategy();
    }
    std::string st = g.auto_trade ? "Trading the replay on the paper account above."
                   : !g.strat.error().empty() ? "Stopped: " + g.strat.error().substr(0, g.strat.error().find('\n')) : "";
    if (!st.empty()) dim_wrapped(st);
    ImGui::Separator();
    if (!P.bt_dates && g.m.size()) {  // default: from the start of last year to the end of the data
        P.bt_dates = true;
        std::string to = fmt_time(g.m.t.back(), "%Y-%m-%d"), from = std::to_string(std::stoi(to.substr(0, 4)) - 1) + "-01-01";
        std::snprintf(P.bt_from, sizeof P.bt_from, "%s", from.c_str());
        std::snprintf(P.bt_to, sizeof P.bt_to, "%s", to.c_str());
    }
    Row r{1, 1};
    text_field("From", P.bt_from, sizeof P.bt_from, r.next(), "YYYY-MM-DD");
    text_field("To", P.bt_to, sizeof P.bt_to, r.next(), "YYYY-MM-DD");
    bool busy = g.bt.running();
    Row r2{1, 1};
    ImGui::BeginDisabled(busy);
    if (button("\xE2\x96\xB6  Run backtest", r2.next())) {
        int64_t from, to;
        if (!d || !d->error.empty()) g.status = "pick a working strategy first";
        else if (!parse_minutes(P.bt_from, from) || !parse_minutes(P.bt_to, to)) g.status = "backtest dates: YYYY-MM-DD";
        else {
            Broker costs = g.broker;  // the ticket's instrument, slippage and fee
            std::string err;
            costs.fee_schedule.clear();
            if (g.bt_fee_by_year) {
                costs.fee_schedule = g.bt_fees;
                std::sort(costs.fee_schedule.begin(), costs.fee_schedule.end());
                if (costs.fee_schedule.empty()) err = g.status = "add at least one year to the fee schedule";
            }
            if (err.empty()) g.bt.start(*d, g.strat_params, &g.m, from, to + 1440, costs, g.bt_qc_rolls);  // "to" is inclusive
        }
    }
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!busy);
    if (button("Stop", r2.next())) g.bt.cancel();
    ImGui::EndDisabled();
    ImGui::Checkbox("QuantConnect roll schedule", &g.bt_qc_rolls);
    // costs: the order ticket's instrument, slippage and fee, optionally with a fee schedule by year instead
    auto load = [](const char* hist) {
        std::string err;
        parse_fee_schedule(hist, g.bt_fees, err);
    };
    ImGui::PushID("btcost");  // the order ticket has fields with the same labels
    static bool seeded = false;  // start from the historical schedule for the ticket's instrument
    if (!seeded) seeded = true, load(g.instrument ? kMnqFeeHistory : kNqFeeHistory);
    ImGui::Checkbox("Fee by year", &g.bt_fee_by_year);
    if (!g.bt_fee_by_year)
        dim_wrapped(fmt_str("Costs from the order ticket: %s, %d tick%s slippage, $%.2f/side.", g.instrument ? "MNQ" : "NQ",
                            g.broker.slip_ticks, g.broker.slip_ticks == 1 ? "" : "s", g.broker.commission));
    if (g.bt_fee_by_year) {
        int drop = -1;
        for (int k = 0; k < static_cast<int>(g.bt_fees.size()); ++k) {
            ImGui::PushID(k);
            float x = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x;
            float avail = ImGui::GetContentRegionAvail().x - x, sp = ImGui::GetStyle().ItemSpacing.x;
            float w = std::floor((avail - sp) / 2);
            number_int("From", g.bt_fees[k].first, 1990, 2100, w);
            ImGui::SameLine();
            number("Fee", g.bt_fees[k].second, 0, 20, 0.01, 2, w, " $/side");
            ImGui::SameLine();
            if (close_button("x")) drop = k;
            ImGui::PopID();
        }
        if (drop >= 0) g.bt_fees.erase(g.bt_fees.begin() + drop);
        Row rf{1, 1, 1};
        if (button("+ Year", rf.next())) {
            std::pair<int, double> last = g.bt_fees.empty() ? std::pair<int, double>{2010, g.broker.commission}
                                                            : *std::max_element(g.bt_fees.begin(), g.bt_fees.end());
            g.bt_fees.push_back({last.first + 1, last.second});
        }
        if (button("NQ history", rf.next())) load(kNqFeeHistory);
        if (button("MNQ history", rf.next())) load(kMnqFeeHistory);
        dim_wrapped(fmt_str("Each fee applies from that year until the next, instead of the ticket's fee (instrument "
                            "and slippage still come from the ticket: %s, %d tick%s). History: CME exchange + NFA fees "
                            "plus a $0.85 (NQ) / $0.25 (MNQ) broker commission.", g.instrument ? "MNQ" : "NQ",
                            g.broker.slip_ticks, g.broker.slip_ticks == 1 ? "" : "s"));
    }
    ImGui::PopID();
    const BacktestResult& res = g.bt_result;
    float f = busy ? static_cast<float>(g.bt.progress()) : (res.trips.empty() && res.error.empty() ? 0.0f : 1.0f);
    ImGui::ProgressBar(f, ImVec2(-1, std::round(ImGui::GetFrameHeight() * 0.8f)), "");
    if (g.bt_seen != 0) {
        if (!res.error.empty()) color_text(kSell, res.error.substr(0, res.error.find('\n')));
        if (res.trips.empty() && res.error.empty()) dim_text("No trades.");
        if (!res.trips.empty()) {
            const BacktestStats& s = res.stats;
            char buf[200];
            ImGui::TextWrapped("%s", res.params.c_str());
            std::snprintf(buf, sizeof buf, "Net %s   %d trades   win %.0f%%   PF %.2f", money_k(s.net).c_str(), s.trades,
                          100.0 * s.wins / std::max(1, s.trades), s.gross_loss > 0 ? s.gross_win / s.gross_loss : 0.0);
            color_text(s.net >= 0 ? kBuy : kSell, buf);
            int losses = s.trades - s.wins;
            std::snprintf(buf, sizeof buf, "Avg win %s   avg loss %s   %s / trade", money_k(s.wins ? s.gross_win / s.wins : 0).c_str(),
                          money_k(losses ? -s.gross_loss / losses : 0).c_str(), money_k(s.net / std::max(1, s.trades)).c_str());
            ImGui::TextUnformatted(buf);
            std::snprintf(buf, sizeof buf, "Max drawdown %s   Sharpe %.2f (daily)   days up %.0f%%", money_k(s.max_dd).c_str(), s.sharpe,
                          s.pct_days_up);
            ImGui::TextUnformatted(buf);
            std::snprintf(buf, sizeof buf, "Best %s   worst %s   longest losing run %d", money_k(s.largest_win).c_str(),
                          money_k(s.largest_loss).c_str(), s.max_consec_losses);
            ImGui::TextUnformatted(buf);
            std::snprintf(buf, sizeof buf, "Avg hold %.0f min   fees %s   %s to %s, %d sessions%s", s.avg_minutes, money_k(s.fees).c_str(),
                          fmt_time(res.from, "%Y-%m-%d").c_str(), fmt_time(res.to, "%Y-%m-%d").c_str(), s.days,
                          res.qc_rolls ? ", QuantConnect rolls" : "");
            dim_wrapped(buf);
            if (!res.costs.empty()) dim_wrapped("Costs: " + res.costs);
            ImGui::Checkbox("Results by year", &g.bt_by_year);
            if (g.bt_by_year) by_year_table(res.trips);
        }
    }
    backtest_curve_draw(std::round(56 * ui_scale()));
    if (!res.trips.empty() && !busy) {
        int n = static_cast<int>(res.trips.size());
        Row rn{1, 1.3f, 1};
        if (button("\xE2\x97\x80  Prev", rn.next())) step_trade(-1);
        float w = rn.next(), cx = ImGui::GetCursorPosX();
        std::string pos = g.bt_sel >= 0 ? "Trade " + std::to_string(g.bt_sel + 1) + " of " + std::to_string(n)
                                        : std::to_string(n) + " trades";
        ImGui::SetCursorPosX(cx + std::max(0.0f, (w - ImGui::CalcTextSize(pos.c_str()).x) * 0.5f));
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(pos.c_str());
        ImGui::SameLine();
        ImGui::SetCursorPosX(cx + w + ImGui::GetStyle().ItemSpacing.x);
        if (button("Next  \xE2\x96\xB6", rn.w[2])) step_trade(1);
        ImGui::Checkbox("Show trades on chart", &g.bt_show);
        ImGui::SameLine();
        dim_text("[ and ] step");
        // the trade list: click a row to jump to that trade
        float row_h = ImGui::GetTextLineHeightWithSpacing();
        ImGuiTableFlags tf = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_SizingStretchProp;
        if (ImGui::BeginTable("##bt_trades", 4, tf, ImVec2(-1, row_h * 9 + 6))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthStretch, 0.6f);
            ImGui::TableSetupColumn("Entry", ImGuiTableColumnFlags_WidthStretch, 2.6f);
            ImGui::TableSetupColumn("Side", ImGuiTableColumnFlags_WidthStretch, 0.9f);
            ImGui::TableSetupColumn("P&L", ImGuiTableColumnFlags_WidthStretch, 1.2f);
            ImGui::TableHeadersRow();
            bool scroll = P.bt_shown_sel != g.bt_sel && g.bt_sel >= 0;
            ImGuiListClipper clip;
            clip.Begin(n);
            if (scroll) clip.IncludeItemByIndex(g.bt_sel);
            while (clip.Step()) {
                for (int k = clip.DisplayStart; k < clip.DisplayEnd; ++k) {
                    const RoundTrip& t = res.trips[k];
                    ImGui::TableNextRow();
                    if (k == g.bt_sel) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, IM_COL32(66, 110, 190, 150));
                    ImGui::TableNextColumn();
                    char id[24];
                    std::snprintf(id, sizeof id, "%d##tr%d", k + 1, k);
                    if (ImGui::Selectable(id, false, ImGuiSelectableFlags_SpanAllColumns)) goto_trade(k);
                    if (scroll && k == g.bt_sel) ImGui::SetScrollHereY(0.5f);
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(fmt_time(t.t_in, g.hide_dates ? "%H:%M" : "%Y-%m-%d %H:%M").c_str());
                    ImGui::TableNextColumn();
                    color_text(t.side > 0 ? kBuy : kSell, t.side > 0 ? "long" : "short");
                    ImGui::TableNextColumn();
                    color_text(t.pnl >= 0 ? kBuy : kSell, money_k(t.pnl));
                }
            }
            if (scroll) P.bt_shown_sel = g.bt_sel;
            ImGui::EndTable();
        }
        if (res.qc_rolls) dim_wrapped("Run with the QuantConnect roll schedule: near a roll, trades can sit off the chart's bars.");
    }
    Row r3{1, 1};
    ImGui::BeginDisabled(res.trips.empty());
    if (button("Export trades", r3.next())) {
        std::filesystem::path dir = data_dir() / "sessions";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        std::time_t now = std::time(nullptr);
        char name[64];
        std::strftime(name, sizeof name, "backtest_%Y%m%d_%H%M%S.csv", std::localtime(&now));
        g.status = export_trips_csv(dir / name, res.trips) ? "saved " + (dir / name).string() : "could not save the CSV";
    }
    ImGui::EndDisabled();
    if (button("Scripts folder##2", r3.next())) open_scripts_folder();
}

bool header(const char* title) {
    return ImGui::CollapsingHeader(title, ImGuiTreeNodeFlags_DefaultOpen);
}

}  // namespace

// ---------------------------------------------------------------- public
bool begin_smooth_child(const char* id, float w, float h, int child_flags, int window_flags, float dt) {
    SmoothScroll* st = nullptr;
    for (auto& s : g_scrolls)
        if (s.first == id) st = &s.second;
    if (!st) g_scrolls.push_back({id, SmoothScroll{}}), st = &g_scrolls.back().second;
    if (ImGuiWindow* win = st->win) {
        float actual = win->Scroll.y, max = win->ScrollMax.y;
        if (st->last < 0 || std::fabs(actual - IM_ROUND(st->last)) > 0.5f) st->pos = st->target = actual;  // the scrollbar moved it
        for (ScrollEvent& e : frame_scroll()) {
            if (e.dy == 0 || scroll_target(e.dy) != win) continue;
            if (e.notch) st->target = std::clamp(st->target - e.dy, 0.0f, max);
            else st->pos = st->target = std::clamp(st->pos - e.dy, 0.0f, max);
            e.dy = 0;
        }
        if (g_scroll_end && std::string(id) == "##sidebar") st->pos = st->target = max;
        st->target = std::clamp(st->target, 0.0f, max);
        if (st->pos != st->target) {
            st->pos += (st->target - st->pos) * (1.0f - std::exp(-dt * 18.0f));
            if (std::fabs(st->target - st->pos) < 0.25f) st->pos = st->target;
            else keep_animating();
        }
        st->pos = std::clamp(st->pos, 0.0f, max);
        if (IM_ROUND(st->pos) != actual) ImGui::SetNextWindowScroll(ImVec2(-1.0f, st->pos));
        st->last = st->pos;
    }
    bool open = ImGui::BeginChild(id, ImVec2(w, h), child_flags, window_flags | ImGuiWindowFlags_NoScrollWithMouse);
    st->win = ImGui::GetCurrentWindow();
    return open;
}

void panels_scroll_to_end() { g_scroll_end = true; }

void panels_draw(float dt) {
    float width = std::round(420 * ui_scale());
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6 * ui_scale(), 6 * ui_scale()));
    begin_smooth_child("##sidebar", width, 0, ImGuiChildFlags_AlwaysUseWindowPadding, 0, dt);
    ImGui::PopStyleVar();
    ImGui::PushItemWidth(-1);
    if (header("Replay")) replay_panel();
    if (header("Order Ticket")) ticket_panel();
    if (header("Position & Orders")) position_panel();
    if (header("Indicators")) indicators_panel();
    if (header("Trades")) trades_panel(dt);
    if (header("Strategy (Lua)")) strategy_panel();
    const auto& log = g.broker.log;
    for (size_t k = log.size() > 6 ? log.size() - 6 : 0; k < log.size(); ++k) dim_wrapped(log[k]);
    if (!g.status.empty()) dim_wrapped(g.status);
    ImGui::PopItemWidth();
    ImGui::EndChild();
}
