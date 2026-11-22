-- Exponential moving average (seeded with the SMA of its first values).
return indicator {
  name = "EMA",
  overlay = true,
  params = {
    { "period", 21, min = 2, max = 500 },
    { "source", "close", options = { "close", "open", "high", "low", "hl2", "hlc3", "ohlc4" } },
  },
  plots = { { "EMA", width = 1.4 } },
  calc = function(bars, p, out)
    local avg, src = ta.ema(p.period), source(bars, p.source)
    for i = 1, bars.n do out.EMA[i] = avg:update(src(i)) end
  end,
}
