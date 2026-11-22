// Paper broker: fills orders against each newly revealed 1-minute bar.
//
// Fill rules (conservative, since only 1-minute OHLC is known):
//  - market: next bar's open, plus/minus slippage ticks;
//  - stop: when the bar trades at/through the stop, at the worse of stop and open, plus/minus slippage;
//  - limit: when the bar trades through the limit by a tick (or touches it, if "fill on touch"), at the better
//    of limit and open, no slippage;
//  - within a bar, stops are checked before limits, so a bracket whose stop and target are both hit counts as
//    stopped out; a bracket created by a fill in this bar can stop out in the same bar but not take profit.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "market.h"

enum class OrderType { Market, Limit, Stop };

struct Order {
    int id = 0;
    int side = 1;              // +1 buy, -1 sell
    OrderType type = OrderType::Market;
    double price = 0;          // limit/stop price
    int qty = 1;
    int oco = 0;               // orders sharing a non-zero group cancel each other on fill
    bool reduce_only = false;  // bracket legs: never open or flip a position
    double stop_pts = 0, target_pts = 0;  // bracket to attach when this order fills (0 = none)
    size_t born = 0;           // index of the bar that was last revealed when it was placed
    double slip_pts = -1;      // market orders: slippage in points instead of the broker's (strategies)
};

struct Fill {
    int64_t time;
    int side, qty;
    double price;
    std::string why;
    bool protective = false;  // a bracket leg (stop loss / take profit) closed or reduced the position
};

struct RoundTrip {
    int64_t t_in, t_out;
    int side, qty;
    double p_in, p_out, pnl;   // pnl in $ after commission
};

struct Broker {
    double mult = 20.0;        // $ per point (NQ 20, MNQ 2)
    double commission = 2.15; // $ per contract per side
    int slip_ticks = 1;
    bool limit_touch = false;

    int pos = 0;
    double avg = 0;
    int64_t pos_since = 0;
    double realized = 0, fees = 0;  // realized excludes fees
    std::vector<Order> orders;
    std::vector<Fill> fills;
    std::vector<RoundTrip> trips;
    std::vector<std::string> log;

    void reset();
    int submit(Order o);           // returns order id
    void cancel(int id);
    void cancel_all();
    // Process bar i (just revealed). Returns true if anything filled.
    bool on_bar(const Market& m, size_t i);
    // Contract roll: raw prices jump by `delta`; shift the position and working orders to match.
    void roll(double delta, int64_t time);
    double unrealized(double last) const { return pos ? (last - avg) * pos * mult : 0; }
    // Fills a market order right away at `price` (strategies filling at their signal bar's close, as LEAN
    // backtests do). Returns the order id.
    int fill_now(Order o, double price, int64_t time);

  private:
    int next_id_ = 1, next_oco_ = 1;
    double open_fees_ = 0;      // commission paid on the currently open position's entries
    void fill(const Order& o, double price, int64_t time, const char* why);
};
