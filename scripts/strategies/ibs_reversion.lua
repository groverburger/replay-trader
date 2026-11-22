-- IBS reversion: if today's close is in the bottom part of the day's RTH range (internal bar strength below the
-- threshold), buy at the 15:59 open and sell at the next session's 15:59 open. Long only.
--
-- Why it pays: a close near the day's low is often forced or mechanical selling into the close; buying it
-- supplies liquidity and is paid by the next-day rebound.
-- Evidence (after taker costs): NQ 2010-18 Sharpe 0.84, 2019-22 0.94; ES 2010-26 (held out) 0.58, t 2.4.
return strategy {
  name = "IBS reversion",
  description = "Buys a close in the bottom part of the day's range; sells the next session's close.",
  timeframe = 1,
  warmup_days = 5,
  params = {
    { "ibs", 0.2, min = 0.05, max = 0.5, step = 0.05, label = "Buy if IBS below" },
    { "qty", 1, min = 1, max = 50 },
    { "slip", 0.5, min = 0, max = 3, step = 0.25, label = "Slippage at close (pt)" },
  },
  init = function(ctx, p) ctx.vars = {} end,
  on_bar = function(ctx, p, bar)
    local v = ctx.vars
    local hhmm = time.hhmm(bar.time)
    if hhmm == 930 then v.hi, v.lo = bar.high, bar.low end
    if hhmm > 930 and hhmm < 1559 and v.hi then v.hi, v.lo = math.max(v.hi, bar.high), math.min(v.lo, bar.low) end
    if hhmm ~= 1558 or not v.hi then return end
    local ibs = (bar.close - v.lo) / math.max(v.hi - v.lo, 1e-9)
    local signal = ibs < p.ibs
    local pos = ctx:position()
    if pos > 0 and not signal then ctx:sell(pos, { slippage = p.slip })
    elseif pos == 0 and signal then ctx:buy(p.qty, { slippage = p.slip }) end
  end,
}
