// The price chart (with oscillator panes) and the equity curve: BUI "custom" nodes that draw themselves into the
// kit's raster and handle their own pointer input.
#pragma once

struct BKNode;

// Registers the chart font (a TrueType file); the UI font is used when it is missing.
void chart_load_font(const char* path);

BKNode* chart_new();
// Recomputes indicators and axis ranges for the current replay state; call before the chart is rendered.
void chart_prepare();

BKNode* equity_new();
