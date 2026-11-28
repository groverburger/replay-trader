-- Session volume profile: volume at each price row, drawn as horizontal bars from the session's first bar, with
-- the point of control (POC, the row with the most volume) and the value area (the rows around the POC holding
-- 70% of the volume) between VAL and VAH. Bars have no volume-at-price detail, so each bar's volume is spread
-- evenly over the rows from its low to its high; on 1-minute bars that is close to a tick-built profile.
-- The current session's profile grows as the replay reveals it. Optionally, the prior session's POC/VAH/VAL are
-- carried across the next session. Intraday charts only.
return indicator {
  name = "Volume profile",
  overlay = true,
  params = {
    { "session", "rth", options = { "rth", "globex" }, label = "Session" },
    { "row", 1.0, min = 0.25, max = 50, step = 0.25, label = "Row size (pt)" },
    { "va", 70, min = 10, max = 100, label = "Value area %" },
    { "width", 0.35, min = 0.05, max = 1, step = 0.05, label = "Width (share of session)" },
    { "bars", true, label = "Profile bars" },
    { "prior", true, label = "Prior session POC/VA" },
  },
  label = function(p) return string.format("Volume profile %s %gpt", p.session:upper(), p.row) end,
  warmup = function(p) return 3000 end,  -- enough minutes to hold the prior session
  plots = {
    { "POC", style = "step", color = "#ffd24d", width = 1.6 },
    { "VAH", style = "step", color = "#6aa8ff", width = 1.1 },
    { "VAL", style = "step", color = "#6aa8ff", width = 1.1 },
    { "pPOC", style = "step", color = "#ffd24d", width = 1.0, alpha = 0.45 },
    { "pVAH", style = "step", color = "#6aa8ff", width = 1.0, alpha = 0.45 },
    { "pVAL", style = "step", color = "#6aa8ff", width = 1.0, alpha = 0.45 },
    { "volume", style = "markers", color = "#9aa0a6" },   -- profile rows outside the value area
    { "value area", style = "markers", color = "#6aa8ff" },
  },
  calc = function(bars, p, out)
    if bars.tf >= 1440 then return end
    local floor, rth_only = math.floor, p.session == "rth"
    local span = floor((rth_only and 390 or 1380) / bars.tf * p.width + 0.5)  -- bars for the longest row
    local sessions, cur = {}, nil

    -- 1. Group bars into sessions and build each session's volume at price.
    for i = 1, bars.n do
      local t = bars.time[i]
      local tod = time.tod(t)
      local inside = not rth_only or (tod >= 570 and tod < 960)
      local key = rth_only and floor(t / 1440) or time.session(t)
      if inside then
        if not cur or cur.key ~= key then
          cur = { key = key, i0 = i, i1 = i, vol = {}, total = 0, lo = nil, hi = nil }
          sessions[#sessions + 1] = cur
        end
        cur.i1 = i
        local v = bars.volume[i]
        if v and v > 0 then
          local r0, r1 = floor(bars.low[i] / p.row + 1e-9), floor(bars.high[i] / p.row + 1e-9)
          local share = v / (r1 - r0 + 1)
          for r = r0, r1 do cur.vol[r] = (cur.vol[r] or 0) + share end
          cur.total = cur.total + v
          cur.lo = cur.lo and math.min(cur.lo, r0) or r0
          cur.hi = cur.hi and math.max(cur.hi, r1) or r1
          cur.last = bars.close[i]
        end
      end
    end

    -- 2. POC and value area per session.
    for _, s in ipairs(sessions) do
      if s.total > 0 then
        local vol, top, poc = s.vol, -1, nil
        local ref = floor(s.last / p.row + 1e-9)
        for r = s.lo, s.hi do
          local v = vol[r] or 0
          if v > top or (v == top and math.abs(r - ref) < math.abs(poc - ref)) then top, poc = v, r end
        end
        local target, inside, dn, up = p.va / 100 * s.total, vol[poc], poc, poc
        while inside < target and (dn > s.lo or up < s.hi) do
          local below = dn > s.lo and (vol[dn - 1] or 0) or -1
          local above = up < s.hi and (vol[up + 1] or 0) or -1
          if above >= below then up = up + 1; inside = inside + (vol[up] or 0)
          else dn = dn - 1; inside = inside + (vol[dn] or 0) end
        end
        s.poc, s.dn, s.up, s.top = poc, dn, up, top
        s.POC, s.VAL, s.VAH = (poc + 0.5) * p.row, dn * p.row, (up + 1) * p.row
      end
    end

    -- 3. Draw: profile rows from the session's first bar, levels across the session, prior levels carried over.
    local prev
    for _, s in ipairs(sessions) do
      if s.poc then
        if p.bars then
          for r = s.lo, s.hi do
            local v = s.vol[r]
            if v and v > 0 then
              local len = floor(span * v / s.top + 0.5)
              local in_va = r >= s.dn and r <= s.up
              out:segment(s.i0, s.i0 + len, (r + 0.5) * p.row, in_va and "value area" or "volume",
                          r == s.poc and 1 or (in_va and 0.55 or 0.4))
            end
          end
        end
        for i = s.i0, s.i1 do
          out.POC[i], out.VAH[i], out.VAL[i] = s.POC, s.VAH, s.VAL
          if p.prior and prev then out.pPOC[i], out.pVAH[i], out.pVAL[i] = prev.POC, prev.VAH, prev.VAL end
        end
        prev = s
      end
    end
  end,
}
