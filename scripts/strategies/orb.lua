-- Opening range breakout.
--
-- After the first `range_minutes` of the regular session, place a buy stop a tick above the range and a sell
-- stop a tick below it (one cancels the other). The stop loss sits at the other side of the range, capped at
-- `max_stop` points; the target is `target_r` times the risk. Once the trade is `breakeven_r` in profit, the
-- stop moves to the entry. No new entries after `last_entry`; everything is flat by 15:55.
return strategy {
  name = "Opening range breakout",
  description = "Stop entries on a break of the opening range; range stop, R target, breakeven, flat by 15:55.",
  timeframe = 1,
  params = {
    { "range_minutes", 15, min = 5, max = 120, label = "Range (min)" },
    { "qty", 1, min = 1, max = 20 },
    { "max_stop", 40.0, min = 5, max = 200, step = 0.25, label = "Max stop (pt)" },
    { "min_range", 10.0, min = 0, max = 200, step = 0.25, label = "Min range (pt)" },
    { "target_r", 2.0, min = 0.5, max = 10, step = 0.25, label = "Target (R)" },
    { "breakeven_r", 1.0, min = 0, max = 5, step = 0.25, label = "Breakeven at (R, 0 = off)" },
    { "last_entry", 1130, min = 940, max = 1550, label = "Last entry (hhmm)" },
    { "vwap_filter", false, label = "Only trade with VWAP" },
  },
  init = function(ctx, p)
    ctx.vars = {}
  end,
  on_day = function(ctx, p)
    ctx.vars = { vwap = ta.vwap() }
  end,
  on_bar = function(ctx, p, bar)
    local v = ctx.vars
    if not v.vwap then v.vwap = ta.vwap() end
    v.vwap:update((bar.high + bar.low + bar.close) / 3, bar.volume, time.session(bar.time))
    local hhmm, tod = ctx:hhmm(), time.tod(ctx:now())
    local range_end = 570 + p.range_minutes

    -- build the range
    if tod > 570 and tod <= range_end then
      v.hi = math.max(v.hi or bar.high, bar.high)
      v.lo = math.min(v.lo or bar.low, bar.low)
    end

    -- range complete: arm the breakout orders
    if tod >= range_end and tod < 960 and v.hi and not v.armed then
      v.armed = true
      local width = v.hi - v.lo
      if width < p.min_range then ctx:log("range", width, "pt below the minimum: no trade"); return end
      local risk = math.min(width + 0.5, p.max_stop)
      v.risk = risk
      local above = not p.vwap_filter or bar.close > v.vwap.value
      local below = not p.vwap_filter or bar.close < v.vwap.value
      if above then
        v.long_id = ctx:buy(p.qty, { stop = v.hi + ctx.tick, stop_loss = risk, take_profit = risk * p.target_r })
      end
      if below then
        v.short_id = ctx:sell(p.qty, { stop = v.lo - ctx.tick, stop_loss = risk, take_profit = risk * p.target_r })
      end
      ctx:log(string.format("range %.2f-%.2f (%.2f pt), risk %.2f pt", v.lo, v.hi, width, risk))
    end

    -- manage the open trade: breakeven stop
    local pos = ctx:position()
    if pos ~= 0 and p.breakeven_r > 0 and v.risk and not v.at_breakeven then
      if ctx:points_open() >= p.breakeven_r * v.risk then
        local stop = ctx:stop_order()
        if stop then
          ctx:modify(stop.id, ctx:avg_price())
          v.at_breakeven = true
          ctx:log("stop to breakeven")
        end
      end
    end

    -- no new entries late in the day; flat into the close
    if pos == 0 and hhmm >= p.last_entry and (v.long_id or v.short_id) then
      if v.long_id then ctx:cancel(v.long_id) end
      if v.short_id then ctx:cancel(v.short_id) end
      v.long_id, v.short_id = nil, nil
    end
    if hhmm >= 1555 and hhmm < 1600 and (pos ~= 0 or #ctx:orders() > 0) then ctx:flatten() end
  end,
  on_fill = function(ctx, p, fill)
    local v = ctx.vars
    -- an entry filled: cancel the other side
    if fill.why == "stop" and not fill.protective then
      if fill.side > 0 and v.short_id then ctx:cancel(v.short_id) end
      if fill.side < 0 and v.long_id then ctx:cancel(v.long_id) end
      v.long_id, v.short_id = nil, nil
    end
  end,
}
