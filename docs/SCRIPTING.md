# Scripting Replay Trader with Lua

Every indicator and strategy in Replay Trader is a Lua script, including the built-in ones. You can add your
own without rebuilding anything:

- Put indicators in `scripts/indicators/` and strategies in `scripts/strategies/`, inside the app's data
  folder. The **Scripts folder** button in the app opens it.
- Saved changes are picked up within a second. You can also press **Reload scripts**.
- A file with the same name as a built-in script replaces it.
- `scripts/examples/` holds copies of every built-in script to start from. The app rewrites them on each
  launch and never loads them, so copy the ones you want into `indicators/` or `strategies/` before editing.

Scripts use Lua 5.4 with the `string`, `table`, `math`, `utf8` and `coroutine` libraries, but no file or OS
access. `print(...)` writes to the log. A script that runs for too long (an endless loop, say) is stopped with
an error instead of freezing the app.

**Times** are New York minutes since 1970-01-01, the same clock as the chart. These helpers convert them:

| Helper | Returns |
|---|---|
| `time.hhmm(t)` | 935 for 09:35 |
| `time.tod(t)` | Minutes since midnight |
| `time.date(t)` | `"2025-04-09"` |
| `time.ymd(t)` | Year, month, day |
| `time.weekday(t)` | 0 = Sunday |
| `time.session(t)` | Globex session id (the 18:00 open starts the next one) |
| `time.is_rth(t)` | True from 09:30 to 16:00 |

## Indicators

```lua
-- scripts/indicators/my_sma.lua
return indicator {
  name = "My SMA",
  overlay = true,                      -- on the price chart (false: its own pane below)
  params = {
    { "period", 20, min = 2, max = 500 },                   -- integer: a number field
    { "mult", 1.5, min = 0.1, max = 5, step = 0.1 },        -- decimal: step sets the precision
    { "source", "close", options = { "close", "hl2" } },    -- string: a dropdown
    { "show_band", true, label = "Show band" },             -- boolean: a checkbox
  },
  plots = {
    { "avg", width = 1.6 },              -- no colour: uses the indicator's swatch colour
    { "upper", color = "#88aaff", alpha = 0.6 },
    { "lower", color = "#88aaff", alpha = 0.6 },
  },
  fills = { { "upper", "lower", alpha = 0.08 } },  -- shade between two plots
  calc = function(bars, p, out)
    local avg, src = ta.sma(p.period), source(bars, p.source)
    for i = 1, bars.n do
      local v = avg:update(src(i))
      out.avg[i] = v
      if v and p.show_band then out.upper[i], out.lower[i] = v * (1 + p.mult / 100), v * (1 - p.mult / 100) end
    end
  end,
}
```

**`calc(bars, p, out)`** runs whenever the bars on the chart change. You'll rarely need to optimize it: it
covers the bars in view plus a warm-up.

`bars` holds the chart's bars on the current timeframe, oldest first, as arrays indexed `1..bars.n`:

| Field | Meaning |
|---|---|
| `open`, `high`, `low`, `close`, `volume` | Prices and volume per bar |
| `time` | Bar start time |
| `last_complete` | Index of the last closed bar. `bars.partial` is true while the last bar is still forming |
| `tf` | Timeframe in minutes (1440 = daily) |

The last bar is still forming during replay. Anything that needs bars to its right, such as pivots, should stop
at `last_complete`, so it never draws something the future decides.

`p` holds the params by name: numbers, booleans, and the chosen option's string.

`out.<plot>[i] = value` sets a plot's value. Leave it `nil` where there is no value. `out` can also draw
shapes:

- `out:marker(i, price, "up" | "down", plot)` draws a triangle below the price (pointing up) or above it
  (pointing down), in the plot's colour.
- `out:segment(i0, i1, price, plot, alpha)` draws a horizontal line from bar `i0` to bar `i1`.

**Definition options:**

- **Plot styles:**
  - `"line"` (default).
  - `"step"`: each bar's value drawn flat across its own width. Use it for levels.
  - `"histogram"`: bars from zero. Add `color_by = "bar"` to colour each bar by its candle.
  - `"markers"`: draws nothing itself, and exists to give markers and segments a colour.
- **Pane options:** `range = { 0, 100 }` fixes the pane's scale, `levels = { 30, 70 }` draws reference lines,
  and `zero_based = true` starts the scale at 0.
- **`label = function(p) return "My SMA " .. p.period end`** sets the legend text. The default is the name
  followed by the numeric params.
- **`warmup = function(p) return 4 * p.period end`** sets how many bars before the view to compute from. The
  default is 4 × the largest integer param + 10.

## Strategies

A strategy trades the paper account automatically. It runs in two modes:

- **Auto-trade:** tick "Auto-trade the replay" in the Strategy panel, and it trades alongside you as the
  replay plays.
- **Backtest:** press "Run backtest" to run it over a date range in the background. You get statistics, an
  equity curve and a CSV of the trades.

Either way the strategy sees only closed bars on its own timeframe, and its orders fill from the next minute
on. The fill rules are the ones manual orders use: no lookahead, with slippage and commission from the order
ticket.

```lua
-- scripts/strategies/my_breakout.lua
return strategy {
  name = "Prior-bar breakout",
  description = "Buys a break of the prior 5-minute bar's high in an uptrend.",
  timeframe = 5,                        -- minutes per bar it sees (1 = every minute)
  warmup_days = 0,                      -- history it sees before it may trade (orders are refused until then)
  params = {
    { "qty", 1, min = 1, max = 10 },
    { "stop", 15.0, min = 1, max = 100, step = 0.25, label = "Stop (pt)" },
  },
  init = function(ctx, p)               -- start of a run (and of each new replay session)
    ctx.vars = { trend = ta.ema(50) }
  end,
  on_day = function(ctx, p)             -- each new Globex session (and right after init)
    ctx.vars.traded = false
  end,
  on_bar = function(ctx, p, bar)        -- each closed bar
    local ema = ctx.vars.trend:update(bar.close)
    if not ema or ctx.vars.traded or not ctx:rth() then return end
    if ctx:position() == 0 and bar.close > ema then
      ctx:buy(p.qty, { stop = ctx:high(0) + ctx.tick, stop_loss = p.stop, take_profit = 2 * p.stop })
      ctx.vars.traded = true
    end
    if ctx:hhmm() >= 1555 then ctx:flatten() end
  end,
  on_fill = function(ctx, p, fill)      -- each fill: side, qty, price, time, why ("market"/"limit"/"stop"),
    ctx:log("filled", fill.side, fill.price, fill.why)  -- protective (true for a stop loss / take profit)
  end,
}
```

### The `ctx` object

**Bars on the strategy's timeframe.** `k = 0` is the bar that just closed, `1` the one before.

- `ctx:open(k)`, `ctx:high(k)`, `ctx:low(k)`, `ctx:close(k)`, `ctx:volume(k)`, `ctx:time(k)` return values,
  or `nil` beyond the available history.
- `ctx:highest(n, k)` and `ctx:lowest(n, k)` return the extreme over `n` bars starting `k` bars back.
- `ctx:bar_count()` returns the number of bars available.

**Clock.**

- `ctx:now()` is the time the last minute closed. `ctx:hhmm()`, `ctx:date()`, `ctx:rth()` and `ctx:session()`
  are derived from it.
- `ctx:bar_end()` is the end time of the strategy bar that just closed.
- `ctx:market_open_next()` is false when trading stops right after that bar: the daily break, weekends,
  holidays and early closes.
- `ctx:trading()` is false during `warmup_days`.
- `ctx:adj()` (and `bar.adj`) is the contract-roll adjustment at the minute that just closed. Prices jump at
  each roll; `price - ctx:adj()` is continuous. Use it for anything that compares prices across days, such as
  multi-day returns, RSI or volatility.

**Account.**

- `ctx:position()` returns signed contracts.
- `ctx:avg_price()`, `ctx:last()` and `ctx:points_open()` return the entry price, the last price, and the
  open profit in points.
- `ctx:open_pnl()` and `ctx:closed_pnl()` return dollars, after commission. `ctx.point_value` is $ per point
  and `ctx.tick` is 0.25.

**Orders.** Each order call returns an order id.

- `ctx:buy(qty, opts)` and `ctx:sell(qty, opts)` place an order. They return `nil` during the warm-up.
  - With no `opts`, it's a market order.
  - `opts.limit = price` makes it a limit order, and `opts.stop = price` a stop order.
  - `opts.stop_loss = points` and `opts.take_profit = points` attach a bracket when the entry fills. Its two
    legs cancel each other.
  - `opts.slippage = points` sets a market order's slippage, instead of the ticket's.
  - `opts.fill = "close"` fills a market order at the close of the bar that just closed, plus slippage,
    instead of at the next minute's open. This is the QuantConnect/LEAN backtest convention. Use it to
    reproduce results from there; the default is the more conservative choice.
- `ctx:flatten()` cancels all orders and closes the position at market.
- `ctx:cancel(id)`, `ctx:cancel_all()` and `ctx:modify(id, new_price)` manage working orders. Use `modify` to
  trail stops and move targets.
- `ctx:orders()` lists working orders: `{ id, side, type, price, qty, protective }`.
- `ctx:stop_order()` and `ctx:target_order()` return the current position's bracket legs.
- `ctx:round_tick(price)` rounds a price to the tick.

**State and logging.** Keep state in `ctx.vars`, or in locals: a script's top-level locals persist for the
run. `ctx:log(...)` writes to the log, with the replay time.

### Streaming indicators: `ta`

Create these once, in `init`, then call `:update(...)` once per bar. Each returns the new value (also stored in
`.value`), or `nil` until it has enough bars.

| Constructor | Update with | Notes |
|---|---|---|
| `ta.sma(n)` | `x` | Simple moving average |
| `ta.ema(n)` | `x` | Seeded with the SMA of its first `n` values |
| `ta.rma(n)` | `x` | Wilder's moving average |
| `ta.stdev(n)` | `x` | Population standard deviation |
| `ta.rsi(n)` | `close` | Wilder's RSI |
| `ta.atr(n)` | `high, low, close` | Wilder's ATR |
| `ta.macd(fast, slow, signal)` | `close` | `.value` is the line; also `.signal` and `.hist` |
| `ta.highest(n)`, `ta.lowest(n)` | `x` | Extreme over the last `n` values |
| `ta.vwap()` | `price, volume, time.session(t)` | Resets each session; `.sd` holds the standard deviation |

`ta.crossover(a_prev, a, b_prev, b)` and `ta.crossunder(...)` are true on the bar where `a` crosses `b`.
`source(bars, name)` (indicators only) returns a function `i -> price` for "close", "open", "high", "low",
"hl2", "hlc3" or "ohlc4".

### Built-in strategies

Four strategies with tested results. Each buys after a short-term decline and holds for one to a few sessions.
All passed a 2010-2018 training period, a 2019-2022 test period and a held-out test on ES (S&P 500 futures),
after taker costs:

| Strategy | Rule | ES 2010-2026 Sharpe |
|---|---|---|
| Selloff absorption | session down at least 1 ATR20 from its open: buy the close, sell the next close | 0.68 |
| RSI(2) oversold | RSI(2) below 10: buy the close, sell the first close above the 5-session average | 0.81 |
| IBS reversion | close in the bottom 20% of the day's range: buy, sell the next close | 0.58 |
| Double seven | above the 200-session average, a 7-session closing low: buy; sell a 7-session closing high | 0.68 |

They are long only and partly overlap. Three API examples show other features and have no tested edge:
opening range breakout (stop entries, brackets, breakeven stop), EMA trend (5-minute bars, trailing stop via
`modify`) and VWAP reversion (moving target, time stop, daily loss limit).

Backtest results are hypothetical. They use 1-minute bars with conservative fill rules, so they still can't
model queue position, partial fills or fast markets. Treat a good backtest as a reason to look closer, not as
proof.
