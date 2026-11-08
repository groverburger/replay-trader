# NQ replay trader

Bar-by-bar market replay of NQ 1-minute data with a paper broker and optional indicators. The UI is
[BUI](https://github.com/ppmpreetham/BUI) v0.5.0 (Blender's interface as a C library): the panels are Blender widgets
and the chart is a custom-drawn BUI node (`src/chart.cpp`), which replaces ImPlot.
Nothing after the replay cursor is shown or used: the current chart bar builds minute by minute, indicators
update with it, and swing pivots appear only once their right-hand bars have closed.

## Build and run

```
python3 export_bars.py                  # data/nq_1m.bin from nq_research/data/nq_1min.parquet (prices as traded)
cmake -S . -B build-bui && cmake --build build-bui -j   # fetches and builds BUI's C kit (about a minute)
./build-bui/replay_trader                   # starts on a random day at 09:25
./build-bui/replay_trader --start "2025-04-09 09:25"
./build-bui/replay_trader --demo            # adds a few indicators and makes one bracketed trade
./build-bui/replay_trader --demo --screenshot out.bmp   # renders off-screen; BUI_UI_SCALE=2 for Retina size
```

BUI is GPL-3.0-or-later, so a binary built from this branch is too. On macOS it draws on the CPU (no Metal
renderer yet) and only redraws what changed, so an idle window uses no CPU.

## Replay

- Space play/pause; speed 1-600 minutes per second (Up/Down); Right reveals one minute; Shift+Right finishes the
  current chart bar; N skips to just before the next 09:30 open. "skip overnight" jumps play over 16:00-09:30.
- Timeframes 1m, 5m, 15m, 30m, 1h, 4h, 1D (Globex session, 18:00-17:00). The forming bar is drawn slightly faded.
- The chart follows the latest bar; drag (or scroll sideways on a trackpad) to look back, scroll to zoom (L or
  "Follow" to return). Hovering shows a crosshair and the bar's OHLC.
- Fields work the Blender way: drag a number field to scrub it, Ctrl+wheel to step, or click and type (expressions
  like `20*1.5` work); Ctrl+Z undoes UI edits.
- "hide dates" shows times only, for blind practice. "random day" / "start here" begin a new session (P&L reset).

## Trading

- B / S market buy/sell, X flatten, C cancel working orders. Ticket: NQ ($20/pt) or MNQ ($2/pt), quantity,
  slippage ticks, commission per side, optional bracket (stop and target in points, attached when the entry fills).
- Limit and stop orders: from the ticket price, or Cmd/Ctrl+click (limit) / Shift+click (stop) on the chart. Drag an
  order's line to move it.
- Fills, judged on the next revealed 1-minute bar: market at the open +/- slippage; stops at the worse of stop and
  open +/- slippage; limits only when price trades through by a tick (or touches, if "limit fills on touch"), at
  the better of limit and open. Stops are checked before limits, so if a bracket's stop and target are both inside
  one minute it counts as stopped out.
- Contract rolls: prices are the front month as traded, so they jump at a roll; an open position and working orders
  are shifted by the same amount (logged), so the roll itself creates no P&L.
- Trades list, win rate, profit factor, max drawdown and equity curve; "export CSV" writes sessions/session_*.csv.

## Indicators (none by default)

SMA, EMA, Bollinger bands, session VWAP (resets 18:00), swing pivots (left/right bars, optional levels drawn until
traded through), RSI, MACD, ATR, volume. Add any number from the Indicators panel; each has its own settings,
colour (click the swatch for Blender's colour picker) and on/off switch. Oscillators get their own panels under the price chart.
