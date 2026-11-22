#include "script.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>

#include "datafeed.h"
#include "embedded_files.h"
#include "lua.hpp"

ScriptHost g_scripts;

namespace {

const double kNaN = std::nan("");

// Per-state data, kept in the state's extra space.
struct StateInfo {
    std::vector<std::string>* sink = nullptr;  // print() and errors
    std::chrono::steady_clock::time_point deadline{};
    bool timed = false;
};

StateInfo*& info(lua_State* L) { return *static_cast<StateInfo**>(lua_getextraspace(L)); }

void hook(lua_State* L, lua_Debug*) {
    StateInfo* s = info(L);
    if (s && s->timed && std::chrono::steady_clock::now() > s->deadline) {
        s->timed = false;
        luaL_error(L, "script ran too long (endless loop?)");
    }
}

int l_print(lua_State* L) {
    std::string line;
    int n = lua_gettop(L);
    for (int i = 1; i <= n; ++i) {
        if (i > 1) line += " ";
        line += luaL_tolstring(L, i, nullptr);
        lua_pop(L, 1);
    }
    if (StateInfo* s = info(L); s && s->sink) s->sink->push_back(line);
    return 0;
}

int traceback(lua_State* L) {
    const char* msg = lua_tostring(L, 1);
    luaL_traceback(L, L, msg ? msg : "(error object is not a string)", 1);
    return 1;
}

const EmbeddedFile* embedded(const char* path) {
    for (int i = 0; i < kEmbeddedFileCount; ++i)
        if (!std::strcmp(kEmbeddedFiles[i].path, path)) return &kEmbeddedFiles[i];
    return nullptr;
}

std::string pretty(std::string name) {  // "range_minutes" -> "Range minutes"
    std::replace(name.begin(), name.end(), '_', ' ');
    if (!name.empty()) name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
    return name;
}

// Field readers on the table at `idx` (absolute index).
std::string get_str(lua_State* L, int idx, const char* k, const std::string& def = "") {
    lua_getfield(L, idx, k);
    std::string v = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : def;
    lua_pop(L, 1);
    return v;
}
double get_num(lua_State* L, int idx, const char* k, double def) {
    lua_getfield(L, idx, k);
    double v = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : def;
    lua_pop(L, 1);
    return v;
}
bool get_bool(lua_State* L, int idx, const char* k, bool def) {
    lua_getfield(L, idx, k);
    bool v = lua_isboolean(L, -1) ? lua_toboolean(L, -1) : def;
    lua_pop(L, 1);
    return v;
}
bool has_function(lua_State* L, int idx, const char* k) {
    lua_getfield(L, idx, k);
    bool v = lua_isfunction(L, -1);
    lua_pop(L, 1);
    return v;
}

int precision_of(double step) {
    int p = 0;
    for (double s = step; p < 4 && std::fabs(s - std::round(s)) > 1e-9; s *= 10) ++p;
    return p;
}

void read_params(lua_State* L, int t, ScriptDef& d) {
    lua_getfield(L, t, "params");
    if (lua_istable(L, -1)) {
        int pt = lua_gettop(L);
        for (lua_Integer i = 1; lua_rawgeti(L, pt, i) == LUA_TTABLE; ++i) {
            int e = lua_gettop(L);
            ParamDef p;
            lua_rawgeti(L, e, 1);
            p.name = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "param" + std::to_string(i);
            lua_pop(L, 1);
            p.label = get_str(L, e, "label", pretty(p.name));
            lua_rawgeti(L, e, 2);
            if (lua_isboolean(L, -1)) {
                p.kind = ParamDef::Bool, p.def = lua_toboolean(L, -1);
            } else if (lua_type(L, -1) == LUA_TSTRING) {
                p.kind = ParamDef::Choice;
                std::string dv = lua_tostring(L, -1);
                lua_getfield(L, e, "options");
                if (lua_istable(L, -1))
                    for (lua_Integer k = 1; lua_rawgeti(L, -1, k) == LUA_TSTRING; ++k) {
                        p.options.push_back(lua_tostring(L, -1));
                        lua_pop(L, 1);
                    }
                lua_pop(L, 2);  // the nil that ended the loop, options
                if (p.options.empty()) p.options.push_back(dv);
                auto it = std::find(p.options.begin(), p.options.end(), dv);
                p.def = it == p.options.end() ? 0 : static_cast<double>(it - p.options.begin());
            } else {
                bool integer = lua_isinteger(L, -1);
                p.def = lua_tonumber(L, -1);
                p.step = get_num(L, e, "step", integer ? 1 : 0.1);
                p.kind = integer && std::fabs(p.step - std::round(p.step)) < 1e-9 ? ParamDef::Integer : ParamDef::Number;
                p.precision = p.kind == ParamDef::Integer ? 0 : std::max(1, precision_of(p.step));
                p.min = get_num(L, e, "min", -1e9), p.max = get_num(L, e, "max", 1e9);
            }
            lua_pop(L, 1);  // default
            d.params.push_back(p);
            lua_pop(L, 1);  // entry
        }
        lua_pop(L, 1);  // the non-table that ended the loop
    }
    lua_pop(L, 1);  // params
}

void read_plots(lua_State* L, int t, ScriptDef& d) {
    lua_getfield(L, t, "plots");
    if (lua_istable(L, -1)) {
        int pt = lua_gettop(L);
        for (lua_Integer i = 1; lua_rawgeti(L, pt, i) == LUA_TTABLE; ++i) {
            int e = lua_gettop(L);
            PlotDef p;
            lua_rawgeti(L, e, 1);
            p.name = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "plot" + std::to_string(i);
            lua_pop(L, 1);
            std::string style = get_str(L, e, "style", "line");
            p.style = style == "histogram" ? PlotDef::Histogram : style == "markers" ? PlotDef::Markers
                    : style == "step" ? PlotDef::Step : PlotDef::Line;
            std::string col = get_str(L, e, "color");
            p.has_color = !col.empty();
            p.color = parse_color(col, {1, 1, 1, 1});
            p.alpha = static_cast<float>(get_num(L, e, "alpha", 1.0));
            p.width = static_cast<float>(get_num(L, e, "width", 1.4));
            p.color_by_bar = get_str(L, e, "color_by") == "bar";
            d.plots.push_back(p);
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    auto index = [&](const std::string& n) {
        for (size_t k = 0; k < d.plots.size(); ++k)
            if (d.plots[k].name == n) return static_cast<int>(k);
        return -1;
    };
    lua_getfield(L, t, "fills");
    if (lua_istable(L, -1)) {
        int ft = lua_gettop(L);
        for (lua_Integer i = 1; lua_rawgeti(L, ft, i) == LUA_TTABLE; ++i) {
            int e = lua_gettop(L);
            lua_rawgeti(L, e, 1);
            lua_rawgeti(L, e, 2);
            int a = lua_type(L, -2) == LUA_TSTRING ? index(lua_tostring(L, -2)) : -1;
            int b = lua_type(L, -1) == LUA_TSTRING ? index(lua_tostring(L, -1)) : -1;
            lua_pop(L, 2);
            if (a >= 0 && b >= 0) d.fills.push_back({a, b, static_cast<float>(get_num(L, e, "alpha", 0.08))});
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
}

}  // namespace

// ---------------------------------------------------------------- states
lua_State* new_script_state(std::string& err) {
    lua_State* L = luaL_newstate();
    info(L) = new StateInfo;
    for (auto [name, fn] : {std::pair<const char*, lua_CFunction>{"_G", luaopen_base}, {LUA_TABLIBNAME, luaopen_table},
                            {LUA_STRLIBNAME, luaopen_string}, {LUA_MATHLIBNAME, luaopen_math},
                            {LUA_UTF8LIBNAME, luaopen_utf8}, {LUA_COLIBNAME, luaopen_coroutine}}) {
        luaL_requiref(L, name, fn, 1);
        lua_pop(L, 1);
    }
    for (const char* unsafe : {"dofile", "loadfile"}) {  // no file access from scripts
        lua_pushnil(L);
        lua_setglobal(L, unsafe);
    }
    lua_pushcfunction(L, l_print);
    lua_setglobal(L, "print");
    lua_sethook(L, hook, LUA_MASKCOUNT, 100000);
    const EmbeddedFile* pre = embedded("lib/prelude.lua");
    if (!pre || luaL_loadbufferx(L, pre->data, pre->size, "@prelude.lua", "t") != LUA_OK || !call_script(L, 0, 0, err)) {
        if (pre && err.empty()) err = lua_tostring(L, -1);
        if (!pre) err = "prelude.lua missing from the build";
    }
    return L;
}

void close_script_state(lua_State* L) {
    delete info(L);
    info(L) = nullptr;
    lua_close(L);
}

void set_script_sink(lua_State* L, std::vector<std::string>* sink) { info(L)->sink = sink; }

void set_script_deadline(lua_State* L, double seconds) {
    StateInfo* s = info(L);
    s->timed = seconds > 0;
    s->deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<long>(seconds * 1000));
}

bool call_script(lua_State* L, int nargs, int nresults, std::string& err) {
    int base = lua_gettop(L) - nargs;
    lua_pushcfunction(L, traceback);
    lua_insert(L, base);
    int rc = lua_pcall(L, nargs, nresults, base);
    lua_remove(L, base);
    if (rc != LUA_OK) {
        err = lua_tostring(L, -1) ? lua_tostring(L, -1) : "error";
        lua_pop(L, 1);
        return false;
    }
    return true;
}

bool load_script(lua_State* L, ScriptDef& d) {
    int top = lua_gettop(L);
    // a definition can be loaded again (backtests load a copy into their own state): start from scratch,
    // or the params/plots would be appended twice and the second, default copy would win
    d.params.clear(), d.plots.clear(), d.fills.clear(), d.levels.clear();
    d.error.clear(), d.has_range = false, d.ref = -1;
    auto fail = [&](const std::string& e) {
        d.error = e;
        lua_settop(L, top);
        return false;
    };
    if (luaL_loadbufferx(L, d.source.data(), d.source.size(), ("@" + d.chunk).c_str(), "t") != LUA_OK)
        return fail(lua_tostring(L, -1));
    lua_newtable(L);  // the script's own globals; reads fall through to the shared ones
    lua_newtable(L);
    lua_pushglobaltable(L);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);
    lua_setupvalue(L, -2, 1);  // _ENV
    std::string err;
    set_script_deadline(L, 2.0);
    if (!call_script(L, 0, 1, err)) return fail(err);
    if (!lua_istable(L, -1)) return fail(d.chunk + ": the script must return indicator { ... } or strategy { ... }");
    int t = lua_gettop(L);
    std::string kind = get_str(L, t, "kind");
    if (kind != "indicator" && kind != "strategy") return fail(d.chunk + ": returned a table that is neither indicator nor strategy");
    if ((kind == "strategy") != d.strategy)
        return fail(d.chunk + ": a " + kind + " in the " + (d.strategy ? "strategies" : "indicators") + " folder");
    d.name = get_str(L, t, "name", d.id);
    d.description = get_str(L, t, "description");
    d.overlay = get_bool(L, t, "overlay", true);
    d.zero_based = get_bool(L, t, "zero_based", false);
    d.timeframe = std::max(1, static_cast<int>(get_num(L, t, "timeframe", 1)));
    d.warmup_days = std::max(0, static_cast<int>(get_num(L, t, "warmup_days", 0)));
    lua_getfield(L, t, "range");
    if (lua_istable(L, -1)) {
        lua_rawgeti(L, -1, 1), lua_rawgeti(L, -2, 2);
        if (lua_isnumber(L, -2) && lua_isnumber(L, -1))
            d.has_range = true, d.range_lo = lua_tonumber(L, -2), d.range_hi = lua_tonumber(L, -1);
        lua_pop(L, 2);
    }
    lua_pop(L, 1);
    lua_getfield(L, t, "levels");
    if (lua_istable(L, -1))
        for (lua_Integer i = 1; lua_rawgeti(L, -1, i) == LUA_TNUMBER; ++i) d.levels.push_back(lua_tonumber(L, -1)), lua_pop(L, 1);
    lua_settop(L, t);
    read_params(L, t, d);
    read_plots(L, t, d);
    if (!d.strategy && !has_function(L, t, "calc")) return fail(d.chunk + ": an indicator needs calc = function(bars, p, out)");
    if (d.strategy && !has_function(L, t, "on_bar")) return fail(d.chunk + ": a strategy needs on_bar = function(ctx, p, bar)");
    if (!d.strategy && d.plots.empty()) return fail(d.chunk + ": an indicator needs at least one plot");
    lua_settop(L, t);
    d.ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_settop(L, top);
    return true;
}

// ---------------------------------------------------------------- params
Params default_params(const ScriptDef& d) {
    Params p;
    for (const ParamDef& q : d.params) p.push_back(q.def);
    return p;
}

void push_params(lua_State* L, const ScriptDef& d, const Params& p) {
    lua_createtable(L, 0, static_cast<int>(d.params.size()));
    for (size_t k = 0; k < d.params.size(); ++k) {
        const ParamDef& q = d.params[k];
        double v = k < p.size() ? p[k] : q.def;
        switch (q.kind) {
            case ParamDef::Bool: lua_pushboolean(L, v != 0); break;
            case ParamDef::Choice: {
                size_t i = static_cast<size_t>(std::clamp(v, 0.0, static_cast<double>(q.options.size() - 1)));
                lua_pushstring(L, q.options[i].c_str());
                break;
            }
            case ParamDef::Integer: lua_pushinteger(L, static_cast<lua_Integer>(std::llround(v))); break;
            default: lua_pushnumber(L, v);
        }
        lua_setfield(L, -2, q.name.c_str());
    }
}

std::string script_label(lua_State* L, const ScriptDef& d, const Params& p) {
    if (d.ref >= 0) {
        int top = lua_gettop(L);
        lua_rawgeti(L, LUA_REGISTRYINDEX, d.ref);
        lua_getfield(L, -1, "label");
        if (lua_isfunction(L, -1)) {
            push_params(L, d, p);
            std::string err;
            set_script_deadline(L, 0.5);
            if (call_script(L, 1, 1, err) && lua_type(L, -1) == LUA_TSTRING) {
                std::string s = lua_tostring(L, -1);
                lua_settop(L, top);
                return s;
            }
        }
        lua_settop(L, top);
    }
    std::string s = d.name, sep = " ";
    char buf[32];
    for (size_t k = 0; k < d.params.size() && k < p.size(); ++k) {
        if (d.params[k].kind == ParamDef::Integer) std::snprintf(buf, sizeof buf, "%.0f", p[k]);
        else if (d.params[k].kind == ParamDef::Number) std::snprintf(buf, sizeof buf, "%g", p[k]);
        else continue;
        s += sep + buf, sep = "/";
    }
    return s;
}

int script_warmup(lua_State* L, const ScriptDef& d, const Params& p) {
    int w = -1;
    if (d.ref >= 0) {
        int top = lua_gettop(L);
        lua_rawgeti(L, LUA_REGISTRYINDEX, d.ref);
        lua_getfield(L, -1, "warmup");
        if (lua_isfunction(L, -1)) {
            push_params(L, d, p);
            std::string err;
            set_script_deadline(L, 0.5);
            if (call_script(L, 1, 1, err) && lua_isnumber(L, -1)) w = static_cast<int>(lua_tonumber(L, -1));
        }
        lua_settop(L, top);
    }
    if (w < 0) {
        double m = 0;
        for (size_t k = 0; k < d.params.size() && k < p.size(); ++k)
            if (d.params[k].kind == ParamDef::Integer) m = std::max(m, p[k]);
        w = static_cast<int>(4 * m + 10);
    }
    return std::clamp(w, 10, 20000);
}

// ---------------------------------------------------------------- indicators
bool run_indicator(const ScriptDef& d, const Params& p, const Series& s, int tf, long series_key, IndOutput& out) {
    lua_State* L = g_scripts.L;
    static long bars_key = -1;
    static int bars_ref = LUA_NOREF, bars_gen = -1;
    out = {};
    out.plots.assign(d.plots.size(), std::vector<double>(s.n(), kNaN));
    if (d.ref < 0 || !L) return out.error = d.error, false;
    int top = lua_gettop(L);
    if (series_key != bars_key || bars_gen != g_scripts.generation() || bars_ref == LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, bars_ref);
        int n = static_cast<int>(s.n());
        lua_createtable(L, 0, 10);
        const std::pair<const char*, const std::vector<double>*> cols[] = {
            {"open", &s.o}, {"high", &s.h}, {"low", &s.l}, {"close", &s.c}, {"volume", &s.v}};
        for (auto& [name, col] : cols) {
            lua_createtable(L, n, 0);
            for (int i = 0; i < n; ++i) lua_pushnumber(L, (*col)[i]), lua_rawseti(L, -2, i + 1);
            lua_setfield(L, -2, name);
        }
        lua_createtable(L, n, 0);
        for (int i = 0; i < n; ++i) lua_pushinteger(L, static_cast<lua_Integer>(s.t[i])), lua_rawseti(L, -2, i + 1);
        lua_setfield(L, -2, "time");
        lua_pushinteger(L, n), lua_setfield(L, -2, "n");
        lua_pushinteger(L, s.last_complete() + 1), lua_setfield(L, -2, "last_complete");
        lua_pushboolean(L, s.last_partial), lua_setfield(L, -2, "partial");
        lua_pushinteger(L, tf), lua_setfield(L, -2, "tf");
        bars_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        bars_key = series_key, bars_gen = g_scripts.generation();
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, d.ref);
    int def = lua_gettop(L);
    lua_getfield(L, def, "calc");
    lua_rawgeti(L, LUA_REGISTRYINDEX, bars_ref);
    push_params(L, d, p);
    lua_getglobal(L, "__new_output");
    lua_createtable(L, static_cast<int>(d.plots.size()), 0);
    for (size_t k = 0; k < d.plots.size(); ++k) lua_pushstring(L, d.plots[k].name.c_str()), lua_rawseti(L, -2, static_cast<int>(k + 1));
    std::string err;
    if (!call_script(L, 1, 1, err)) {
        lua_settop(L, top);
        return out.error = err, false;
    }
    lua_pushvalue(L, -1);
    int out_t = lua_gettop(L) - 4;  // calc, bars, params, out, out(copy)
    lua_insert(L, out_t);           // keep a copy below the call's arguments
    set_script_deadline(L, 2.0);
    if (!call_script(L, 3, 0, err)) {
        lua_settop(L, top);
        return out.error = err, false;
    }
    // stack: def, out
    int o = lua_gettop(L);
    long n = static_cast<long>(s.n());
    for (size_t k = 0; k < d.plots.size(); ++k) {
        lua_getfield(L, o, d.plots[k].name.c_str());
        if (lua_istable(L, -1))
            for (long i = 0; i < n; ++i) {
                if (lua_rawgeti(L, -1, i + 1) == LUA_TNUMBER) out.plots[k][i] = lua_tonumber(L, -1);
                lua_pop(L, 1);
            }
        lua_pop(L, 1);
    }
    auto plot_index = [&](int idx) {
        if (lua_type(L, idx) != LUA_TSTRING) return 0;
        std::string nm = lua_tostring(L, idx);
        for (size_t k = 0; k < d.plots.size(); ++k)
            if (d.plots[k].name == nm) return static_cast<int>(k);
        return 0;
    };
    lua_getfield(L, o, "_markers");
    for (lua_Integer k = 1; lua_rawgeti(L, -1, k) == LUA_TTABLE; ++k) {
        for (int f = 1; f <= 4; ++f) lua_rawgeti(L, -f, f);
        // stack: ..., entry, i, price, up, plot
        long i = static_cast<long>(lua_tointeger(L, -4)) - 1;
        if (i >= 0 && i < n) out.markers.push_back({i, lua_tonumber(L, -3), static_cast<bool>(lua_toboolean(L, -2)), plot_index(-1)});
        lua_pop(L, 5);
    }
    lua_pop(L, 2);
    lua_getfield(L, o, "_segments");
    for (lua_Integer k = 1; lua_rawgeti(L, -1, k) == LUA_TTABLE; ++k) {
        for (int f = 1; f <= 5; ++f) lua_rawgeti(L, -f, f);
        // stack: ..., entry, i0, i1, price, plot, alpha
        long i0 = static_cast<long>(lua_tointeger(L, -5)) - 1, i1 = static_cast<long>(lua_tointeger(L, -4)) - 1;
        if (i0 >= 0 && i0 < n)
            out.segments.push_back({i0, std::clamp(i1, i0, n - 1), lua_tonumber(L, -3), plot_index(-2),
                                    static_cast<float>(lua_isnumber(L, -1) ? lua_tonumber(L, -1) : 1.0)});
        lua_pop(L, 6);
    }
    lua_settop(L, top);
    return true;
}

// ---------------------------------------------------------------- host
fs::path ScriptHost::user_dir() const {
    if (const char* d = std::getenv("NQRT_SCRIPTS_DIR"); d && *d) return fs::path(d);  // another scripts folder
    return data_dir() / "scripts";
}

bool ScriptHost::init(std::string& err) {
    L = new_script_state(err);
    info(L)->sink = &messages;
    if (!err.empty()) return false;
    install_user_folder();
    reload();
    return true;
}

// The user's folder: indicators/ and strategies/ (loaded), examples/ (copies of the built-ins to start from,
// refreshed every launch, never loaded) and README.md (the API reference).
void ScriptHost::install_user_folder() {
    std::error_code ec;
    fs::path u = user_dir();
    for (const char* sub : {"indicators", "strategies", "examples/indicators", "examples/strategies"})
        fs::create_directories(u / sub, ec);
    auto write_if_changed = [&](const fs::path& p, const char* data, size_t size) {
        std::string cur;
        if (std::FILE* f = open_file(p, "rb")) {
            char buf[8192];
            for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) cur.append(buf, n);
            std::fclose(f);
        }
        if (cur.size() == size && !std::memcmp(cur.data(), data, size)) return;
        if (std::FILE* f = open_file(p, "wb")) std::fwrite(data, 1, size, f), std::fclose(f);
    };
    for (int i = 0; i < kEmbeddedFileCount; ++i) {
        const EmbeddedFile& e = kEmbeddedFiles[i];
        std::string path = e.path;
        if (path == "README.md") write_if_changed(u / "README.md", e.data, e.size);
        else if (path.rfind("indicators/", 0) == 0 || path.rfind("strategies/", 0) == 0)
            write_if_changed(u / "examples" / path, e.data, e.size);
    }
}

std::string ScriptHost::user_stamp() const {
    std::string s;
    std::error_code ec;
    for (const char* sub : {"indicators", "strategies"}) {
        std::vector<std::string> rows;
        for (auto it = fs::directory_iterator(user_dir() / sub, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
            if (it->path().extension() != ".lua") continue;
            auto t = fs::last_write_time(it->path(), ec).time_since_epoch().count();
            rows.push_back(it->path().filename().string() + ":" + std::to_string(fs::file_size(it->path(), ec)) + ":" +
                           std::to_string(static_cast<long long>(t)));
        }
        std::sort(rows.begin(), rows.end());
        for (auto& r : rows) s += std::string(sub) + "/" + r + ";";
    }
    return s;
}

void ScriptHost::reload() {
    for (ScriptDef& d : defs_) luaL_unref(L, LUA_REGISTRYINDEX, d.ref);
    defs_.clear();
    std::map<std::string, ScriptDef> by_key;  // "i:ema" / "s:orb"
    for (int i = 0; i < kEmbeddedFileCount; ++i) {
        std::string path = kEmbeddedFiles[i].path;
        bool strat = path.rfind("strategies/", 0) == 0;
        if (!strat && path.rfind("indicators/", 0) != 0) continue;
        ScriptDef d;
        d.strategy = strat;
        d.id = fs::path(path).stem().string();
        d.origin = "built-in";
        d.chunk = "built-in/" + path;
        d.source.assign(kEmbeddedFiles[i].data, kEmbeddedFiles[i].size);
        by_key[(strat ? "s:" : "i:") + d.id] = std::move(d);
    }
    std::error_code ec;
    for (const char* sub : {"indicators", "strategies"}) {
        for (auto it = fs::directory_iterator(user_dir() / sub, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
            if (it->path().extension() != ".lua") continue;
            ScriptDef d;
            d.strategy = std::string(sub) == "strategies";
            d.id = it->path().stem().string();
            d.origin = it->path().string();
            d.chunk = std::string(sub) + "/" + it->path().filename().string();
            if (std::FILE* f = open_file(it->path(), "rb")) {
                char buf[8192];
                for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) d.source.append(buf, n);
                std::fclose(f);
            }
            by_key[(d.strategy ? "s:" : "i:") + d.id] = std::move(d);
        }
    }
    for (auto& [key, d] : by_key) {
        if (!load_script(L, d)) messages.push_back(d.error);
        defs_.push_back(std::move(d));
    }
    std::stable_sort(defs_.begin(), defs_.end(), [](const ScriptDef& a, const ScriptDef& b) {
        std::string x = a.name.empty() ? a.id : a.name, y = b.name.empty() ? b.id : b.name;
        std::transform(x.begin(), x.end(), x.begin(), ::tolower);
        std::transform(y.begin(), y.end(), y.begin(), ::tolower);
        return x < y;
    });
    stamp_ = user_stamp();
    ++generation_;
}

bool ScriptHost::poll_changes() {
    if (user_stamp() == stamp_) return false;
    reload();
    return true;
}

std::vector<const ScriptDef*> ScriptHost::list(bool strategies) const {
    std::vector<const ScriptDef*> v;
    for (const ScriptDef& d : defs_)
        if (d.strategy == strategies) v.push_back(&d);
    return v;
}

const ScriptDef* ScriptHost::find(bool strategy, const std::string& id) const {
    for (const ScriptDef& d : defs_)
        if (d.strategy == strategy && d.id == id) return &d;
    return nullptr;
}

Rgba parse_color(const std::string& hex, Rgba fallback) {
    if (hex.size() != 7 && hex.size() != 9) return fallback;
    if (hex[0] != '#') return fallback;
    unsigned v[4] = {0, 0, 0, 255};
    for (size_t k = 0; k < (hex.size() - 1) / 2; ++k)
        if (std::sscanf(hex.c_str() + 1 + 2 * k, "%2x", &v[k]) != 1) return fallback;
    return {v[0] / 255.0f, v[1] / 255.0f, v[2] / 255.0f, v[3] / 255.0f};
}
