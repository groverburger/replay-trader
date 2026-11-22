-- Key session levels for the current day: the prior regular session's high, low and close, the overnight
-- (18:00-09:30) high and low once the overnight session is over, and today's 09:30 open. Intraday charts only.
return indicator {
  name = "Session levels",
  overlay = true,
  params = {
    { "prior_day", true, label = "Prior day H/L/C" },
    { "overnight", true, label = "Overnight H/L" },
    { "open", true, label = "RTH open" },
  },
  label = function(p) return "Session levels" end,
  warmup = function(p) return 3000 end,  -- enough minutes to have seen the prior session
  plots = {
    { "PDH", style = "step", color = "#4cc38a", width = 1.2 },
    { "PDL", style = "step", color = "#f06a5a", width = 1.2 },
    { "PDC", style = "step", color = "#b0b0b0", width = 1.0, alpha = 0.7 },
    { "ONH", style = "step", color = "#6aa8ff", width = 1.0 },
    { "ONL", style = "step", color = "#6aa8ff", width = 1.0 },
    { "open", style = "step", color = "#ffd24d", width = 1.0 },
  },
  calc = function(bars, p, out)
    if bars.tf >= 1440 then return end
    local sess, rth_h, rth_l, rth_c, prior, on_h, on_l, opn, on_done = nil
    for i = 1, bars.n do
      local t = bars.time[i]
      local s, tod = time.session(t), time.tod(t)
      if s ~= sess then  -- a new Globex session: today's RTH becomes "prior day"
        if rth_h then prior = { rth_h, rth_l, rth_c } end
        sess, rth_h, rth_l, rth_c, on_h, on_l, opn, on_done = s, nil, nil, nil, nil, nil, nil, false
      end
      local rth = tod >= 570 and tod < 960
      if rth then
        on_done = true
        opn = opn or bars.open[i]
        rth_h = math.max(rth_h or bars.high[i], bars.high[i])
        rth_l = math.min(rth_l or bars.low[i], bars.low[i])
        rth_c = bars.close[i]
      elseif not on_done then
        on_h = math.max(on_h or bars.high[i], bars.high[i])
        on_l = math.min(on_l or bars.low[i], bars.low[i])
      end
      if p.prior_day and prior then out.PDH[i], out.PDL[i], out.PDC[i] = prior[1], prior[2], prior[3] end
      if p.overnight and on_done then out.ONH[i], out.ONL[i] = on_h, on_l end
      if p.open and opn then out.open[i] = opn end
    end
  end,
}
