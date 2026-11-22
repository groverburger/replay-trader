// A small CPU rasterizer for ImGui draw data: --screenshot renders off-screen with it, so CI runners and
// headless machines need no window, GPU or OpenGL driver.
#pragma once

#include <cstdint>
#include <vector>

struct ImDrawData;

struct SoftTexture {
    int w = 0, h = 0;
    const unsigned char* rgba = nullptr;  // the font atlas (the only texture the app uses)
};

// Renders `dd` into a w x h image of 0x00RRGGBB pixels (top row first), over `clear`.
void soft_render(const ImDrawData* dd, const SoftTexture& tex, int w, int h, uint32_t clear, std::vector<uint32_t>& out);
