#include "broker.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "civil.h"

void Broker::reset() {
    pos = 0, avg = 0, pos_since = 0, realized = 0, fees = 0, open_fees_ = 0;
    orders.clear(), fills.clear(), trips.clear(), log.clear();
    next_id_ = 1, next_oco_ = 1;
}

int Broker::submit(Order o) {
    o.id = next_id_++;
    orders.push_back(o);
    return o.id;
}

void Broker::cancel(int id) {
    orders.erase(std::remove_if(orders.begin(), orders.end(), [&](const Order& o) { return o.id == id; }),
                 orders.end());
}

void Broker::cancel_all() { orders.clear(); }

void Broker::roll(double delta, int64_t time) {
    if (pos == 0 && orders.empty()) return;
    avg += pos ? delta : 0;
    for (Order& o : orders)
        if (o.type != OrderType::Market) o.price += delta;
    char buf[96];
    std::snprintf(buf, sizeof buf, "contract roll: prices shifted %+.2f (position and orders moved with it)", delta);
    log.push_back(buf);
    (void)time;
}

double Broker::fee_at(int64_t time) const {
    if (fee_schedule.empty()) return commission;
    int y, m, d;
    civil_from_days(floor_div(time, 1440), y, m, d);
    double fee = fee_schedule.front().second;
    for (const auto& [year, f] : fee_schedule)
        if (year <= y) fee = f;
    return fee;
}

std::string Broker::cost_summary() const {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%s, %d tick%s slippage, ", mult == 2.0 ? "MNQ" : "NQ", slip_ticks, slip_ticks == 1 ? "" : "s");
    std::string s = buf;
    if (fee_schedule.empty()) {
        std::snprintf(buf, sizeof buf, "$%.2f/side", commission);
        return s + buf;
    }
    s += "$/side by year: ";
    for (size_t k = 0; k < fee_schedule.size(); ++k) {
        std::snprintf(buf, sizeof buf, "%s%d %.2f", k ? ", " : "", fee_schedule[k].first, fee_schedule[k].second);
        s += buf;
    }
    return s;
}

bool parse_fee_schedule(const std::string& s, std::vector<std::pair<int, double>>& out, std::string& err) {
    out.clear();
    size_t i = 0;
    auto skip = [&] { while (i < s.size() && (s[i] == ',' || s[i] == ' ' || s[i] == ';')) ++i; };
    for (skip(); i < s.size(); skip()) {
        char* end;
        long y = std::strtol(s.c_str() + i, &end, 10);
        size_t j = end - s.c_str();
        if (j == i || j >= s.size() || (s[j] != '=' && s[j] != ':') || y < 1900 || y > 2200) {
            err = "fee schedule: want YEAR=$/side, e.g. 2010=2.01, 2020=2.10";
            return false;
        }
        double f = std::strtod(s.c_str() + j + 1, &end);
        if (end == s.c_str() + j + 1 || f < 0 || f > 100) {
            err = "fee schedule: bad fee for " + std::to_string(y);
            return false;
        }
        out.push_back({static_cast<int>(y), f});
        i = end - s.c_str();
    }
    if (out.empty()) {
        err = "fee schedule is empty";
        return false;
    }
    std::sort(out.begin(), out.end());
    return true;
}

void Broker::fill(const Order& o, double price, int64_t time, const char* why) {
    int qty = o.qty;
    if (o.reduce_only) qty = std::min(qty, pos * -o.side > 0 ? std::abs(pos) : 0);
    if (qty <= 0) return;
    fills.push_back({time, o.side, qty, price, why, o.reduce_only});
    const double rate = fee_at(time);
    double fee = rate * qty;
    fees += fee;
    int signed_qty = o.side * qty;
    if (pos == 0 || (pos > 0) == (signed_qty > 0)) {  // open or add
        if (pos == 0) pos_since = time, open_fees_ = 0;
        avg = (avg * std::abs(pos) + price * qty) / (std::abs(pos) + qty);
        pos += signed_qty;
        open_fees_ += fee;
    } else {  // reduce, close or flip
        int closing = std::min(qty, std::abs(pos));
        double pnl = (price - avg) * (pos > 0 ? 1 : -1) * closing * mult;
        realized += pnl;
        double share = open_fees_ * closing / std::abs(pos);  // entry fees of the closed part
        open_fees_ -= share;
        trips.push_back({pos_since, time, pos > 0 ? 1 : -1, closing, avg, price,
                         pnl - share - rate * closing, share + rate * closing});
        pos += o.side * closing;
        int rest = qty - closing;
        if (pos == 0) avg = 0;
        if (rest > 0) {  // flipped: the remainder opens a new position
            pos = o.side * rest, avg = price, pos_since = time, open_fees_ = rate * rest;
        }
    }
    if (o.managed && o.reduce_only)  // this protective order is done: don't count it when resizing the others
        for (Order& x : orders)
            if (x.id == o.id) x.qty = 0;
    if (o.managed) {
        bool adds = pos != 0 && (pos > 0) == (o.side > 0);  // this fill opened or added to the position
        if (adds && o.stop_pts > 0 && !managed_stop()) {
            Order s;
            s.side = -o.side, s.type = OrderType::Stop, s.qty = std::abs(pos), s.reduce_only = true, s.managed = true;
            s.price = price - o.side * o.stop_pts;
            s.born = static_cast<size_t>(-1);
            submit(s);
        }
        if (adds && (o.target_pts > 0 || o.target2_pts > 0)) {
            int q1 = o.target2_pts > 0 ? static_cast<int>(std::lround(qty * o.t1_frac)) : qty;
            if (o.target_pts <= 0) q1 = 0;
            q1 = std::clamp(q1, 0, qty);
            for (int leg = 0; leg < 2; ++leg) {
                int q = leg == 0 ? q1 : qty - q1;
                double d = leg == 0 ? o.target_pts : (o.target2_pts > 0 ? o.target2_pts : o.target_pts);
                if (q <= 0 || d <= 0) continue;
                Order t;
                t.side = -o.side, t.type = OrderType::Limit, t.qty = q, t.reduce_only = true, t.managed = true;
                t.price = price + o.side * d;
                t.born = static_cast<size_t>(-1);
                submit(t);
            }
        }
        sync_managed();
        return;
    }
    sync_managed();
    if (o.stop_pts > 0 || o.target_pts > 0) {  // attach the bracket to this fill
        int g = next_oco_++;
        if (o.stop_pts > 0) {
            Order s;
            s.side = -o.side, s.type = OrderType::Stop, s.qty = qty, s.oco = g, s.reduce_only = true;
            s.price = price - o.side * o.stop_pts;
            s.born = static_cast<size_t>(-1);  // marks "created this bar"
            submit(s);
        }
        if (o.target_pts > 0) {
            Order t;
            t.side = -o.side, t.type = OrderType::Limit, t.qty = qty, t.oco = g, t.reduce_only = true;
            t.price = price + o.side * o.target_pts;
            t.born = static_cast<size_t>(-1);
            submit(t);
        }
    }
}

Order* Broker::managed_stop() {
    for (Order& o : orders)
        if (o.managed && o.type == OrderType::Stop && pos != 0 && o.side == (pos > 0 ? -1 : 1)) return &o;
    return nullptr;
}

void Broker::sync_managed() {
    // Orders aren't erased here (on_bar may be walking the list): dropped ones get qty 0, and purge() or the
    // end of on_bar removes them.
    int want = pos > 0 ? -1 : 1, size = std::abs(pos);
    std::vector<Order*> targets;
    for (Order& o : orders) {
        if (!o.managed || !o.reduce_only) continue;
        if (pos == 0 || o.side != want) o.qty = 0;
        else if (o.type == OrderType::Stop) o.qty = size;
        else targets.push_back(&o);
    }
    int total = 0;
    for (Order* t : targets) total += t->qty;
    // trim the farthest targets first
    std::sort(targets.begin(), targets.end(), [&](Order* a, Order* b) { return (a->price - b->price) * pos > 0; });
    for (Order* t : targets) {
        if (total <= size) break;
        int cut = std::min(t->qty, total - size);
        t->qty -= cut, total -= cut;
    }
}

void Broker::purge() {
    orders.erase(std::remove_if(orders.begin(), orders.end(), [](const Order& o) { return o.managed && o.qty <= 0; }),
                 orders.end());
}

int Broker::fill_now(Order o, double price, int64_t time) {
    o.id = next_id_++;
    o.type = OrderType::Market;
    fill(o, price, time, "market");
    return o.id;
}

bool Broker::on_bar(const Market& m, size_t i) {
    const double o = m.o[i], h = m.h[i], l = m.l[i], slip = slip_ticks * kTick;
    const int64_t t = m.t[i];
    bool any = false;
    std::vector<int> done;  // ids filled or cancelled during this bar
    auto finish = [&](const Order& x) {
        done.push_back(x.id);
        for (const Order& y : orders)
            if ((x.oco && y.oco == x.oco && y.id != x.id) || (pos == 0 && y.reduce_only))  // OCO, or nothing to protect
                done.push_back(y.id);
    };
    auto pending = [&](const Order& x) { return std::find(done.begin(), done.end(), x.id) == done.end(); };

    // 1. market orders at the open
    for (size_t k = 0; k < orders.size(); ++k) {
        Order x = orders[k];
        if (x.type != OrderType::Market || !pending(x)) continue;
        fill(x, o + x.side * (x.slip_pts >= 0 ? x.slip_pts : slip), t, "market");
        finish(x), any = true;
    }
    // 2. stops (including stops created by fills above), then 3. limits not created this bar
    for (OrderType pass : {OrderType::Stop, OrderType::Limit}) {
        for (size_t k = 0; k < orders.size(); ++k) {
            Order x = orders[k];
            if (x.type != pass || !pending(x)) continue;
            if (x.managed && x.qty <= 0) continue;  // dropped by sync_managed
            bool fresh = x.born == static_cast<size_t>(-1);
            if (pass == OrderType::Limit && fresh) continue;
            bool hit;
            double px;
            if (pass == OrderType::Stop) {
                hit = x.side > 0 ? h >= x.price : l <= x.price;
                px = (x.side > 0 ? std::max(x.price, fresh ? x.price : o) : std::min(x.price, fresh ? x.price : o)) +
                     x.side * slip;
            } else {
                double through = limit_touch ? 0 : kTick;
                hit = x.side > 0 ? l <= x.price - through : h >= x.price + through;
                px = x.side > 0 ? std::min(x.price, o) : std::max(x.price, o);
            }
            if (!hit) continue;
            fill(x, px, t, pass == OrderType::Stop ? "stop" : "limit");
            finish(x), any = true;
        }
    }
    orders.erase(std::remove_if(orders.begin(), orders.end(), [&](const Order& x) { return !pending(x) || (x.managed && x.qty <= 0); }),
                 orders.end());
    for (Order& x : orders)
        if (x.born == static_cast<size_t>(-1)) x.born = i;  // brackets are ordinary orders from the next bar on
    return any;
}
