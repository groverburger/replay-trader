-- Simple moving average.
return indicator {
  name = "SMA",
  overlay = true,
  params = {
    { "period", 20, min = 2, max = 500 },
    { "source", "close", options = { "close", "open", "high", "low", "hl2", "hlc3", "ohlc4" } },
  },
  plots = { { "SMA", width = 1.4 } },
  calc = function(bars, p, out)
    local avg, src = ta.sma(p.period), source(bars, p.source)
    for i = 1, bars.n do out.SMA[i] = avg:update(src(i)) end
  end,
}
