-- EMA trend following on 5-minute bars.
--
-- Go long when the fast EMA crosses above the slow EMA while price is above the trend EMA (short mirrored),
-- during regular hours. The protective stop starts `stop_atr` ATRs away and trails behind the best close
-- since entry (a chandelier stop); an opposite cross also exits. At most `max_trades` per day; flat by 15:55.
return strategy {
  name = "EMA trend (5m)",
  description = "Fast/slow EMA cross with a trend filter, ATR chandelier trailing stop, flat by 15:55.",
  timeframe = 5,
  params = {
    { "fast", 9, min = 2, max = 100 },
    { "slow", 21, min = 3, max = 200 },
    { "trend", 100, min = 10, max = 500, label = "Trend EMA" },
    { "atr", 14, min = 2, max = 100, label = "ATR period" },
    { "stop_atr", 2.5, min = 0.5, max = 10, step = 0.25, label = "Trail (ATR)" },
    { "qty", 1, min = 1, max = 20 },
    { "max_trades", 3, min = 1, max = 20, label = "Max trades/day" },
    { "start", 945, min = 930, max = 1500, label = "Start (hhmm)" },
    { "stop", 1530, min = 1000, max = 1555, label = "Last entry (hhmm)" },
  },
  init = function(ctx, p)
    -- indicators run across days, so they are created once per run, not per day
    ctx.vars = { fast = ta.ema(p.fast), slow = ta.ema(p.slow), trend = ta.ema(p.trend), atr = ta.atr(p.atr), trades = 0 }
  end,
  on_day = function(ctx, p)
    ctx.vars.trades = 0
  end,
  on_bar = function(ctx, p, bar)
    local v = ctx.vars
    local pf, ps = v.fast.value, v.slow.value
    local f, s, tr = v.fast:update(bar.close), v.slow:update(bar.close), v.trend:update(bar.close)
    local atr = v.atr:update(bar.high, bar.low, bar.close)
    if not (f and s and tr and atr and pf and ps) then return end
    local hhmm, pos = ctx:hhmm(), ctx:position()

    if hhmm >= 1555 and hhmm < 1600 then
      if pos ~= 0 or #ctx:orders() > 0 then ctx:flatten() end
      return
    end

    -- trail the stop behind the best close since entry
    if pos ~= 0 then
      v.best = pos > 0 and math.max(v.best or bar.close, bar.close) or math.min(v.best or bar.close, bar.close)
      local stop = ctx:stop_order()
      if stop then
        local want = ctx:round_tick(v.best - (pos > 0 and 1 or -1) * p.stop_atr * atr)
        if (pos > 0 and want > stop.price) or (pos < 0 and want < stop.price) then ctx:modify(stop.id, want) end
      end
      -- opposite cross: exit
      if (pos > 0 and ta.crossunder(pf, f, ps, s)) or (pos < 0 and ta.crossover(pf, f, ps, s)) then
        ctx:flatten()
        ctx:log("exit on opposite cross")
      end
      return
    end

    if hhmm < p.start or hhmm > p.stop or v.trades >= p.max_trades or #ctx:orders() > 0 then return end
    local risk = ctx:round_tick(p.stop_atr * atr)
    if ta.crossover(pf, f, ps, s) and bar.close > tr then
      ctx:buy(p.qty, { stop_loss = risk })
      v.trades, v.best = v.trades + 1, nil
    elseif ta.crossunder(pf, f, ps, s) and bar.close < tr then
      ctx:sell(p.qty, { stop_loss = risk })
      v.trades, v.best = v.trades + 1, nil
    end
  end,
}
