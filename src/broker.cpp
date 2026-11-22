#include "broker.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

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

void Broker::fill(const Order& o, double price, int64_t time, const char* why) {
    int qty = o.qty;
    if (o.reduce_only) qty = std::min(qty, pos * -o.side > 0 ? std::abs(pos) : 0);
    if (qty <= 0) return;
    fills.push_back({time, o.side, qty, price, why, o.reduce_only});
    double fee = commission * qty;
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
                         pnl - share - commission * closing});
        pos += o.side * closing;
        int rest = qty - closing;
        if (pos == 0) avg = 0;
        if (rest > 0) {  // flipped: the remainder opens a new position
            pos = o.side * rest, avg = price, pos_since = time, open_fees_ = commission * rest;
        }
    }
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
        fill(x, o + x.side * slip, t, "market");
        finish(x), any = true;
    }
    // 2. stops (including stops created by fills above), then 3. limits not created this bar
    for (OrderType pass : {OrderType::Stop, OrderType::Limit}) {
        for (size_t k = 0; k < orders.size(); ++k) {
            Order x = orders[k];
            if (x.type != pass || !pending(x)) continue;
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
    orders.erase(std::remove_if(orders.begin(), orders.end(), [&](const Order& x) { return !pending(x); }),
                 orders.end());
    for (Order& x : orders)
        if (x.born == static_cast<size_t>(-1)) x.born = i;  // brackets are ordinary orders from the next bar on
    return any;
}
