// The price chart (with oscillator panes) and the equity curve: BUI "custom" nodes that draw themselves into the
// kit's raster and handle their own pointer input.
#pragma once

struct BKNode;

// Registers the chart font: chart_font.ttf in the data folder, else the build machine's RT_FONT if there is one;
// without either the chart uses the UI font (Inter).
void chart_load_font();

BKNode* chart_new();
// Recomputes indicators and axis ranges for the current replay state; call before the chart is rendered.
void chart_prepare();
// Advances the zoom and axis easing by dt seconds (before chart_prepare); true while still animating.
bool chart_animate(double dt);
bool chart_animating();  // after chart_prepare: whether another frame is needed

BKNode* equity_new();
