-- Wilder's relative strength index.
return indicator {
  name = "RSI",
  overlay = false,
  range = { 0, 100 },
  levels = { 30, 70 },
  params = { { "period", 14, min = 2, max = 200 } },
  plots = { { "RSI", width = 1.4 } },
  calc = function(bars, p, out)
    local r = ta.rsi(p.period)
    for i = 1, bars.n do out.RSI[i] = r:update(bars.close[i]) end
  end,
}
