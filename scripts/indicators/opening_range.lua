-- Opening range: the high and low of the first minutes after the 09:30 open, drawn for the rest of the
-- regular session once the range is complete. Intraday charts only.
return indicator {
  name = "Opening range",
  overlay = true,
  params = { { "minutes", 15, min = 1, max = 120 } },
  label = function(p) return "OR " .. p.minutes .. "m" end,
  warmup = function(p) return 600 end,
  plots = {
    { "high", style = "step", width = 1.2 },
    { "low", style = "step", width = 1.2 },
  },
  fills = { { "high", "low", alpha = 0.06 } },
  calc = function(bars, p, out)
    if bars.tf >= 1440 then return end
    local sess, hi, lo
    for i = 1, bars.n do
      local t = bars.time[i]
      local s, tod = time.session(t), time.tod(t)
      if s ~= sess then sess, hi, lo = s, nil, nil end
      if tod >= 570 and tod < 570 + p.minutes then
        hi = math.max(hi or bars.high[i], bars.high[i])
        lo = math.min(lo or bars.low[i], bars.low[i])
      elseif tod >= 570 + p.minutes and tod < 960 and hi then
        out.high[i], out.low[i] = hi, lo
      end
    end
  end,
}
