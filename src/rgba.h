#pragma once

struct Rgba {
    float r, g, b, a;
    Rgba alpha(float x) const { return {r, g, b, x}; }
};
