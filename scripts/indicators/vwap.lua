-- Session VWAP (resets at the 18:00 Globex open) from each bar's typical price, with optional bands.
return indicator {
  name = "VWAP (session)",
  overlay = true,
  params = {
    { "bands", 0.0, min = 0, max = 4, step = 0.5, label = "Bands (sd, 0 = off)" },
  },
  label = function(p) return p.bands > 0 and string.format("VWAP, %.1f sd", p.bands) or "VWAP" end,
  plots = {
    { "VWAP", width = 1.6 },
    { "upper", width = 1.0, alpha = 0.5 },
    { "lower", width = 1.0, alpha = 0.5 },
  },
  fills = { { "upper", "lower", alpha = 0.05 } },
  calc = function(bars, p, out)
    local vw = ta.vwap()
    for i = 1, bars.n do
      local tp = (bars.high[i] + bars.low[i] + bars.close[i]) / 3
      local v = vw:update(tp, bars.volume[i], time.session(bars.time[i]))
      out.VWAP[i] = v
      if p.bands > 0 then out.upper[i], out.lower[i] = v + p.bands * vw.sd, v - p.bands * vw.sd end
    end
  end,
}
