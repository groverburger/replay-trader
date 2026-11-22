-- Bollinger bands: an SMA with bands a number of (population) standard deviations away.
return indicator {
  name = "Bollinger bands",
  overlay = true,
  params = {
    { "period", 20, min = 2, max = 500 },
    { "stddev", 2.0, min = 0.5, max = 4, step = 0.1, label = "Std dev" },
  },
  label = function(p) return string.format("BB %d, %.1f", p.period, p.stddev) end,
  plots = {
    { "basis", width = 1.0, alpha = 0.6 },
    { "upper", width = 1.0 },
    { "lower", width = 1.0 },
  },
  fills = { { "upper", "lower", alpha = 0.08 } },
  calc = function(bars, p, out)
    local mid, sd = ta.sma(p.period), ta.stdev(p.period)
    for i = 1, bars.n do
      local c = bars.close[i]
      local m, s = mid:update(c), sd:update(c)
      if m and s then
        out.basis[i], out.upper[i], out.lower[i] = m, m + p.stddev * s, m - p.stddev * s
      end
    end
  end,
}
