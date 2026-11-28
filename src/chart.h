// The price chart (with oscillator panes) and the equity curves, drawn with ImGui draw lists. The chart handles
// its own pointer input: pan, zoom, crosshair, Cmd/Ctrl+click limits, Shift+click stops, dragging order lines.
#pragma once

struct ImFont;

// The fonts the chart draws its labels with (chart_font.ttf, the developer's RT_FONT, or the UI font) at the
// axis size (11 pt) and the title size; set after the font atlas is built.
void chart_set_fonts(ImFont* text, ImFont* title);

// Recomputes indicators and axis ranges for the current replay state; call once per frame before drawing.
void chart_prepare();
// Advances the zoom and pan easing by dt seconds (before chart_prepare); true while still animating.
bool chart_animate(double dt);
bool chart_animating();  // after chart_prepare: whether another frame is needed
// Indicators recompute on a worker thread while the chart shows their previous result (the interactive window).
// Off (the default, for screenshots and tools): every recompute runs inside chart_prepare.
void chart_set_async(bool on);
// Leaves follow mode and frames chart buckets x0..x1 (current timeframe) with room around them.
void chart_focus(double x0, double x1);
void chart_zoom_out(double factor);  // widens the view around its centre (developer screenshots)

// The chart at the current cursor position, w x h interface units.
void chart_draw(float w, float h);
// Equity curves (full available width): the replay's closed trades, and the last backtest (g.bt_result).
void equity_draw(float h);
void backtest_curve_draw(float h);
