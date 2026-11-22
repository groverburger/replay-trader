-- Wilder's average true range.
return indicator {
  name = "ATR",
  overlay = false,
  params = { { "period", 14, min = 1, max = 200 } },
  plots = { { "ATR", width = 1.4 } },
  calc = function(bars, p, out)
    local a = ta.atr(p.period)
    for i = 1, bars.n do out.ATR[i] = a:update(bars.high[i], bars.low[i], bars.close[i]) end
  end,
}
