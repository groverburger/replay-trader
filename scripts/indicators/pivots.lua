-- Swing pivots: bar i is a pivot high if its high is above the `left` highs before it and at least the
-- `right` highs after it (lows mirrored). A pivot appears only once its right-hand bars have all closed.
-- Optional levels run from each pivot until price trades through them.
return indicator {
  name = "Swing pivots",
  overlay = true,
  params = {
    { "left", 5, min = 1, max = 50 },
    { "right", 5, min = 1, max = 50 },
    { "levels", true },
  },
  label = function(p) return string.format("Pivots %d/%d", p.left, p.right) end,
  warmup = function(p) return p.left + p.right + 1 end,
  plots = { { "pivots", style = "markers" } },
  -- One pass. Pivot i is confirmed at bar i + right; its level is tested from the bar after that. Unbroken
  -- levels form a stack whose prices only get closer to the market towards the top (a new pivot high can't be
  -- above an unbroken older one, or it would have broken it), so each bar pops the levels it breaks off the top.
  calc = function(bars, p, out)
    local h, l, n, left, right = bars.high, bars.low, bars.n, p.left, p.right
    local first, last = 1 + left, bars.last_complete - right
    local levels = p.levels
    local hs, hn, ls, ln = {}, 0, {}, 0   -- unbroken pivot highs / lows (bar indices), stack tops at hn / ln
    for j = 1, n do
      if levels then
        local hj, lj = h[j], l[j]
        while hn > 0 and h[hs[hn]] < hj do out:segment(hs[hn], j, h[hs[hn]], "pivots", 0.25); hn = hn - 1 end
        while ln > 0 and l[ls[ln]] > lj do out:segment(ls[ln], j, l[ls[ln]], "pivots", 0.25); ln = ln - 1 end
      end
      local i = j - right
      if i >= first and i <= last then
        local hi, li = h[i], l[i]
        local ph, pl = true, true
        for k = i - left, i - 1 do if h[k] >= hi then ph = false; break end end
        if ph then for k = i + 1, j do if h[k] > hi then ph = false; break end end end
        for k = i - left, i - 1 do if l[k] <= li then pl = false; break end end
        if pl then for k = i + 1, j do if l[k] < li then pl = false; break end end end
        if ph then
          out:marker(i, hi, "down", "pivots")
          if levels then hn = hn + 1; hs[hn] = i end
        end
        if pl then
          out:marker(i, li, "up", "pivots")
          if levels then ln = ln + 1; ls[ln] = i end
        end
      end
    end
    for k = 1, hn do out:segment(hs[k], n, h[hs[k]], "pivots", 0.6) end
    for k = 1, ln do out:segment(ls[k], n, l[ls[k]], "pivots", 0.6) end
  end,
}
