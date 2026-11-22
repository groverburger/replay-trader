-- RSI(2) oversold: when the 2-session RSI of closes (today's 15:58 price as the latest) is below the threshold,
-- buy at the 15:59 open; sell at the 15:59 open of the first later session whose price is above the 5-session
-- average of closes. Long only. Uses roll-adjusted (continuous) closes.
--
-- Why it pays: short, sharp declines are often forced or mechanical selling (de-risking, margin calls,
-- leveraged-ETF rebalancing); buying it supplies liquidity over a few days and is paid by the bounce.
-- Evidence (after taker costs): NQ 2010-18 Sharpe 0.71, 2019-22 0.72; ES 2010-26 (held out) 0.81, t 3.4.
return strategy {
  name = "RSI(2) oversold",
  description = "Buys the close when RSI(2) < threshold; sells the first close above the 5-session average.",
  timeframe = 1,
  warmup_days = 30,
  params = {
    { "threshold", 10, min = 1, max = 50, label = "RSI(2) below" },
    { "qty", 1, min = 1, max = 50 },
    { "slip", 0.5, min = 0, max = 3, step = 0.25, label = "Slippage at close (pt)" },
  },
  init = function(ctx, p)
    ctx.vars = { closes = {}, g = nil, l = nil }
  end,
  on_bar = function(ctx, p, bar)
    local v = ctx.vars
    local hhmm = time.hhmm(bar.time)
    local px = bar.close - bar.adj                       -- continuous across contract rolls
    if hhmm == 930 and v.last_rth then                    -- yesterday's official close (also on early closes)
      local c = v.closes
      c[#c + 1] = v.last_rth
      local n = #c
      if n == 3 then
        local d1, d2 = c[2] - c[1], c[3] - c[2]
        v.g, v.l = (math.max(d1, 0) + math.max(d2, 0)) / 2, (math.max(-d1, 0) + math.max(-d2, 0)) / 2
      elseif n > 3 then
        local d = c[n] - c[n - 1]
        v.g, v.l = (v.g + math.max(d, 0)) / 2, (v.l + math.max(-d, 0)) / 2
      end
      if n > 10 then table.remove(c, 1) end
      v.last_rth = nil
    end
    if hhmm >= 930 and hhmm < 1600 then v.last_rth = px end
    if hhmm ~= 1558 or not v.g then return end
    local c, n = v.closes, #v.closes
    local d = px - c[n]
    local gg, ll = (v.g + math.max(d, 0)) / 2, (v.l + math.max(-d, 0)) / 2
    local rsi = ll == 0 and 100 or 100 - 100 / (1 + gg / ll)
    local oversold = rsi < p.threshold
    local pos = ctx:position()
    if pos == 0 then
      if oversold then
        ctx:buy(p.qty, { slippage = p.slip })
        v.entry_session = ctx:session()
      end
    elseif ctx:session() ~= v.entry_session and n >= 4 then
      local sma = (c[n] + c[n - 1] + c[n - 2] + c[n - 3] + px) / 5
      if px > sma then
        if oversold then v.entry_session = ctx:session()   -- exit and re-entry in the same minute: stay long
        else ctx:sell(pos, { slippage = p.slip }) end
      end
    end
  end,
}
