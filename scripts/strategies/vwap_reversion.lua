-- VWAP mean reversion.
--
-- When a bar closes beyond the session VWAP's `band` standard deviations with RSI at an extreme, fade it:
-- the target is VWAP itself (a limit order moved to the current VWAP every bar), the stop is `stop_atr` ATRs
-- beyond the entry, and a trade that hasn't worked after `max_bars` bars is closed. Trading stops for the day
-- after a closed loss of `daily_loss` dollars. Entries 10:00-15:00 only; flat by 15:55.
return strategy {
  name = "VWAP reversion",
  description = "API example (no tested edge): fades VWAP-band extremes; target follows VWAP; daily loss limit.",
  timeframe = 1,
  params = {
    { "band", 2.0, min = 0.5, max = 4, step = 0.25, label = "Band (sd)" },
    { "rsi", 14, min = 2, max = 50, label = "RSI period" },
    { "rsi_extreme", 25, min = 5, max = 45, label = "RSI extreme (from 0/100)" },
    { "stop_atr", 3.0, min = 0.5, max = 10, step = 0.25, label = "Stop (ATR)" },
    { "max_bars", 45, min = 5, max = 300, label = "Time stop (bars)" },
    { "qty", 1, min = 1, max = 20 },
    { "daily_loss", 1500, min = 100, max = 20000, label = "Daily loss limit ($)" },
  },
  init = function(ctx, p)
    ctx.vars = { vwap = ta.vwap(), rsi = ta.rsi(p.rsi), atr = ta.atr(14) }
  end,
  on_day = function(ctx, p)
    local v = ctx.vars
    v.day_start_pnl, v.done_for_day = ctx:closed_pnl(), false
  end,
  on_bar = function(ctx, p, bar)
    local v = ctx.vars
    local vw = v.vwap:update((bar.high + bar.low + bar.close) / 3, bar.volume, time.session(bar.time))
    local r = v.rsi:update(bar.close)
    local atr = v.atr:update(bar.high, bar.low, bar.close)
    local hhmm, pos = ctx:hhmm(), ctx:position()
    if not (r and atr) then return end

    if hhmm >= 1555 and hhmm < 1600 then
      if pos ~= 0 or #ctx:orders() > 0 then ctx:flatten() end
      return
    end

    if pos ~= 0 then
      v.bars_in = (v.bars_in or 0) + 1
      local target = ctx:target_order()
      if target then ctx:modify(target.id, ctx:round_tick(vw)) end  -- chase VWAP
      if v.bars_in >= p.max_bars then
        ctx:flatten()
        ctx:log("time stop")
      end
      return
    end

    -- daily loss limit
    if v.day_start_pnl and ctx:closed_pnl() - v.day_start_pnl <= -p.daily_loss and not v.done_for_day then
      v.done_for_day = true
      ctx:log("daily loss limit reached")
    end
    if v.done_for_day or hhmm < 1000 or hhmm >= 1500 or #ctx:orders() > 0 then return end

    local sd = v.vwap.sd
    if sd <= 0 then return end
    local stop = ctx:round_tick(p.stop_atr * atr)
    if bar.close > vw + p.band * sd and r >= 100 - p.rsi_extreme then
      ctx:sell(p.qty, { stop_loss = stop, take_profit = math.max(ctx.tick, bar.close - vw) })
      v.bars_in = 0
    elseif bar.close < vw - p.band * sd and r <= p.rsi_extreme then
      ctx:buy(p.qty, { stop_loss = stop, take_profit = math.max(ctx.tick, vw - bar.close) })
      v.bars_in = 0
    end
  end,
}
