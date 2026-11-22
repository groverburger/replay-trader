#include "softraster.h"

#include <algorithm>
#include <cmath>

#include "imgui.h"

namespace {

struct Px {
    float r, g, b;
};

struct Sampler {
    const SoftTexture& t;
    // Bilinear sample at normalized (u, v): r, g, b, a in 0..1.
    void at(float u, float v, float out[4]) const {
        if (!t.rgba) {
            out[0] = out[1] = out[2] = out[3] = 1;
            return;
        }
        float x = u * t.w - 0.5f, y = v * t.h - 0.5f;
        int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
        float fx = x - x0, fy = y - y0;
        auto texel = [&](int xx, int yy, int c) {
            xx = std::clamp(xx, 0, t.w - 1), yy = std::clamp(yy, 0, t.h - 1);
            return t.rgba[(static_cast<size_t>(yy) * t.w + xx) * 4 + c] * (1.0f / 255.0f);
        };
        for (int c = 0; c < 4; ++c) {
            float a = texel(x0, y0, c) + (texel(x0 + 1, y0, c) - texel(x0, y0, c)) * fx;
            float b = texel(x0, y0 + 1, c) + (texel(x0 + 1, y0 + 1, c) - texel(x0, y0 + 1, c)) * fx;
            out[c] = a + (b - a) * fy;
        }
    }
};

inline float edge(const ImVec2& a, const ImVec2& b, float px, float py) {
    return (b.x - a.x) * (py - a.y) - (b.y - a.y) * (px - a.x);
}

void triangle(std::vector<Px>& img, int w, const Sampler& tex, ImVec2 p[3], const ImDrawVert* v[3], int cx0, int cy0, int cx1, int cy1) {
    float area = edge(p[0], p[1], p[2].x, p[2].y);
    if (area == 0) return;
    if (area < 0) std::swap(p[1], p[2]), std::swap(v[1], v[2]), area = -area;
    int x0 = std::max(cx0, static_cast<int>(std::floor(std::min({p[0].x, p[1].x, p[2].x}))));
    int x1 = std::min(cx1, static_cast<int>(std::ceil(std::max({p[0].x, p[1].x, p[2].x}))));
    int y0 = std::max(cy0, static_cast<int>(std::floor(std::min({p[0].y, p[1].y, p[2].y}))));
    int y1 = std::min(cy1, static_cast<int>(std::ceil(std::max({p[0].y, p[1].y, p[2].y}))));
    if (x0 >= x1 || y0 >= y1) return;
    float col[3][4];
    for (int k = 0; k < 3; ++k) {
        ImVec4 c = ImGui::ColorConvertU32ToFloat4(v[k]->col);
        col[k][0] = c.x, col[k][1] = c.y, col[k][2] = c.z, col[k][3] = c.w;
    }
    bool solid_uv = v[0]->uv.x == v[1]->uv.x && v[0]->uv.x == v[2]->uv.x && v[0]->uv.y == v[1]->uv.y && v[0]->uv.y == v[2]->uv.y;
    bool solid_col = v[0]->col == v[1]->col && v[0]->col == v[2]->col;
    float t0[4];
    if (solid_uv) tex.at(v[0]->uv.x, v[0]->uv.y, t0);
    float inv = 1.0f / area;
    for (int y = y0; y < y1; ++y) {
        float py = y + 0.5f;
        for (int x = x0; x < x1; ++x) {
            float px = x + 0.5f;
            float w0 = edge(p[1], p[2], px, py), w1 = edge(p[2], p[0], px, py), w2 = edge(p[0], p[1], px, py);
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            w0 *= inv, w1 *= inv, w2 *= inv;
            float c[4], t[4];
            for (int k = 0; k < 4; ++k) c[k] = solid_col ? col[0][k] : col[0][k] * w0 + col[1][k] * w1 + col[2][k] * w2;
            if (solid_uv) std::copy(t0, t0 + 4, t);
            else tex.at(v[0]->uv.x * w0 + v[1]->uv.x * w1 + v[2]->uv.x * w2, v[0]->uv.y * w0 + v[1]->uv.y * w1 + v[2]->uv.y * w2, t);
            float a = c[3] * t[3];
            if (a <= 0) continue;
            Px& d = img[static_cast<size_t>(y) * w + x];
            d.r = c[0] * t[0] * a + d.r * (1 - a);
            d.g = c[1] * t[1] * a + d.g * (1 - a);
            d.b = c[2] * t[2] * a + d.b * (1 - a);
        }
    }
}

}  // namespace

void soft_render(const ImDrawData* dd, const SoftTexture& tex, int w, int h, uint32_t clear, std::vector<uint32_t>& out) {
    Px bg{((clear >> 16) & 255) / 255.0f, ((clear >> 8) & 255) / 255.0f, (clear & 255) / 255.0f};
    std::vector<Px> img(static_cast<size_t>(w) * h, bg);
    Sampler s{tex};
    ImVec2 off = dd->DisplayPos, sc = dd->FramebufferScale;
    for (const ImDrawList* dl : dd->CmdLists) {
        for (const ImDrawCmd& cmd : dl->CmdBuffer) {
            if (cmd.UserCallback) continue;
            int cx0 = std::max(0, static_cast<int>((cmd.ClipRect.x - off.x) * sc.x));
            int cy0 = std::max(0, static_cast<int>((cmd.ClipRect.y - off.y) * sc.y));
            int cx1 = std::min(w, static_cast<int>((cmd.ClipRect.z - off.x) * sc.x));
            int cy1 = std::min(h, static_cast<int>((cmd.ClipRect.w - off.y) * sc.y));
            if (cx0 >= cx1 || cy0 >= cy1) continue;
            const ImDrawIdx* idx = dl->IdxBuffer.Data + cmd.IdxOffset;
            const ImDrawVert* vtx = dl->VtxBuffer.Data + cmd.VtxOffset;
            for (unsigned k = 0; k + 2 < cmd.ElemCount; k += 3) {
                const ImDrawVert* v[3] = {&vtx[idx[k]], &vtx[idx[k + 1]], &vtx[idx[k + 2]]};
                ImVec2 p[3];
                for (int j = 0; j < 3; ++j) p[j] = ImVec2((v[j]->pos.x - off.x) * sc.x, (v[j]->pos.y - off.y) * sc.y);
                triangle(img, w, s, p, v, cx0, cy0, cx1, cy1);
            }
        }
    }
    out.resize(img.size());
    auto b = [](float x) { return static_cast<uint32_t>(std::clamp(x, 0.0f, 1.0f) * 255.0f + 0.5f); };
    for (size_t i = 0; i < img.size(); ++i) out[i] = b(img[i].r) << 16 | b(img[i].g) << 8 | b(img[i].b);
}
