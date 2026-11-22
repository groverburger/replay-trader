// Glue between the Dear ImGui interface and the app state. Every frame main.cpp builds the whole interface
// again (immediate mode): the data screen (setup.cpp), or the sidebar (panels.cpp) and the chart (chart.cpp).
#pragma once

#include <string>
#include <vector>

struct ImFont;
class Downloader;

// ---------------------------------------------------------------- main.cpp
// Something changed: schedule a frame. Safe to call from any thread.
void poke();
// Keeps frames coming (an animation is running); call during a frame.
void keep_animating();

// Wheel and trackpad input that arrived for this frame, in interface units (points). dy > 0 scrolls up (the
// content moves down), dx > 0 moves the content right. `notch` marks a mouse-wheel click (to be eased) as
// opposed to a trackpad's precise, continuous delta. Consumers set dx = dy = 0 on what they used.
struct ScrollEvent {
    float dx, dy;
    bool notch;
};
std::vector<ScrollEvent>& frame_scroll();
constexpr float kNotch = 40.0f;  // one wheel click scrolls this many points (as BUI's 2 widget units)

// Interface scale (1 on macOS, where ImGui works in points; the monitor's DPI scale on Windows) and device
// pixels per interface unit (2 on Retina). snap() rounds a coordinate to the nearest device pixel.
float ui_scale();
float px_scale();
inline float snap(float v) {
    float s = px_scale();
    return static_cast<float>(static_cast<long long>(v * s + (v >= 0 ? 0.5f : -0.5f))) / s;
}

extern ImFont* g_font_bold;  // the title font (the UI font is ImGui's default)
// The chart asks for a mouse cursor this frame (-1: leave it to ImGui).
enum { CURSOR_NONE = -1, CURSOR_ARROW, CURSOR_CROSSHAIR, CURSOR_NS, CURSOR_MOVE };
extern int g_cursor_request;

Downloader& downloader();
void enter_trader();  // load the cached market and replace the data screen with the trader

// ---------------------------------------------------------------- panels.cpp
// The left-hand column of panels, drawn into the current window.
void panels_draw(float dt);
// Scrolling with eased wheel notches and 1:1 trackpad deltas: call instead of ImGui::BeginChild for a region
// that scrolls (the trades list, the sidebar). Uses frame_scroll() when the pointer is over it.
bool begin_smooth_child(const char* id, float w, float h, int child_flags, int window_flags, float dt);
// Scrolls the sidebar to its end on the next frame (--show-strategy screenshots).
void panels_scroll_to_end();

// ---------------------------------------------------------------- setup.cpp
// The data screen shown before the trader: API key, download progress, cached-data fallback.
void setup_init(const std::string& key, const std::string& key_source);
void setup_draw();
void setup_mark_started();  // a download was started from main (key already known)
void setup_error(const std::string& text);
