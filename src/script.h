// Lua scripting: indicators and strategies are Lua files. The built-in ones are compiled into the app
// (scripts/ in the source tree); the user's own live in <data folder>/scripts/{indicators,strategies} and are
// picked up (and reloaded when edited) without recompiling. A user script with the same file name as a
// built-in replaces it. API reference: docs/SCRIPTING.md.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "market.h"
#include "rgba.h"

struct lua_State;
namespace fs = std::filesystem;

struct ParamDef {
    enum Kind { Number, Integer, Bool, Choice } kind = Number;
    std::string name, label;
    double def = 0, min = -1e9, max = 1e9, step = 1;
    int precision = 0;
    std::vector<std::string> options;  // Choice
};
using Params = std::vector<double>;    // one per ParamDef: the number, 0/1, or the option index

struct PlotDef {
    enum Style { Line, Histogram, Markers, Step } style = Line;
    std::string name;
    bool has_color = false;            // else the indicator's own colour (its swatch)
    Rgba color{1, 1, 1, 1};
    float alpha = 1, width = 1.4f;
    bool color_by_bar = false;         // histogram coloured by the bar's direction
};

struct FillDef {
    int a, b;                          // plot indices
    float alpha;
};

struct ScriptDef {
    std::string id;                    // file name without .lua
    bool strategy = false;
    std::string name, description;
    std::string origin;                // "built-in" or the file's path
    std::string source, chunk;         // code and chunk name (backtests load it into their own Lua state)
    std::string error;                 // load error: the script is listed but unusable
    std::vector<ParamDef> params;
    // indicators
    bool overlay = true, zero_based = false, has_range = false;
    double range_lo = 0, range_hi = 1;
    std::vector<double> levels;
    std::vector<PlotDef> plots;
    std::vector<FillDef> fills;
    // strategies
    int timeframe = 1;
    int ref = -1;                      // registry reference to the definition table (in g_scripts.L)
};

// A Lua state with the standard safe libraries and the prelude (scripts/lib/prelude.lua). Each thread that
// runs scripts needs its own.
lua_State* new_script_state(std::string& err);
void close_script_state(lua_State* L);
void set_script_sink(lua_State* L, std::vector<std::string>* sink);  // where print() goes
// Runs a script's source in its own environment and reads its definition table into `def` (keeping a registry
// reference to the table). Returns false with def.error set.
bool load_script(lua_State* L, ScriptDef& def);
// Limits a script call to `seconds` of run time (an endless loop raises an error instead of freezing).
void set_script_deadline(lua_State* L, double seconds);
// pcall with a traceback; on error pops nothing and returns the message.
bool call_script(lua_State* L, int nargs, int nresults, std::string& err);

Params default_params(const ScriptDef& d);
void push_params(lua_State* L, const ScriptDef& d, const Params& p);  // table: name -> value
std::string script_label(lua_State* L, const ScriptDef& d, const Params& p);
int script_warmup(lua_State* L, const ScriptDef& d, const Params& p);

struct IndMarker {
    long i;
    double price;
    bool up;
    int plot;
};
struct IndSegment {
    long i0, i1;
    double price;
    int plot;
    float alpha;
};
struct IndOutput {
    std::vector<std::vector<double>> plots;  // per PlotDef, one value per series bar (NaN where none)
    std::vector<IndMarker> markers;
    std::vector<IndSegment> segments;
    std::string error;
};
// Runs an indicator's calc over the series. `series_key` identifies the series contents (the bars table is
// built once per key and shared by all indicators).
bool run_indicator(const ScriptDef& d, const Params& p, const Series& s, int tf, long series_key, IndOutput& out);

class ScriptHost {
public:
    bool init(std::string& err);
    void reload();
    bool poll_changes();                       // reloads when a user script was added, changed or removed
    std::vector<const ScriptDef*> list(bool strategies) const;
    const ScriptDef* find(bool strategy, const std::string& id) const;
    int generation() const { return generation_; }
    fs::path user_dir() const;                 // <data folder>/scripts
    std::vector<std::string> messages;         // load errors and print() output, newest last
    lua_State* L = nullptr;

private:
    std::vector<ScriptDef> defs_;
    std::string stamp_;                        // user scripts' names, sizes and times at the last load
    int generation_ = 0;
    std::string user_stamp() const;
    void install_user_folder();
};
extern ScriptHost g_scripts;

Rgba parse_color(const std::string& hex, Rgba fallback);
