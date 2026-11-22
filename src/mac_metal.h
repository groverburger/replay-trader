// macOS: the Metal renderer (src/mac_metal.mm).
#pragma once

struct GLFWwindow;

bool metal_init(GLFWwindow* win);  // false: no Metal device (the OpenGL renderer is used instead)
void metal_shutdown();
void metal_rebuild_fonts();
// Takes the next drawable (waiting while all are queued: the vsync pacing) and starts the backend's frame.
// Returns the ms spent waiting.
double metal_new_frame(int fb_w, int fb_h);
// Renders ImGui's draw data into that drawable and presents it.
void metal_render(float r, float g, float b);
