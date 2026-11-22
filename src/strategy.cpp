#include "strategy.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

#include "civil.h"
#include "datafeed.h"
#include "lua.hpp"

namespace {

template <int (StrategyRun::*F)(lua_State*)>
int bound(lua_State* L) {
    return (static_cast<StrategyRun*>(lua_touserdata(L, lua_upvalueindex(1)))->*F)(L);
}

int64_t session_of(int64_t t) { return floor_div(t + 360, 1440); }  // Globex session: 18:00 starts the next

std::string clock(int64_t t) {
    int64_t day = floor_div(t, 1440), tod = t - day * 1440;
    int y, m, d;
    civil_from_days(day, y, m, d);
    char b[32];
    std::snprintf(b, sizeof b, "%04d-%02d-%02d %02d:%02d", y, m, d, static_cast<int>(tod / 60), static_cast<int>(tod % 60));
    return b;
}

const char* type_name(OrderType t) { return t == OrderType::Market ? "market" : t == OrderType::Limit ? "limit" : "stop"; }

}  // namespace

// ---------------------------------------------------------------- run
bool StrategyRun::start(lua_State* L, const ScriptDef& def, const Params& p, Broker* broker, const Market* m,
                        const Agg* agg, size_t at, std::vector<std::string>* log) {
    stop();
    error_.clear();
    L_ = L, b_ = broker, m_ = m, agg_ = def.timeframe > 1 ? agg : nullptr, log_ = log, name_ = def.name, i_ = at;
    if (def.ref < 0) return fail(def.error.empty() ? "the strategy did not load" : def.error), false;
    lua_rawgeti(L, LUA_REGISTRYINDEX, def.ref);
    ref_ = luaL_ref(L, LUA_REGISTRYINDEX);  // our own reference: survives a script reload
    push_params(L, def, p);
    p_ref_ = luaL_ref(L, LUA_REGISTRYINDEX);

    lua_newtable(L);  // core: the C functions behind ctx (prelude.lua's __make_ctx)
    const std::pair<const char*, lua_CFunction> fns[] = {
        {"get", bound<&StrategyRun::get_bar>},          {"count", bound<&StrategyRun::count>},
        {"now", bound<&StrategyRun::now>},              {"position", bound<&StrategyRun::position>},
        {"last", bound<&StrategyRun::last>},            {"submit", bound<&StrategyRun::submit>},
        {"cancel", bound<&StrategyRun::cancel>},        {"cancel_all", bound<&StrategyRun::cancel_all>},
        {"modify", bound<&StrategyRun::modify>},        {"orders", bound<&StrategyRun::orders>},
        {"log", bound<&StrategyRun::log>},              {"point_value", bound<&StrategyRun::point_value>}};
    for (auto& [name, fn] : fns) {
        lua_pushlightuserdata(L, this);
        lua_pushcclosure(L, fn, 1);
        lua_setfield(L, -2, name);
    }
    lua_getglobal(L, "__make_ctx");
    lua_insert(L, -2);
    std::string err;
    if (!call_script(L, 1, 1, err)) return fail(err), false;
    ctx_ref_ = luaL_ref(L, LUA_REGISTRYINDEX);

    if (agg_) {  // the last strategy bar that has fully closed at minute `at`
        long b = static_cast<long>(agg_->bucket_of(at));
        bool last_minute = static_cast<size_t>(b + 1) < agg_->first.size() && agg_->first[b + 1] == static_cast<int64_t>(at + 1);
        done_ = last_minute ? b : b - 1;
    } else {
        done_ = static_cast<long>(at);
    }
    session_ = session_of(m->t[at]);
    fills_seen_ = broker->fills.size();
    if (!call("init", 0) || !call("on_day", 0)) return false;
    return true;
}

void StrategyRun::stop() {
    if (L_) {
        for (int* r : {&ref_, &ctx_ref_, &p_ref_}) {
            if (*r >= 0) luaL_unref(L_, LUA_REGISTRYINDEX, *r);
            *r = -2;
        }
    }
    ref_ = ctx_ref_ = p_ref_ = -2;
}

void StrategyRun::fail(const std::string& e) {
    error_ = e;
    if (log_) log_->push_back(name_ + " stopped: " + e);
}

// def[fn](ctx, p, <nargs values already pushed>); a missing optional function is fine.
bool StrategyRun::call(const char* fn, int nargs) {
    if (!running()) {
        lua_pop(L_, nargs);
        return false;
    }
    int base = lua_gettop(L_) - nargs;
    lua_rawgeti(L_, LUA_REGISTRYINDEX, ref_);
    lua_getfield(L_, -1, fn);
    lua_remove(L_, -2);
    if (!lua_isfunction(L_, -1)) {
        lua_settop(L_, base);
        return true;
    }
    lua_insert(L_, base + 1);
    lua_rawgeti(L_, LUA_REGISTRYINDEX, ctx_ref_);
    lua_insert(L_, base + 2);
    lua_rawgeti(L_, LUA_REGISTRYINDEX, p_ref_);
    lua_insert(L_, base + 3);
    std::string err;
    set_script_deadline(L_, 1.0);
    bool ok = call_script(L_, nargs + 2, 0, err);
    lua_settop(L_, base);
    if (!ok) fail(err);
    return ok;
}

void StrategyRun::after_minute(size_t i) {
    if (!running()) return;
    i_ = i;
    for (; fills_seen_ < b_->fills.size() && running(); ++fills_seen_) {
        const Fill& f = b_->fills[fills_seen_];
        lua_createtable(L_, 0, 6);
        lua_pushinteger(L_, f.side), lua_setfield(L_, -2, "side");
        lua_pushinteger(L_, f.qty), lua_setfield(L_, -2, "qty");
        lua_pushnumber(L_, f.price), lua_setfield(L_, -2, "price");
        lua_pushinteger(L_, f.time), lua_setfield(L_, -2, "time");
        lua_pushstring(L_, f.why.c_str()), lua_setfield(L_, -2, "why");
        lua_pushboolean(L_, f.protective), lua_setfield(L_, -2, "protective");
        call("on_fill", 1);
    }
    int64_t s = session_of(m_->t[i]);
    if (s != session_) {
        session_ = s;
        if (!call("on_day", 0)) return;
    }
    long b;
    bool closed;
    if (agg_) {
        b = static_cast<long>(agg_->bucket_of(i));
        closed = static_cast<size_t>(b + 1) < agg_->first.size() ? agg_->first[b + 1] == static_cast<int64_t>(i + 1) : false;
    } else {
        b = static_cast<long>(i), closed = true;
    }
    if (!closed || b <= done_) return;
    done_ = b;
    lua_createtable(L_, 0, 6);
    for (int f = 1; f <= 6; ++f) {
        lua_pushinteger(L_, 0);  // get_bar(0, f) pushes the value
        lua_pushinteger(L_, f);
        get_bar(L_);
        lua_remove(L_, -2), lua_remove(L_, -2);
        static const char* names[] = {"", "open", "high", "low", "close", "volume", "time"};
        lua_setfield(L_, -2, names[f]);
    }
    call("on_bar", 1);
}

// ---------------------------------------------------------------- bindings
int StrategyRun::get_bar(lua_State* L) {
    long k = static_cast<long>(luaL_checkinteger(L, -2)), f = static_cast<long>(luaL_checkinteger(L, -1));
    long idx = done_ - k;
    if (k < 0 || idx < 0) return lua_pushnil(L), 1;
    if (agg_) {
        const Agg& a = *agg_;
        const std::vector<double>* cols[] = {&a.o, &a.h, &a.l, &a.c, &a.v};
        if (f == 6) lua_pushinteger(L, a.t[idx]);
        else if (f >= 1 && f <= 5) lua_pushnumber(L, (*cols[f - 1])[idx]);
        else lua_pushnil(L);
    } else {
        const Market& m = *m_;
        const std::vector<float>* cols[] = {&m.o, &m.h, &m.l, &m.c, &m.v};
        if (f == 6) lua_pushinteger(L, m.t[idx]);
        else if (f >= 1 && f <= 5) lua_pushnumber(L, (*cols[f - 1])[idx]);
        else lua_pushnil(L);
    }
    return 1;
}

int StrategyRun::count(lua_State* L) { return lua_pushinteger(L, done_ + 1), 1; }
int StrategyRun::now(lua_State* L) { return lua_pushinteger(L, m_->t[i_] + 1), 1; }
int StrategyRun::last(lua_State* L) { return lua_pushnumber(L, m_->c[i_]), 1; }
int StrategyRun::point_value(lua_State* L) { return lua_pushnumber(L, b_->mult), 1; }

int StrategyRun::position(lua_State* L) {
    lua_pushinteger(L, b_->pos);
    lua_pushnumber(L, b_->avg);
    lua_pushnumber(L, b_->unrealized(m_->c[i_]));
    lua_pushnumber(L, b_->realized - b_->fees);
    return 4;
}

int StrategyRun::submit(lua_State* L) {
    int side = luaL_checkinteger(L, 1) > 0 ? 1 : -1;
    int qty = static_cast<int>(luaL_checkinteger(L, 2));
    std::string kind = luaL_checkstring(L, 3);
    double price = luaL_optnumber(L, 4, 0), sl = luaL_optnumber(L, 5, 0), tp = luaL_optnumber(L, 6, 0);
    if (qty <= 0) return luaL_error(L, "order quantity must be at least 1");
    Order o;
    o.side = side, o.qty = qty, o.born = i_;
    o.type = kind == "limit" ? OrderType::Limit : kind == "stop" ? OrderType::Stop : OrderType::Market;
    if (o.type != OrderType::Market) {
        if (!(price > 0)) return luaL_error(L, "%s order needs a price", kind.c_str());
        o.price = std::round(price / kTick) * kTick;
    }
    o.stop_pts = std::max(0.0, sl), o.target_pts = std::max(0.0, tp);
    return lua_pushinteger(L, b_->submit(o)), 1;
}

int StrategyRun::cancel(lua_State* L) {
    b_->cancel(static_cast<int>(luaL_checkinteger(L, 1)));
    return 0;
}

int StrategyRun::cancel_all(lua_State*) {
    b_->cancel_all();
    return 0;
}

int StrategyRun::modify(lua_State* L) {
    int id = static_cast<int>(luaL_checkinteger(L, 1));
    double price = luaL_checknumber(L, 2);
    for (Order& o : b_->orders)
        if (o.id == id && o.type != OrderType::Market && price > 0) {
            o.price = std::round(price / kTick) * kTick;
            return lua_pushboolean(L, 1), 1;
        }
    return lua_pushboolean(L, 0), 1;
}

int StrategyRun::orders(lua_State* L) {
    lua_createtable(L, static_cast<int>(b_->orders.size()), 0);
    int k = 1;
    for (const Order& o : b_->orders) {
        lua_createtable(L, 0, 6);
        lua_pushinteger(L, o.id), lua_setfield(L, -2, "id");
        lua_pushinteger(L, o.side), lua_setfield(L, -2, "side");
        lua_pushstring(L, type_name(o.type)), lua_setfield(L, -2, "type");
        lua_pushnumber(L, o.price), lua_setfield(L, -2, "price");
        lua_pushinteger(L, o.qty), lua_setfield(L, -2, "qty");
        lua_pushboolean(L, o.reduce_only), lua_setfield(L, -2, "protective");
        lua_rawseti(L, -2, k++);
    }
    return 1;
}

int StrategyRun::log(lua_State* L) {
    if (log_) {
        log_->push_back(clock(m_->t[i_] + 1) + "  " + luaL_checkstring(L, 1));
        if (log_->size() > 5000) log_->erase(log_->begin(), log_->begin() + 1000);
    }
    return 0;
}

// ---------------------------------------------------------------- backtest
void Backtest::start(const ScriptDef& def, const Params& p, const Market* m, int64_t from, int64_t to, const Broker& settings) {
    cancel();
    Broker b;
    b.mult = settings.mult, b.commission = settings.commission, b.slip_ticks = settings.slip_ticks;
    b.limit_touch = settings.limit_touch;
    {
        std::lock_guard<std::mutex> l(mu_);
        result_ = {};
    }
    progress_ = 0, cancel_ = false, running_ = true;
    ++runs_;
    ScriptDef copy = def;
    copy.ref = -1;
    th_ = std::thread(&Backtest::run, this, copy, p, m, from, to, b);
}

void Backtest::cancel() {
    cancel_ = true;
    if (th_.joinable()) th_.join();
    running_ = false;
}

BacktestResult Backtest::result() const {
    std::lock_guard<std::mutex> l(mu_);
    return result_;
}

void Backtest::run(ScriptDef def, Params p, const Market* m, int64_t from, int64_t to, Broker b) {
    BacktestResult r;
    r.strategy = def.name;
    std::string err;
    lua_State* L = new_script_state(err);
    set_script_sink(L, &r.log);
    auto finish = [&] {
        close_script_state(L);
        {
            std::lock_guard<std::mutex> l(mu_);
            result_ = std::move(r);
        }
        running_ = false;
        if (on_progress) on_progress();
    };
    if (!err.empty() || !load_script(L, def)) {
        r.error = err.empty() ? def.error : err;
        return finish();
    }
    r.params = script_label(L, def, p);
    size_t i0 = static_cast<size_t>(std::lower_bound(m->t.begin(), m->t.end(), from) - m->t.begin());
    size_t i1 = static_cast<size_t>(std::lower_bound(m->t.begin(), m->t.end(), to) - m->t.begin());
    if (i1 <= i0 + 1) {
        r.error = "no data in that date range";
        return finish();
    }
    Agg agg;
    if (def.timeframe > 1) agg.build(*m, def.timeframe);
    StrategyRun run;
    if (!run.start(L, def, p, &b, m, def.timeframe > 1 ? &agg : nullptr, i0, &r.log)) {
        r.error = run.error();
        return finish();
    }
    size_t i = i0 + 1;
    int gaps = 0;
    for (; i < i1 && !cancel_; ++i) {
        if (b.pos != 0 && m->t[i] - m->t[i - 1] > 30 && ++gaps <= 50)  // early closes, holidays, data holes
            r.log.push_back(clock(m->t[i - 1] + 1) + "  position held across a " + std::to_string(m->t[i] - m->t[i - 1] - 1) +
                            "-minute gap in the data (to " + clock(m->t[i]) + ")");
        float d = m->adj[i] - m->adj[i - 1];
        if (d != 0) b.roll(d, m->t[i]);
        b.on_bar(*m, i);
        run.after_minute(i);
        if (!run.running()) break;
        if ((i & 4095) == 0) {
            progress_ = static_cast<double>(i - i0) / static_cast<double>(i1 - i0);
            if (on_progress) on_progress();
        }
    }
    if (!run.error().empty()) r.error = run.error();
    if (cancel_) r.error = "stopped";
    run.stop();
    if (b.pos != 0 && i < m->size()) {  // close what's still open at the next minute's open
        b.cancel_all();
        Order o;
        o.side = b.pos > 0 ? -1 : 1, o.qty = std::abs(b.pos), o.born = i - 1;
        b.submit(o);
        float d = m->adj[i] - m->adj[i - 1];
        if (d != 0) b.roll(d, m->t[i]);
        b.on_bar(*m, i);
        r.log.push_back("open position closed at the end of the test");
    }
    r.from = m->t[i0], r.to = m->t[std::min(i, m->size() - 1)];
    r.trips = b.trips;
    double eq = 0;
    for (const RoundTrip& t : r.trips) r.equity.push_back(eq += t.pnl);
    r.stats = compute_stats(r.trips, *m, i0, std::min(i, i1));
    r.stats.fees = b.fees;
    progress_ = 1;
    finish();
}

BacktestStats compute_stats(const std::vector<RoundTrip>& trips, const Market& m, size_t i0, size_t i1) {
    BacktestStats s;
    double eq = 0, peak = 0, minutes = 0;
    int consec = 0;
    std::map<int64_t, double> daily;
    for (const RoundTrip& t : trips) {
        ++s.trades;
        s.net += t.pnl;
        if (t.pnl > 0) ++s.wins, s.gross_win += t.pnl, consec = 0;
        else s.gross_loss -= t.pnl, s.max_consec_losses = std::max(s.max_consec_losses, ++consec);
        s.largest_win = std::max(s.largest_win, t.pnl), s.largest_loss = std::min(s.largest_loss, t.pnl);
        eq += t.pnl, peak = std::max(peak, eq), s.max_dd = std::max(s.max_dd, peak - eq);
        minutes += static_cast<double>(t.t_out - t.t_in);
        daily[session_of(t.t_out)] += t.pnl;
    }
    s.avg_minutes = s.trades ? minutes / s.trades : 0;
    // every session in the range counts, with 0 on days without trades
    std::vector<double> days;
    int64_t last = INT64_MIN;
    for (size_t i = i0; i < i1 && i < m.size(); ++i) {
        int64_t d = session_of(m.t[i]);
        if (d == last) continue;
        last = d;
        auto it = daily.find(d);
        days.push_back(it == daily.end() ? 0.0 : it->second);
    }
    s.days = static_cast<int>(days.size());
    if (days.size() > 1) {
        double mean = 0, var = 0;
        for (double v : days) mean += v;
        mean /= days.size();
        for (double v : days) var += (v - mean) * (v - mean);
        double sd = std::sqrt(var / (days.size() - 1));
        s.sharpe = sd > 0 ? mean / sd * std::sqrt(252.0) : 0;
    }
    int up = 0;
    for (auto& [d, v] : daily) up += v > 0;
    s.pct_days_up = daily.empty() ? 0 : 100.0 * up / daily.size();
    return s;
}

bool export_trips_csv(const fs::path& path, const std::vector<RoundTrip>& trips) {
    std::FILE* f = open_file(path, "w");
    if (!f) return false;
    std::fprintf(f, "entry_time,exit_time,side,qty,entry,exit,pnl_usd\n");
    for (const RoundTrip& r : trips)
        std::fprintf(f, "%s,%s,%s,%d,%.2f,%.2f,%.2f\n", clock(r.t_in).c_str(), clock(r.t_out).c_str(),
                     r.side > 0 ? "long" : "short", r.qty, r.p_in, r.p_out, r.pnl);
    return std::fclose(f) == 0;
}
