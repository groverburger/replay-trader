-- Double seven: above the 200-session average, buy a 7-session closing low at the 15:59 open; sell at a
-- 7-session closing high. Long only. Uses roll-adjusted (continuous) closes.
--
-- Why it pays: pullbacks within an uptrend are often liquidity-driven; buying them supplies liquidity and is
-- paid by the recovery.
-- Evidence (after taker costs): NQ 2010-18 Sharpe 0.79, 2019-22 0.41; ES 2010-26 (held out) 0.68, t 2.9.
return strategy {
  name = "Double seven",
  description = "Above the 200-session average: buy a 7-session closing low, sell a 7-session closing high.",
  timeframe = 1,
  warmup_days = 320,
  params = {
    { "n", 7, min = 3, max = 30, label = "Lookback (sessions)" },
    { "trend", 200, min = 0, max = 400, label = "Trend SMA (0 = off)" },
    { "qty", 1, min = 1, max = 50 },
    { "slip", 0.5, min = 0, max = 3, step = 0.25, label = "Slippage at close (pt)" },
  },
  init = function(ctx, p) ctx.vars = { closes = {} } end,
  on_bar = function(ctx, p, bar)
    local v = ctx.vars
    local hhmm = time.hhmm(bar.time)
    local px = bar.close - bar.adj
    if hhmm == 930 and v.last_rth then
      table.insert(v.closes, v.last_rth)
      if #v.closes > 420 then table.remove(v.closes, 1) end
      v.last_rth = nil
    end
    if hhmm >= 930 and hhmm < 1600 then v.last_rth = px end
    if hhmm ~= 1558 then return end
    local c, n = v.closes, #v.closes
    if n < math.max(p.n, p.trend) then return end
    local lo, hi = px, px                       -- the last n closes include today's
    for k = n - p.n + 2, n do lo, hi = math.min(lo, c[k]), math.max(hi, c[k]) end
    local pos = ctx:position()
    local enter = px <= lo
    if enter and p.trend > 0 then
      local s = px
      for k = n - p.trend + 2, n do s = s + c[k] end
      enter = px > s / p.trend
    end
    if pos == 0 and enter then ctx:buy(p.qty, { slippage = p.slip })
    elseif pos > 0 and px >= hi and not enter then ctx:sell(pos, { slippage = p.slip }) end
  end,
}
