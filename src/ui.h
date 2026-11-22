// Glue between the BUI widget tree and the app state.
#pragma once

#include <string>

struct BKNode;
class Downloader;

// Something changed: schedule a frame (main.cpp), which syncs the widgets and redraws the chart.
void poke();

// The left-hand column of panels (panels.cpp).
BKNode* panels_new();
// Brings every widget up to date with the app state (labels, values, dynamic rows).
void panels_sync();
// BKHost.emit for panel widgets; returns true when the node belonged to the panels.
bool panels_event(BKNode* node, int kind);

// The data screen shown before the trader (setup.cpp): API key, download progress, cached-data fallback.
BKNode* setup_new(const std::string& key, const std::string& key_source);
void setup_sync();
bool setup_event(BKNode* node, int kind);
void setup_mark_started();  // a download was started from main (key already known)
void setup_free();
void setup_error(const std::string& text);

// main.cpp
Downloader& downloader();
void enter_trader();  // load the cached market and replace the data screen with the trader
