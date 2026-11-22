# Development notes

## Code layout

| File | What it does |
|---|---|
| `src/main.cpp` | Startup: options, API key, data screen or trader, window loop, headless screenshots |
| `src/setup.cpp` | The data screen (API key, download progress, cached-data fallback) |
| `src/datafeed.cpp` | API client, per-month cache, building the market, contract rolls |
| `src/http.cpp` | HTTPS GET: libcurl on macOS/Linux, WinHTTP on Windows |
| `src/market.cpp` | 1-minute bars, aggregation into chart timeframes, the partly revealed bar |
| `src/broker.cpp` | Paper broker: orders, fills, brackets, P&L, roll shifts |
| `src/app.cpp` | Replay state and actions shared by the panels and the chart |
| `src/panels.cpp` | Left-hand panels (BUI widget tree) |
| `src/chart.cpp` | Price chart, oscillator panes and equity curve (custom-drawn BUI nodes) |
| `src/civil.h` | Calendar arithmetic without libc time functions (they differ across platforms) |
| `src/script.cpp` | Lua: script discovery and hot reload, definitions, params, running indicators |
| `src/strategy.cpp` | Strategies: the `ctx` bindings, minute-by-minute driving, backtests on a worker thread, stats |
| `scripts/lib/prelude.lua` | The Lua side of the API: `indicator`/`strategy`, `time`, `ta`, the `ctx` methods |
| `scripts/indicators/`, `scripts/strategies/` | Built-in scripts, compiled into the binary (`cmake/embed_files.cmake`) |

The UI is [BUI](https://github.com/ppmpreetham/BUI) v0.5.0, Blender's interface as a C library. On macOS it
draws on the CPU and only redraws what changed, so an idle window uses no CPU. The chart therefore invalidates
itself only when something it draws has changed (`chart_prepare` keeps a signature), not on every frame.

**Scrolling patch.** `packaging/bui/precise-scroll.patch` is applied to BUI at fetch time. Upstream, the macOS
host rounds trackpad scrolling to whole wheel notches, so scroll regions jump about 40 px at a time and the chart
zooms in steps. With the patch:

- **Trackpad input:** precise deltas reach `BK_scroll` as fractional notches. A notch is one wheel click, or 2
  widget units.
- **Scroll regions:** they move by exact pixels. A move re-arranges only that region, shifts the pixels already
  drawn, and draws just the newly exposed strip and the scrollbar. Upstream relaid out and redrew the whole
  window, chart included, for every pixel scrolled. A sidebar scroll frame went from 8–12 ms to about 1.3 ms
  at Retina size, and the result was checked pixel-identical to a full redraw.
- **Custom nodes:** they get fractional `wheel_x` and `wheel_y` values.
- **Windows:** the host already sends touchpad fractions to `BK_scroll`, so it benefits too.

Mouse wheels are unchanged. The chart pans 1:1 with the fingers and zooms continuously on a trackpad, and eases
whole mouse-wheel notches.

**Scripting.** Each script runs in its own environment on top of the prelude. Scripts get no file or OS access,
and a time-limit hook stops runaway loops.

- **Indicators** run on the UI thread, once per change of the visible series, params or scripts (cached per
  indicator).
- **Live strategies** share that Lua state and run on the replay's broker.
- **Backtests** load the strategy into their own Lua state on a worker thread and run it on a private broker
  over the shared, read-only market. That takes about 0.1–0.4 s per year of 1-minute data.

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

- **Through 2024-06:** the splice is 00:00 on the 14th, with the spreads from Team Safi's 15-minute research data.
- **From 2025-09:** the actual 1-minute splice minute. The 15-minute roll times used in that research data are
  7–14 minutes early on five rolls, which would make a held position show a fake jump of about 230–290 points.

Later quarters are detected. The splice is taken to be the largest open-minus-previous-close jump on the 8th–16th
of the expiry month, ignoring jumps across gaps of more than 10 minutes. It must be at least half the previous
roll's jump and 4 times the next-largest move. Run `replay_trader --list-rolls` to print the roll points the
current cache produces.

## Options for development

| Option | Effect |
|---|---|
| `--screenshot out.bmp` | Render off-screen and exit (`BUI_UI_SCALE=2` for Retina size). It downloads first if there's a key |
| `--show-setup` | With `--screenshot`, render the data screen instead |
| `--from YYYY-MM-DD` | Download only from that day (for testing the downloader) |
| `--data file.bin` | Load a file in the `NQ1M0001` format instead of the cache (`tools/export_bars.py`, CI's `tools/make_test_data.py`) |
| `--list-rolls` | Print the cached market's roll points and exit |

## Releases

`.github/workflows/release.yml` runs on version tags (`v*`) and on manual dispatch:

- **macOS** (macos-14): universal `.app`, ad-hoc signed.
- **Windows** (MSYS2 MinGW-w64): a static `.exe`.

Each build is smoke-tested by rendering a screenshot from synthetic data. The Windows job also checks that a bad
API key is rejected over WinHTTP. On a tag, the zips are attached to a GitHub release.

To build the same zips locally, run `packaging/package.sh` (Windows via `brew install mingw-w64`).

The builds aren't notarized or Authenticode-signed. Users get the one-time Gatekeeper or SmartScreen prompt
described in the README.

The local developer build loads a chart font from `../strategy_report/pdf_src/fonts/` if it exists. That font
isn't redistributable, so it is never in an `RT_DIST` build. Users can drop any `chart_font.ttf` into the data
folder; otherwise the chart uses the UI font.
