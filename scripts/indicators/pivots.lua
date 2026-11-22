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
  calc = function(bars, p, out)
    local h, l, n = bars.high, bars.low, bars.n
    for i = 1 + p.left, bars.last_complete - p.right do
      local ph, pl = true, true
      for j = i - p.left, i + p.right do
        if j ~= i then
          if (j < i and h[j] >= h[i]) or (j > i and h[j] > h[i]) then ph = false end
          if (j < i and l[j] <= l[i]) or (j > i and l[j] < l[i]) then pl = false end
          if not (ph or pl) then break end
        end
      end
      for _, high in ipairs({ true, false }) do
        if (high and ph) or (not high and pl) then
          local price = high and h[i] or l[i]
          out:marker(i, price, high and "down" or "up", "pivots")
          if p.levels then
            local broken = nil
            for j = i + p.right + 1, n do
              if (high and h[j] > price) or (not high and l[j] < price) then broken = j; break end
            end
            out:segment(i, broken or n, price, "pivots", broken and 0.25 or 0.6)
          end
        end
      end
    end
  end,
}
