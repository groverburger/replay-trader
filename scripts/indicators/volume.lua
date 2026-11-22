-- Volume, coloured by the bar's direction, with an optional moving average.
return indicator {
  name = "Volume",
  overlay = false,
  zero_based = true,
  params = { { "average", 0, min = 0, max = 200, label = "Average (0 = off)" } },
  label = function(p) return "Volume" end,
  warmup = function(p) return 4 * p.average + 10 end,
  plots = {
    { "volume", style = "histogram", color_by = "bar", alpha = 0.7 },
    { "average", color = "#e0e0e0", width = 1.0, alpha = 0.7 },
  },
  calc = function(bars, p, out)
    local avg = p.average > 1 and ta.sma(p.average) or nil
    for i = 1, bars.n do
      out.volume[i] = bars.volume[i]
      if avg then out.average[i] = avg:update(bars.volume[i]) end
    end
  end,
}
