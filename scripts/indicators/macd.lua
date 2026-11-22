-- MACD: fast EMA minus slow EMA, its signal EMA, and the histogram between them.
return indicator {
  name = "MACD",
  overlay = false,
  params = {
    { "fast", 12, min = 2, max = 100 },
    { "slow", 26, min = 3, max = 200 },
    { "signal", 9, min = 2, max = 100 },
  },
  label = function(p) return string.format("MACD %d/%d/%d", p.fast, p.slow, p.signal) end,
  warmup = function(p) return 4 * (p.slow + p.signal) end,
  plots = {
    { "hist", style = "histogram", color = "#999999", alpha = 0.6 },
    { "MACD", width = 1.4 },
    { "signal", color = "#ff9a4d", width = 1.0 },
  },
  calc = function(bars, p, out)
    local m = ta.macd(p.fast, p.slow, p.signal)
    for i = 1, bars.n do
      out.MACD[i] = m:update(bars.close[i])
      out.signal[i], out.hist[i] = m.signal, m.hist
    end
  end,
}
