# Development notes

## Code layout

| File | What it does |
|---|---|
| `src/main.cpp` | Startup: options, API key, data screen or trader, fonts and theme, window loop, frame timing, headless screenshots |
| `src/setup.cpp` | The data screen (API key, download progress, cached-data fallback) |
| `src/datafeed.cpp` | API client, per-month cache, building the market, contract rolls |
| `src/http.cpp` | HTTPS GET: libcurl on macOS/Linux, WinHTTP on Windows |
| `src/market.cpp` | 1-minute bars, aggregation into chart timeframes, the partly revealed bar |
| `src/broker.cpp` | Paper broker: orders, fills, brackets, P&L, roll shifts |
| `src/app.cpp` | Replay state and actions shared by the panels and the chart |
| `src/panels.cpp` | Left-hand panels, number fields, smooth scrolling |
| `src/chart.cpp` | Price chart, oscillator panes and equity curves (ImGui draw lists), pan / zoom / order input |
| `src/softraster.cpp` | CPU rasterizer for ImGui draw data (`--screenshot` without a window or GPU) |
| `src/mac_metal.mm`, `src/mac_input.m` | macOS: the Metal renderer; telling trackpad scrolling from wheel clicks |
| `src/civil.h` | Calendar arithmetic without libc time functions (they differ across platforms) |
| `src/script.cpp` | Lua: script discovery and hot reload, definitions, params, running indicators |
| `src/strategy.cpp` | Strategies: the `ctx` bindings, minute-by-minute driving, backtests on a worker thread, stats |
| `scripts/lib/prelude.lua` | The Lua side of the API: `indicator`/`strategy`, `time`, `ta`, the `ctx` methods |
| `scripts/indicators/`, `scripts/strategies/` | Built-in scripts, compiled into the binary (`cmake/embed_files.cmake`) |

The UI is [Dear ImGui](https://github.com/ocornut/imgui) 1.91.9b on [GLFW](https://www.glfw.org) 3.4, rebuilt
every frame from the app state (immediate mode). It renders on the GPU: Metal on macOS (a `CAMetalLayer`, which
presents at the display's rate, 120 Hz on ProMotion; OpenGL on macOS doesn't pace to ProMotion), OpenGL 3
elsewhere, with an OpenGL 2 fallback for old drivers and VMs. Text is Inter 4.0 (embedded), rasterized at the
display's pixel density.

**Frames.** The loop sleeps in `glfwWaitEventsTimeout` while nothing changes, so an idle window draws nothing. Input
draws a few frames (ImGui settles hover and clicks over two); playback, zoom and pan easing, smooth scrolling and
the price axis easing keep frames coming at the display's rate (`keep_animating`); the downloader and backtests
wake the loop from their threads with `glfwPostEmptyEvent`. User scripts are checked once a second.

**Scrolling.** GLFW gives the scroll callback a trackpad's precise deltas (on macOS, pixels times 0.1;
`mac_input.m` tells them from wheel clicks, which arrive as lines) and Windows touchpads' fractions of a notch.
`ScrollEvent`s are in interface units; a wheel click is 40 units.

- **Sidebar and trade list** (`begin_smooth_child`): trackpad deltas move them 1:1 with the fingers, wheel clicks
  glide. The position is kept as a float, so slow swipes don't get lost in ImGui's whole-unit scroll rounding.
- **Chart:** sideways scrolling pans 1:1, vertical scrolling zooms continuously; wheel clicks ease (Shift turns a
  wheel sideways).
- ImGui's own scrolling (dropdown lists) gets the same deltas, scaled so it also moves 1:1.

**Frame times** at Retina size (3200 x 1900 px, M-series MacBook Pro, ProMotion), from `RT_BENCH`, which feeds
synthetic scroll input through the same path as the trackpad: building and encoding a frame takes 0.2–0.3 ms on
average (p95 under 1 ms) for chart panning, zooming, wheel zooming and sidebar scrolling, and frames arrive every
8.3 ms (120 Hz). The BUI version took about 7 ms to draw a chart pan or zoom frame on the CPU plus about 3 ms to
present it (about 75 fps).

| Variable | Effect |
|---|---|
| `RT_FRAME_LOG=1` | Log every frame's CPU time, presentation wait and interval to stderr, with a summary on exit |
| `RT_BENCH=pan`, `zoom`, `wheel`, `scroll`, `scrollwheel` | Replay 4 s of synthetic scrolling over the chart or the sidebar in the real window, print frame-time statistics and quit |

**Scripting.** Each script runs in its own environment on top of the prelude. Scripts get no file or OS access,
and a time-limit hook stops runaway loops.

- **Indicators** run on the UI thread, once per change of the visible series, params or scripts (cached per
  indicator).
- **Live strategies** share that Lua state and run on the replay's broker.
- **Backtests** load the strategy into their own Lua state on a worker thread and run it on a private broker
  over the shared, read-only market. That takes about 0.1–0.4 s per year of 1-minute data.

Backtests can run on `qc_roll_schedule(m)` (`src/datafeed.cpp`), a copy of the market that rolls on
QuantConnect's schedule. That schedule was determined empirically by comparing against QuantConnect backtests; the
2025-09 and 2025-12 spreads were measured from QuantConnect's own fills. With it, a Lua port of a production
QuantConnect strategy matched an independent, QuantConnect-validated reference engine on 100% of 1,936 trades for
2020–2025, with identical P&L.

The Lua indicators were checked against the old C++ implementations on 1m, 5m and 1h series. They match to
within 1e-11, with identical missing-value boundaries and identical pivots.

## Data

Bars come from the karaiq Futures Data API (`https://api.karaiq.com/data/NQ?bar=1min&security_type=future`, with
an `X-API-Key` header). The cache is a per-user folder: `~/Library/Application Support/Replay Trader/` on
macOS, `%APPDATA%\Replay Trader\` on Windows, `~/.local/share/replay-trader/` on Linux.
`NQRT_DATA_DIR` overrides it.

- The API serves the continuous front month, newest first, with US Central bar-start times. It returns at most
  1,380 rows per request, which is exactly one Globex day. So the app requests one Central calendar day at a time
  and skips Saturdays.
- The server handles one request per key at a time (about 0.9 s each), so the full history takes about an hour no
  matter how many requests are in flight. The app keeps 4 in flight to hide the round trip.
- Months are fetched newest first and each is written only once it is complete
  (`cache/NQ_1min/YYYY-MM.bin`: magic `NQRAW001`, int64 count, then 24-byte rows of int32 Central minute and float32
  OHLCV). A download that stops leaves whole months behind and resumes with the missing ones. Each start also
  re-fetches the last week and anything after the newest cached bar.
- New York time is Central plus one hour, all year round. Prices are as traded (front month).

## Contract rolls

The continuous series splices to the next quarterly contract a few days before expiry. No trader sees that
price jump: a held position would have been rolled. So the app shifts an open position and its working orders
by the jump instead of booking it as P&L.

`kRolls` in `src/datafeed.cpp` lists the splice minute and jump for every roll through 2026-09:

- **Through 2024-06:** the splice is 00:00 on the 14th, with spreads measured on 15-minute bars of the same data.
- **From 2025-09:** the actual 1-minute splice minute. The 15-minute roll times used in that research data are
  7–14 minutes early on five rolls, which would make a held position show a fake jump of about 230–290 points.

Later quarters are detected. The splice is taken to be the largest open-minus-previous-close jump on the 8th–16th
of the expiry month, ignoring jumps across gaps of more than 10 minutes. It must be at least half the previous
roll's jump and 4 times the next-largest move. Run `replay_trader --list-rolls` to print the roll points the
current cache produces.

## Options for development

| Option | Effect |
|---|---|
| `--screenshot out.bmp` | Render off-screen on the CPU and exit: no window or GPU needed (`RT_UI_SCALE=2` for Retina size, `RT_SHOT_SIZE=850x500` for another size). It downloads first if there's a key |
| `--demo` | Start with a few indicators and a bracketed trade. `RT_SHOT_INDS=macd,pivots` picks other indicators (script ids, user scripts included); `RT_SHOT_ADVANCE=300` replays that many minutes first (default 70) and `RT_SHOT_AFTER=5` that many after the demo's entry (default 25). `RT_SHOT_AUTOTRADE=1` ticks Auto-trade on indicators that trade and only replays |
| `--show-setup` | With `--screenshot`, render the data screen instead |
| `--show-strategy` | The opening range breakout trading the replay, with a 2025 backtest and the sidebar scrolled to the Strategy panel (`docs/strategy.png`). `RT_SHOT_TRADE=7` jumps to the backtest's seventh trade; `RT_SHOT_ZOOM=10` then widens the view tenfold |
| `--from YYYY-MM-DD` | Download only from that day (for testing the downloader) |
| `--data file.bin` | Load a file in the `NQ1M0001` format instead of the cache (`tools/export_bars.py`, CI's `tools/make_test_data.py`) |
| `--list-rolls` | Print the cached market's roll points and exit |

The screenshots in `docs/` come from the `RT_DIST` build (so the chart uses Inter), off-screen at 2x:

```sh
APP="build-mac/Replay Trader.app/Contents/MacOS/Replay Trader"; D=data/nq_1m.bin
RT_UI_SCALE=2 "$APP" --data $D --start "2025-04-09 09:25" --demo --screenshot trader.bmp
RT_UI_SCALE=2 "$APP" --data $D --start "2025-06-05 09:25" --show-strategy --screenshot strategy.bmp
HOME=/Users/you RT_SHOT_SIZE=850x500 RT_UI_SCALE=2 "$APP" --show-setup --screenshot data-screen.bmp
sips -s format png trader.bmp --out docs/trader.png   # and so on
```

## Releases

`.github/workflows/release.yml` runs on version tags (`v*`) and on manual dispatch:

- **macOS** (macos-15): universal `.app`, ad-hoc signed.
- **Windows** (MSYS2 MinGW-w64): a static `.exe`.

Each build is smoke-tested by rendering a screenshot from synthetic data. The Windows job also checks that a bad
API key is rejected over WinHTTP. On a tag, the zips are attached to a GitHub release.

To build the same zips locally, run `packaging/package.sh` (Windows via `brew install mingw-w64`).

The builds aren't notarized or Authenticode-signed. Users get the one-time Gatekeeper or SmartScreen prompt
described in the README.

The local developer build loads a chart font from `../strategy_report/pdf_src/fonts/` if it exists. That font
isn't redistributable, so it is never in an `RT_DIST` build. Users can drop any `chart_font.ttf` into the data
folder; otherwise the chart uses the UI font (Inter).
