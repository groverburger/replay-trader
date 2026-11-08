// Glue between the BUI widget tree and the app state.
#pragma once

struct BKNode;

// Something changed: schedule a frame (main.cpp), which syncs the widgets and redraws the chart.
void poke();

// The left-hand column of panels (panels.cpp).
BKNode* panels_new();
// Brings every widget up to date with the app state (labels, values, dynamic rows).
void panels_sync();
// BKHost.emit for panel widgets; returns true when the node belonged to the panels.
bool panels_event(BKNode* node, int kind);
