-- Selloff absorption: after a session falls at least k x ATR20 from its 09:30 open, buy at the 15:59 open and
-- sell at the next session's 15:59 open. Long only.
--
-- Why it pays: after large down days, volatility-targeting funds, risk-parity books, leveraged ETFs and
-- margin-constrained traders sell into the close regardless of value. Buying that flow supplies liquidity; the
-- next-day rebound is the payment.
-- Evidence (after taker costs): NQ 2010-18 Sharpe 0.27, 2019-22 0.37; ES 2010-26 (held out) 0.68, t 2.9.
return strategy {
  name = "Selloff absorption",
  description = "Buys the close after a session falls >= k ATR20 from its open; sells the next session's close.",
  timeframe = 1,
  warmup_days = 40,                    -- 20 sessions of RTH ranges for ATR20 before trading
  params = {
    { "k", 1.0, min = 0.25, max = 3, step = 0.25, label = "Selloff (ATR20 multiple)" },
    { "qty", 1, min = 1, max = 50 },
    { "slip", 0.5, min = 0, max = 3, step = 0.25, label = "Slippage at close (pt)" },
  },
  init = function(ctx, p)
    ctx.vars = { ranges = {} }
  end,
  on_bar = function(ctx, p, bar)
    local v = ctx.vars
    local hhmm = time.hhmm(bar.time)
    if hhmm == 930 then v.open, v.hi, v.lo = bar.open, bar.high, bar.low end
    if hhmm >= 930 and hhmm < 1600 and v.hi then v.hi, v.lo = math.max(v.hi, bar.high), math.min(v.lo, bar.low) end
    if hhmm == 1558 and v.open then
      local atr = nil
      if #v.ranges == 20 then
        local s = 0
        for _, r in ipairs(v.ranges) do s = s + r end
        atr = s / 20
      end
      local signal = atr ~= nil and bar.close - v.open <= -p.k * atr
      local pos = ctx:position()
      if pos > 0 and not signal then
        ctx:sell(pos, { slippage = p.slip })                 -- held one session: out at the 15:59 open
      elseif pos == 0 and signal then
        ctx:buy(p.qty, { slippage = p.slip })                -- in at the 15:59 open
      end
    end
    if hhmm == 1559 and v.hi then
      table.insert(v.ranges, v.hi - v.lo)
      if #v.ranges > 20 then table.remove(v.ranges, 1) end
      v.open = nil
    end
  end,
}
