-- Overnight drift: long from the Globex open (18:00) to 03:00 New York time. Long only, one trade per night.
--
-- Why it pays: NQ's positive drift is earned mostly outside regular hours, and within the night it is
-- concentrated in the evening and around the European pre-open, not 03:00-09:30. Holding index risk while
-- U.S. cash markets are shut is a risk premium, so it is larger when volatility is higher; on quiet nights
-- it is too small to cover costs, which is what the optional volatility filter skips.
-- Evidence (NQ, 1 contract, $2.15/side + 0.25 pt slippage each way; vol filter 0.8): Sharpe 0.68 in 2010-18
-- (training), 0.81 in 2019-22 (test), 0.23 in 2023-26 (held out, run once). Weak in 2010-12, 2016, 2022-23.
-- Costs matter: at 0.5 pt slippage the 2010-18 Sharpe falls to 0.23. The volatility ratio is the latest
-- ATR20 of RTH ranges over its median of the last 250 sessions.
return strategy {
  name = "Overnight drift",
  description = "Long from the 18:00 Globex open to 03:00; optionally only when RTH volatility isn't unusually low.",
  timeframe = 1,
  warmup_days = 400,
  params = {
    { "entry", 1800, min = 1800, max = 2330, label = "Entry (hhmm, evening)" },
    { "exit", 300, min = 0, max = 900, label = "Exit (hhmm, morning)" },
    { "min_vol", 0.8, min = 0, max = 1.5, step = 0.05, label = "Min vol ratio (0 = off)" },
    { "qty", 1, min = 1, max = 50 },
    { "slip", 0.25, min = 0, max = 3, step = 0.25, label = "Slippage (pt)" },
  },
  init = function(ctx, p) ctx.vars = { ranges = {}, atrs = {} } end,
  on_bar = function(ctx, p, bar)
    local v = ctx.vars
    local hhmm = time.hhmm(bar.time)
    -- RTH range of each session -> ATR20, and a year of ATR20 values for the volatility ratio
    if hhmm == 930 then v.hi, v.lo = bar.high, bar.low end
    if hhmm > 930 and hhmm < 1600 and v.hi then v.hi, v.lo = math.max(v.hi, bar.high), math.min(v.lo, bar.low) end
    if hhmm == 1559 and v.hi then
      table.insert(v.ranges, v.hi - v.lo)
      if #v.ranges > 20 then table.remove(v.ranges, 1) end
      if #v.ranges == 20 then
        local s = 0
        for _, r in ipairs(v.ranges) do s = s + r end
        table.insert(v.atrs, s / 20)
        if #v.atrs > 250 then table.remove(v.atrs, 1) end
      end
      v.hi = nil
    end

    local pos = ctx:position()
    local evening = hhmm >= p.entry
    local session = time.session(bar.time)
    if pos == 0 and evening and v.entered ~= session then
      v.entered = session                        -- decide once per night, at the first bar after `entry`
      if p.min_vol > 0 then
        local n = #v.atrs
        if n < 120 then return end
        local sorted = {}
        for i, a in ipairs(v.atrs) do sorted[i] = a end
        table.sort(sorted)
        if v.atrs[n] < p.min_vol * sorted[math.floor(n / 2) + 1] then return end
      end
      ctx:buy(p.qty, { slippage = p.slip })
    elseif pos > 0 and not evening and hhmm >= p.exit - 1 and hhmm < 1600 then
      ctx:sell(pos, { slippage = p.slip })      -- fills at the next bar's open
    end
  end,
}
