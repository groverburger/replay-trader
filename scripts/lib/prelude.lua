-- Replay Trader script library, loaded before every indicator and strategy script.
-- Reference: docs/SCRIPTING.md (also copied into the scripts folder as README.md).
--
-- Times are New York minutes since 1970-01-01 (naive local time), the same clock as the chart.

local floor, max, min, abs, sqrt = math.floor, math.max, math.min, math.abs, math.sqrt

-------------------------------------------------------------------------------- definitions
function indicator(def) def.kind = "indicator"; return def end
function strategy(def) def.kind = "strategy"; return def end

-------------------------------------------------------------------------------- time helpers
time = {}
function time.tod(t) return t % 1440 end                                   -- minutes since midnight
function time.hhmm(t) local m = t % 1440; return floor(m / 60) * 100 + m % 60 end   -- 9:35 -> 935
function time.session(t) return floor((t + 360) / 1440) end                -- Globex session id (18:00 starts the next)
function time.is_rth(t) local m = t % 1440; return m >= 570 and m < 960 end -- 09:30-16:00
function time.weekday(t) return (floor(t / 1440) + 4) % 7 end              -- 0 Sunday .. 6 Saturday
function time.ymd(t)
  local z = floor(t / 1440) + 719468
  local era = floor(z / 146097)
  local doe = z - era * 146097
  local yoe = floor((doe - floor(doe / 1460) + floor(doe / 36524) - floor(doe / 146096)) / 365)
  local doy = doe - (365 * yoe + floor(yoe / 4) - floor(yoe / 100))
  local mp = floor((5 * doy + 2) / 153)
  local d = doy - floor((153 * mp + 2) / 5) + 1
  local m = mp < 10 and mp + 3 or mp - 9
  return yoe + era * 400 + (m <= 2 and 1 or 0), m, d
end
function time.date(t) return string.format("%04d-%02d-%02d", time.ymd(t)) end

-- Price source by name for indicators: returns function(i) -> value.
function source(bars, name)
  local o, h, l, c = bars.open, bars.high, bars.low, bars.close
  if name == "open" then return function(i) return o[i] end
  elseif name == "high" then return function(i) return h[i] end
  elseif name == "low" then return function(i) return l[i] end
  elseif name == "hl2" then return function(i) return (h[i] + l[i]) / 2 end
  elseif name == "hlc3" then return function(i) return (h[i] + l[i] + c[i]) / 3 end
  elseif name == "ohlc4" then return function(i) return (o[i] + h[i] + l[i] + c[i]) / 4 end
  end
  return function(i) return c[i] end
end

-------------------------------------------------------------------------------- ta: streaming indicators
-- Each constructor returns an object; call :update(...) once per bar. It returns the new value (also in
-- .value), or nil until enough bars have been seen.
ta = {}
local function class() local c = {}; c.__index = c; return c end

local SMA = class()
function ta.sma(n) return setmetatable({ n = n, buf = {}, i = 0, sum = 0, count = 0 }, SMA) end
function SMA:update(x)
  self.i = self.i % self.n + 1
  local old = self.buf[self.i]
  if old then self.sum = self.sum - old end
  self.buf[self.i] = x
  self.sum = self.sum + x
  if self.count < self.n then self.count = self.count + 1 end
  self.value = self.count == self.n and self.sum / self.n or nil
  return self.value
end

-- EMA seeded with the SMA of its first n values.
local EMA = class()
function ta.ema(n) return setmetatable({ n = n, a = 2 / (n + 1), count = 0, sum = 0 }, EMA) end
function EMA:update(x)
  if self.count < self.n then
    self.count = self.count + 1
    self.sum = self.sum + x
    if self.count == self.n then self.value = self.sum / self.n end
  else
    self.value = self.a * x + (1 - self.a) * self.value
  end
  return self.value
end

-- Wilder's moving average (RSI, ATR), seeded with an SMA.
local RMA = class()
function ta.rma(n) return setmetatable({ n = n, count = 0, sum = 0 }, RMA) end
function RMA:update(x)
  if self.count < self.n then
    self.count = self.count + 1
    self.sum = self.sum + x
    if self.count == self.n then self.value = self.sum / self.n end
  else
    self.value = (self.value * (self.n - 1) + x) / self.n
  end
  return self.value
end

-- Population standard deviation over the last n values.
local STDEV = class()
function ta.stdev(n) return setmetatable({ n = n, buf = {}, i = 0, count = 0 }, STDEV) end
function STDEV:update(x)
  self.i = self.i % self.n + 1
  self.buf[self.i] = x
  if self.count < self.n then self.count = self.count + 1 end
  if self.count < self.n then self.value = nil; return nil end
  local s = 0
  for k = 1, self.n do s = s + self.buf[k] end
  local mean, v = s / self.n, 0
  for k = 1, self.n do local d = self.buf[k] - mean; v = v + d * d end
  self.value = sqrt(v / self.n)
  return self.value
end

local RSI = class()
function ta.rsi(n) return setmetatable({ gain = ta.rma(n), loss = ta.rma(n) }, RSI) end
function RSI:update(x)
  local prev = self.prev
  self.prev = x
  if not prev then return nil end
  local d = x - prev
  local g, l = self.gain:update(max(d, 0)), self.loss:update(max(-d, 0))
  if not g then return nil end
  self.value = l == 0 and 100 or 100 - 100 / (1 + g / l)
  return self.value
end

local ATR = class()
function ta.atr(n) return setmetatable({ avg = ta.rma(n) }, ATR) end
function ATR:update(h, l, c)
  local pc = self.pc
  self.pc = c
  local tr = pc and max(h - l, abs(h - pc), abs(l - pc)) or h - l
  self.value = self.avg:update(tr)
  return self.value
end

-- MACD: .value is the MACD line, .signal and .hist as well.
local MACD = class()
function ta.macd(fast, slow, signal)
  return setmetatable({ f = ta.ema(fast), s = ta.ema(slow), sig = ta.ema(signal) }, MACD)
end
function MACD:update(x)
  local f, s = self.f:update(x), self.s:update(x)
  if not (f and s) then return nil end
  self.value = f - s
  self.signal = self.sig:update(self.value)
  self.hist = self.signal and self.value - self.signal or nil
  return self.value
end

local EXTREME = class()
local function extreme(n, better)
  return setmetatable({ n = n, buf = {}, i = 0, count = 0, better = better }, EXTREME)
end
function ta.highest(n) return extreme(n, function(a, b) return a > b end) end
function ta.lowest(n) return extreme(n, function(a, b) return a < b end) end
function EXTREME:update(x)
  self.i = self.i % self.n + 1
  self.buf[self.i] = x
  if self.count < self.n then self.count = self.count + 1 end
  local best = nil
  for k = 1, self.count do
    local v = self.buf[k]
    if best == nil or self.better(v, best) then best = v end
  end
  self.value = best
  return best
end

-- Volume-weighted average price, reset whenever `session` changes (pass time.session(t)).
-- .sd is the volume-weighted standard deviation, for bands.
local VWAP = class()
function ta.vwap() return setmetatable({}, VWAP) end
function VWAP:update(price, volume, session)
  if session ~= self.session then
    self.session, self.pv, self.pv2, self.vol = session, 0, 0, 0
  end
  self.pv = self.pv + price * volume
  self.pv2 = self.pv2 + price * price * volume
  self.vol = self.vol + volume
  if self.vol > 0 then
    self.value = self.pv / self.vol
    self.sd = sqrt(max(0, self.pv2 / self.vol - self.value * self.value))
  else
    self.value, self.sd = price, 0
  end
  return self.value
end

function ta.crossover(a_prev, a, b_prev, b) return a_prev and b_prev and a_prev <= b_prev and a > b end
function ta.crossunder(a_prev, a, b_prev, b) return a_prev and b_prev and a_prev >= b_prev and a < b end

-------------------------------------------------------------------------------- indicator output (used by the app)
local OUT = class()
function __new_output(names)
  local out = setmetatable({ _markers = {}, _segments = {} }, OUT)
  for _, n in ipairs(names) do out[n] = {} end
  return out
end
-- A triangle at bar i: dir "up" (below the price, pointing up) or "down" (above, pointing down).
function OUT:marker(i, price, dir, plot)
  self._markers[#self._markers + 1] = { i, price, dir == "up", plot }
end
-- A horizontal line from bar i0 to bar i1 at price.
function OUT:segment(i0, i1, price, plot, alpha)
  self._segments[#self._segments + 1] = { i0, i1, price, plot, alpha or 1 }
end

-------------------------------------------------------------------------------- strategy context (used by the app)
function __make_ctx(core)
  local ctx = { vars = {}, tick = 0.25 }
  ctx.point_value = core.point_value()

  -- bars on the strategy's timeframe: k = 0 is the bar that just closed, 1 the one before, ...
  function ctx:open(k) return core.get(k or 0, 1) end
  function ctx:high(k) return core.get(k or 0, 2) end
  function ctx:low(k) return core.get(k or 0, 3) end
  function ctx:close(k) return core.get(k or 0, 4) end
  function ctx:volume(k) return core.get(k or 0, 5) end
  function ctx:time(k) return core.get(k or 0, 6) end           -- bar start
  function ctx:bar_count() return core.count() end
  function ctx:highest(n, k)
    local best = nil
    for j = k or 0, (k or 0) + n - 1 do local v = core.get(j, 2); if v and (not best or v > best) then best = v end end
    return best
  end
  function ctx:lowest(n, k)
    local best = nil
    for j = k or 0, (k or 0) + n - 1 do local v = core.get(j, 3); if v and (not best or v < best) then best = v end end
    return best
  end

  -- clock: the minute that just closed
  function ctx:now() return core.now() end
  function ctx:hhmm() return time.hhmm(core.now()) end
  function ctx:date() return time.date(core.now()) end
  function ctx:rth() return time.is_rth(core.now() - 1) end
  function ctx:session() return time.session(core.now() - 1) end

  -- account
  function ctx:position() return (core.position()) end             -- signed contracts
  function ctx:avg_price() return select(2, core.position()) end
  function ctx:open_pnl() return select(3, core.position()) end    -- $ on the open position
  function ctx:closed_pnl() return select(4, core.position()) end  -- $ realized, after commission
  function ctx:points_open() local q, avg = core.position(); return q == 0 and 0 or (core.last() - avg) * (q > 0 and 1 or -1) end
  function ctx:last() return core.last() end

  -- orders. opts: limit=price | stop=price (else market), stop_loss=points, take_profit=points (a bracket
  -- attached when the entry fills). Returns the order id. Orders fill from the next minute on.
  local function submit(side, qty, opts)
    opts = opts or {}
    local kind, price = "market", 0
    if opts.limit then kind, price = "limit", opts.limit elseif opts.stop then kind, price = "stop", opts.stop end
    return core.submit(side, qty or 1, kind, price, opts.stop_loss or 0, opts.take_profit or 0)
  end
  function ctx:buy(qty, opts) return submit(1, qty, opts) end
  function ctx:sell(qty, opts) return submit(-1, qty, opts) end
  function ctx:flatten()
    core.cancel_all()
    local q = core.position()
    if q ~= 0 then return submit(q > 0 and -1 or 1, abs(q)) end
  end
  function ctx:cancel(id) core.cancel(id) end
  function ctx:cancel_all() core.cancel_all() end
  function ctx:modify(id, price) return core.modify(id, price) end
  -- working orders: { id, side (+1/-1), type ("market"/"limit"/"stop"), price, qty, protective (bracket leg) }
  function ctx:orders() return core.orders() end
  -- the protective stop of the current position (a bracket leg), or nil
  function ctx:stop_order()
    for _, o in ipairs(core.orders()) do if o.protective and o.type == "stop" then return o end end
  end
  function ctx:target_order()
    for _, o in ipairs(core.orders()) do if o.protective and o.type == "limit" then return o end end
  end
  function ctx:round_tick(p) return floor(p / 0.25 + 0.5) * 0.25 end

  function ctx:log(...)
    local parts = {}
    for i = 1, select("#", ...) do parts[#parts + 1] = tostring((select(i, ...))) end
    core.log(table.concat(parts, " "))
  end
  return ctx
end
