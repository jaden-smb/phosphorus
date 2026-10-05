// tools/phxstudio/pixeldoc.h — the sprite/pixel editor's DOCUMENT models, headless and unit-
// tested in the editors suite:
//
//   PixelDoc  an RGBA8 image (a sprite sheet, a tileset, a portrait) with the pixel-art tools —
//             pencil lines with brush size, rect/ellipse (outline or filled), contiguous and
//             global fill, colour replace, region copy/paste, flips, rotation, wrap-shift (for
//             seamless tiles), canvas resize with an anchor, bounded snapshot undo — plus the GBA
//             checks the bake cares about (distinct colours per 8x8 tile, BGR555 snapping).
//             Loads through the pipeline's own PNG decoder, saves through png_write.h.
//   SprDoc    a sprite definition (.sprdef text or the JSON sidecar): sheet path, frame size and
//             NAMED clips (the bake hashes names on load, so the editor keeps its own parser and
//             the suite proves builders.h still accepts everything this saves).
//
// Host-only (STL fine).
#ifndef PHX_TOOLS_PHXSTUDIO_PIXELDOC_H
#define PHX_TOOLS_PHXSTUDIO_PIXELDOC_H

#include "png.h"         // tools/phxpack — the one PNG decoder
#include "json.h"        // tools/phxpack — the one JSON parser
#include "abspath.h"
#include "png_write.h"   // tools/common

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

namespace phxstudio {

// phx RGBA: R | G<<8 | B<<16 | A<<24
inline uint32_t px_rgba(int r, int g, int b, int a = 255) {
    return uint32_t(r & 255) | uint32_t(g & 255) << 8 | uint32_t(b & 255) << 16 | uint32_t(a & 255) << 24;
}
inline int px_r(uint32_t c) { return int(c & 255); }
inline int px_g(uint32_t c) { return int((c >> 8) & 255); }
inline int px_b(uint32_t c) { return int((c >> 16) & 255); }
inline int px_a(uint32_t c) { return int(c >> 24); }
// Snap to what a GBA BG/OBJ palette entry can hold (5 bits per channel), expanded back to 8
// bits the way the hardware's DAC does (v5 << 3 | v5 >> 2) so snapped colours are stable.
inline uint32_t snap555(uint32_t c) {
    if (px_a(c) == 0) return 0;
    auto s = [](int v) { const int v5 = v >> 3; return (v5 << 3) | (v5 >> 2); };
    return px_rgba(s(px_r(c)), s(px_g(c)), s(px_b(c)), 255);
}
inline std::string hex_rgb(uint32_t c) {
    char b[16];
    std::snprintf(b, sizeof(b), "%02X%02X%02X", px_r(c), px_g(c), px_b(c));
    return b;
}
inline bool parse_hex_rgb(const std::string& s0, uint32_t& out) {
    std::string s = s0;
    if (!s.empty() && s[0] == '#') s = s.substr(1);
    if (s.size() != 6 && s.size() != 8) return false;
    char* end = nullptr;
    const unsigned long v = std::strtoul(s.c_str(), &end, 16);
    if (!end || *end) return false;
    if (s.size() == 6) out = px_rgba(int(v >> 16) & 255, int(v >> 8) & 255, int(v) & 255, 255);
    else out = px_rgba(int(v >> 24) & 255, int(v >> 16) & 255, int(v >> 8) & 255, int(v) & 255);
    return true;
}

// HSV (h 0..359, s/v 0..255) <-> RGB, integer only.
inline uint32_t hsv_to_rgb(int h, int s, int v) {
    h = ((h % 360) + 360) % 360;
    const int region = h / 60, rem = (h % 60) * 255 / 60;
    const int p = v * (255 - s) / 255, q = v * (255 - s * rem / 255) / 255, t = v * (255 - s * (255 - rem) / 255) / 255;
    switch (region) {
    case 0: return px_rgba(v, t, p);  case 1: return px_rgba(q, v, p);  case 2: return px_rgba(p, v, t);
    case 3: return px_rgba(p, q, v);  case 4: return px_rgba(t, p, v);  default: return px_rgba(v, p, q);
    }
}
inline void rgb_to_hsv(uint32_t c, int& h, int& s, int& v) {
    const int r = px_r(c), g = px_g(c), b = px_b(c);
    const int mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b)), d = mx - mn;
    v = mx;
    s = mx ? d * 255 / mx : 0;
    if (!d) { h = 0; return; }
    if (mx == r)      h = (60 * (g - b) / d + 360) % 360;
    else if (mx == g) h = 60 * (b - r) / d + 120;
    else              h = 60 * (r - g) / d + 240;
}

// A few palettes to start from (each is a classic pixel-art set; DB16 first).
struct NamedPalette { const char* name; std::vector<uint32_t> colors; };
inline const std::vector<NamedPalette>& builtin_palettes() {
    static const std::vector<NamedPalette> k = [] {
        auto hex = [](std::initializer_list<uint32_t> v) {
            std::vector<uint32_t> o;
            for (uint32_t x : v) o.push_back(px_rgba(int(x >> 16) & 255, int(x >> 8) & 255, int(x) & 255));
            return o;
        };
        std::vector<NamedPalette> p;
        p.push_back({ "DB16", hex({ 0x140c1c, 0x442434, 0x30346d, 0x4e4a4e, 0x854c30, 0x346524, 0xd04648, 0x757161,
                                    0x597dce, 0xd27d2c, 0x8595a1, 0x6daa2c, 0xd2aa99, 0x6dc2ca, 0xdad45e, 0xdeeed6 }) });
        p.push_back({ "PICO-8", hex({ 0x000000, 0x1d2b53, 0x7e2553, 0x008751, 0xab5236, 0x5f574f, 0xc2c3c7, 0xfff1e8,
                                      0xff004d, 0xffa300, 0xffec27, 0x00e436, 0x29adff, 0x83769c, 0xff77a8, 0xffccaa }) });
        p.push_back({ "Game Boy", hex({ 0x0f380f, 0x306230, 0x8bac0f, 0x9bbc0f }) });
        p.push_back({ "Ember", hex({ 0x1a1020, 0x3a1c2c, 0x6a2430, 0xa8342c, 0xe0582c, 0xff8a30, 0xffc050, 0xfff0b0,
                                     0x203048, 0x305878, 0x4890b0, 0x80d0e0, 0x284028, 0x487838, 0x88b048, 0xf0f0e8 }) });
        return p;
    }();
    return k;
}

// ============================================================================================
// PixelDoc
// ============================================================================================
class PixelDoc {
public:
    int w = 0, h = 0;
    std::vector<uint32_t> px;
    bool dirty = false;

    static PixelDoc blank(int w, int h, uint32_t fill = 0) {
        PixelDoc d;
        d.w = std::max(1, w); d.h = std::max(1, h);
        d.px.assign(size_t(d.w) * size_t(d.h), fill);
        return d;
    }
    static bool load_png(const std::string& path, PixelDoc& out, std::string* err = nullptr) {
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) { if (err) *err = "cannot open " + path; return false; }
        std::vector<uint8_t> bytes;
        uint8_t buf[65536];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) bytes.insert(bytes.end(), buf, buf + n);
        std::fclose(f);
        std::vector<uint32_t> rgba;
        uint16_t iw = 0, ih = 0;
        if (!phxtool::png_decode(bytes.data(), bytes.size(), rgba, iw, ih)) {
            if (err) *err = "not a PNG the pipeline can decode (8-bit gray/RGB/palette/RGBA): " + path;
            return false;
        }
        out.w = iw; out.h = ih; out.px = std::move(rgba);
        out.dirty = false;
        out.undo_.clear(); out.redo_.clear();
        return true;
    }
    bool save_png(const std::string& path, std::string* err = nullptr) {
        if (!phxtool::png_write_file(path, px.data(), w, h, err)) return false;
        dirty = false;
        return true;
    }

    bool in(int x, int y) const { return x >= 0 && y >= 0 && x < w && y < h; }
    uint32_t get(int x, int y) const { return in(x, y) ? px[size_t(y) * size_t(w) + size_t(x)] : 0u; }
    void set(int x, int y, uint32_t c) {
        if (!in(x, y)) return;
        uint32_t& p = px[size_t(y) * size_t(w) + size_t(x)];
        if (p != c) { p = c; dirty = true; }
    }
    // A square brush of `size` centred on (x, y) (size 1 = one pixel; even sizes lean up-left).
    void dab(int x, int y, uint32_t c, int size = 1) {
        const int lo = -((size - 1) / 2), hi = lo + size - 1;
        for (int dy = lo; dy <= hi; ++dy)
            for (int dx = lo; dx <= hi; ++dx) set(x + dx, y + dy, c);
    }
    // Bresenham line of dabs (pencil strokes connect successive mouse samples).
    void line(int x0, int y0, int x1, int y1, uint32_t c, int size = 1) {
        const int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0);
        const int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
        int err = dx + dy;
        for (;;) {
            dab(x0, y0, c, size);
            if (x0 == x1 && y0 == y1) break;
            const int e2 = 2 * err;
            if (e2 >= dy) { err += dy; x0 += sx; }
            if (e2 <= dx) { err += dx; y0 += sy; }
        }
    }
    void rect(int x0, int y0, int x1, int y1, uint32_t c, bool filled, int size = 1) {
        if (x0 > x1) std::swap(x0, x1);
        if (y0 > y1) std::swap(y0, y1);
        if (filled) { for (int y = y0; y <= y1; ++y) for (int x = x0; x <= x1; ++x) set(x, y, c); return; }
        line(x0, y0, x1, y0, c, size); line(x0, y1, x1, y1, c, size);
        line(x0, y0, x0, y1, c, size); line(x1, y0, x1, y1, c, size);
    }
    // Ellipse inscribed in the rect (midpoint algorithm on the doubled grid so even sizes work).
    void ellipse(int x0, int y0, int x1, int y1, uint32_t c, bool filled) {
        if (x0 > x1) std::swap(x0, x1);
        if (y0 > y1) std::swap(y0, y1);
        const int64_t a2 = int64_t(x1 - x0), b2 = int64_t(y1 - y0);   // doubled radii
        if (a2 == 0 || b2 == 0) { rect(x0, y0, x1, y1, c, true); return; }
        const int64_t cx2 = x0 + x1, cy2 = y0 + y1;                    // doubled centre
        for (int y = y0; y <= y1; ++y) {
            // x extent at this row: ((2x - cx2)/a2)^2 + ((2y - cy2)/b2)^2 <= 1
            const int64_t dy2 = 2 * y - cy2;
            const int64_t num = b2 * b2 - dy2 * dy2;                   // a2^2 * that / b2^2 >= (2x-cx2)^2
            if (num < 0) continue;
            // half-width in doubled units: a2 * sqrt(num) / b2
            int64_t lo = 0, hi = a2;
            while (lo < hi) { const int64_t m = (lo + hi + 1) / 2; if (m * m * b2 * b2 <= a2 * a2 * num) lo = m; else hi = m - 1; }
            const int xa = int((cx2 - lo + 1) / 2), xb = int((cx2 + lo) / 2);
            if (filled) { for (int x = xa; x <= xb; ++x) set(x, y, c); }
            else { set(xa, y, c); set(xb, y, c); }
        }
        if (!filled) {   // columns pass closes the gaps on steep parts of the outline
            for (int x = x0; x <= x1; ++x) {
                const int64_t dx2 = 2 * x - cx2;
                const int64_t num = a2 * a2 - dx2 * dx2;
                if (num < 0) continue;
                int64_t lo = 0, hi = b2;
                while (lo < hi) { const int64_t m = (lo + hi + 1) / 2; if (m * m * a2 * a2 <= b2 * b2 * num) lo = m; else hi = m - 1; }
                set(x, int((cy2 - lo + 1) / 2), c);
                set(x, int((cy2 + lo) / 2), c);
            }
        }
    }
    // Fill: contiguous (4-connected) region of the clicked colour, or every pixel of that colour.
    int fill(int x, int y, uint32_t c, bool contiguous = true) {
        if (!in(x, y)) return 0;
        const uint32_t from = get(x, y);
        if (from == c) return 0;
        int changed = 0;
        if (!contiguous) {
            for (uint32_t& p : px) if (p == from) { p = c; ++changed; }
        } else {
            std::vector<std::pair<int, int>> st{ { x, y } };
            while (!st.empty()) {
                const auto [cx, cy] = st.back();
                st.pop_back();
                if (!in(cx, cy) || get(cx, cy) != from) continue;
                px[size_t(cy) * size_t(w) + size_t(cx)] = c;
                ++changed;
                st.push_back({ cx + 1, cy }); st.push_back({ cx - 1, cy });
                st.push_back({ cx, cy + 1 }); st.push_back({ cx, cy - 1 });
            }
        }
        if (changed) dirty = true;
        return changed;
    }
    int replace_color(uint32_t from, uint32_t to) {
        int k = 0;
        for (uint32_t& p : px) if (p == from && p != to) { p = to; ++k; }
        if (k) dirty = true;
        return k;
    }

    // ---- regions (x, y, rw, rh clamped to the image) ----
    struct Region { int x = 0, y = 0, w = 0, h = 0; std::vector<uint32_t> px; };
    Region copy(int x, int y, int rw, int rh) const {
        Region r;
        clamp_rect(x, y, rw, rh);
        r.x = x; r.y = y; r.w = rw; r.h = rh;
        r.px.resize(size_t(std::max(0, rw)) * size_t(std::max(0, rh)));
        for (int j = 0; j < rh; ++j) for (int i = 0; i < rw; ++i) r.px[size_t(j) * size_t(rw) + size_t(i)] = get(x + i, y + j);
        return r;
    }
    // Paste at (x, y); transparent source pixels are skipped when `skip_clear`.
    void paste(const Region& r, int x, int y, bool skip_clear) {
        for (int j = 0; j < r.h; ++j)
            for (int i = 0; i < r.w; ++i) {
                const uint32_t c = r.px[size_t(j) * size_t(r.w) + size_t(i)];
                if (skip_clear && px_a(c) == 0) continue;
                set(x + i, y + j, c);
            }
    }
    void clear_rect(int x, int y, int rw, int rh, uint32_t c = 0) {
        clamp_rect(x, y, rw, rh);
        for (int j = 0; j < rh; ++j) for (int i = 0; i < rw; ++i) set(x + i, y + j, c);
    }
    void flip_h(int x, int y, int rw, int rh) {
        clamp_rect(x, y, rw, rh);
        for (int j = 0; j < rh; ++j) for (int i = 0; i < rw / 2; ++i) swap_px(x + i, y + j, x + rw - 1 - i, y + j);
    }
    void flip_v(int x, int y, int rw, int rh) {
        clamp_rect(x, y, rw, rh);
        for (int j = 0; j < rh / 2; ++j) for (int i = 0; i < rw; ++i) swap_px(x + i, y + j, x + i, y + rh - 1 - j);
    }
    // Rotate a SQUARE region 90° clockwise (cw) or counter-clockwise in place.
    bool rotate90(int x, int y, int n, bool cw) {
        int rw = n, rh = n;
        clamp_rect(x, y, rw, rh);
        if (rw != n || rh != n || n <= 0) return false;
        const Region r = copy(x, y, n, n);
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i) {
                const uint32_t c = r.px[size_t(j) * size_t(n) + size_t(i)];
                if (cw) set(x + n - 1 - j, y + i, c); else set(x + j, y + n - 1 - i, c);
            }
        return true;
    }
    // Wrap-shift a region by (dx, dy): pixels leaving one edge re-enter the other (tile seams).
    void shift(int x, int y, int rw, int rh, int dx, int dy) {
        clamp_rect(x, y, rw, rh);
        if (rw <= 0 || rh <= 0) return;
        const Region r = copy(x, y, rw, rh);
        for (int j = 0; j < rh; ++j)
            for (int i = 0; i < rw; ++i)
                set(x + (((i + dx) % rw) + rw) % rw, y + (((j + dy) % rh) + rh) % rh, r.px[size_t(j) * size_t(rw) + size_t(i)]);
    }
    // New canvas size; `ax`/`ay` in {0,1,2} anchor the old pixels left/centre/right, top/mid/bottom.
    void resize(int nw, int nh, int ax = 0, int ay = 0) {
        nw = std::max(1, std::min(nw, 4096)); nh = std::max(1, std::min(nh, 4096));
        std::vector<uint32_t> n(size_t(nw) * size_t(nh), 0);
        const int ox = ax == 0 ? 0 : ax == 1 ? (nw - w) / 2 : nw - w;
        const int oy = ay == 0 ? 0 : ay == 1 ? (nh - h) / 2 : nh - h;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const int tx = x + ox, ty = y + oy;
                if (tx >= 0 && ty >= 0 && tx < nw && ty < nh) n[size_t(ty) * size_t(nw) + size_t(tx)] = get(x, y);
            }
        w = nw; h = nh; px.swap(n);
        dirty = true;
    }
    // Integer upscale by k (nearest) — for turning a 16px sketch into a 32px base, etc.
    void scale_up(int k) {
        if (k <= 1) return;
        std::vector<uint32_t> n(size_t(w * k) * size_t(h * k));
        for (int y = 0; y < h * k; ++y) for (int x = 0; x < w * k; ++x) n[size_t(y) * size_t(w * k) + size_t(x)] = get(x / k, y / k);
        w *= k; h *= k; px.swap(n);
        dirty = true;
    }
    void snap_all_555() {
        for (uint32_t& p : px) { const uint32_t s = snap555(p); if (s != p) { p = s; dirty = true; } }
    }

    // ---- analysis ----
    std::vector<uint32_t> unique_colors(size_t limit = 4096) const {
        std::set<uint32_t> s;
        std::vector<uint32_t> out;
        for (uint32_t p : px) {
            if (px_a(p) == 0) continue;
            if (s.insert(p).second) { out.push_back(p); if (out.size() >= limit) break; }
        }
        return out;
    }
    // Distinct OPAQUE colours in the tile at (tx, ty) of size t (the GBA 4bpp budget is 15).
    int tile_colors(int tx, int ty, int t = 8) const {
        std::set<uint32_t> s;
        for (int y = ty * t; y < (ty + 1) * t; ++y)
            for (int x = tx * t; x < (tx + 1) * t; ++x) { const uint32_t p = get(x, y); if (px_a(p)) s.insert(snap555(p)); }
        return int(s.size());
    }
    int tiles_over_budget(int t = 8, int budget = 15) const {
        int k = 0;
        for (int ty = 0; ty < (h + t - 1) / t; ++ty)
            for (int tx = 0; tx < (w + t - 1) / t; ++tx) if (tile_colors(tx, ty, t) > budget) ++k;
        return k;
    }

    // ---- undo (whole-image snapshots; images are small) ----
    void push_undo() {
        undo_.push_back(Snap{ w, h, px });
        size_t bytes = 0;
        for (const Snap& s : undo_) bytes += s.px.size() * 4;
        while (undo_.size() > 1 && (undo_.size() > kMaxUndo || bytes > kMaxUndoBytes)) {
            bytes -= undo_.front().px.size() * 4;
            undo_.erase(undo_.begin());
        }
        redo_.clear();
    }
    void drop_undo() { if (!undo_.empty()) undo_.pop_back(); }
    bool undo() {
        if (undo_.empty()) return false;
        redo_.push_back(Snap{ w, h, px });
        restore(undo_.back());
        undo_.pop_back();
        dirty = true;
        return true;
    }
    bool redo() {
        if (redo_.empty()) return false;
        undo_.push_back(Snap{ w, h, px });
        restore(redo_.back());
        redo_.pop_back();
        dirty = true;
        return true;
    }
    size_t undo_depth() const { return undo_.size(); }
    size_t redo_depth() const { return redo_.size(); }
    // True when the image differs from the most recent undo snapshot (a stroke that changed nothing
    // can then drop its snapshot).
    bool same_as_last_snapshot() const { return !undo_.empty() && undo_.back().w == w && undo_.back().h == h && undo_.back().px == px; }

private:
    struct Snap { int w, h; std::vector<uint32_t> px; };
    static constexpr size_t kMaxUndo = 100;
    static constexpr size_t kMaxUndoBytes = 64u << 20;
    void restore(const Snap& s) { w = s.w; h = s.h; px = s.px; }
    void swap_px(int x0, int y0, int x1, int y1) {
        const uint32_t a = get(x0, y0), b = get(x1, y1);
        set(x0, y0, b); set(x1, y1, a);
    }
    void clamp_rect(int& x, int& y, int& rw, int& rh) const {
        if (x < 0) { rw += x; x = 0; }
        if (y < 0) { rh += y; y = 0; }
        rw = std::max(0, std::min(rw, w - x));
        rh = std::max(0, std::min(rh, h - y));
    }
    std::vector<Snap> undo_, redo_;
};

// ============================================================================================
// SprDoc — a sprite definition with named clips
// ============================================================================================
struct SprClip {
    std::string name;
    int first = 0, count = 1, fps = 8;
    bool loop = true;
};

// A transition of the sprite's animation state machine (a `trans` line): in clip `from` ("*" = any
// clip), the event `trigger` switches to clip `to`. The engine sends the stock events (stock_triggers)
// from PlatformerController and the anim system; a game sends its own (phx::anim_trigger).
struct SprEdge {
    std::string from, to, trigger;
    bool operator==(const SprEdge& o) const { return from == o.from && to == o.to && trigger == o.trigger; }
};

class SprDoc {
public:
    std::string sheet;           // as written in the file (bare file name = next to the def)
    int frame_w = 16, frame_h = 16;
    std::vector<SprClip> clips;
    std::vector<SprEdge> trans;  // the state machine (clip names; resolved at bake time)
    bool json = false;           // the file's format (.json sidecar vs .sprdef)
    bool dirty = false;

    static bool is_sprite_json(const std::string& text) {
        return text.find("\"animations\"") != std::string::npos && text.find("\"image\"") != std::string::npos;
    }

    static bool parse(const std::string& text, bool as_json, SprDoc& out, std::string* err = nullptr) {
        auto fail = [&](const std::string& why) { if (err) *err = why; return false; };
        out = SprDoc{};
        out.json = as_json;
        if (as_json) {
            phxtool::JsonValue root;
            std::string jerr;
            if (!phxtool::JsonParser::parse(text, root, &jerr)) return fail("malformed JSON: " + jerr);
            if (!root.is_obj()) return fail("top level is not a JSON object");
            out.sheet = root.str_at("image");
            if (const phxtool::JsonValue* t = root.find("tile")) out.frame_w = out.frame_h = t->as_int();
            out.frame_w = root.int_at("frame_w", out.frame_w);
            out.frame_h = root.int_at("frame_h", out.frame_h);
            if (const phxtool::JsonValue* a = root.find("animations"); a && a->is_obj())
                for (const auto& kv : a->members) {
                    SprClip c;
                    c.name = kv.first;
                    const phxtool::JsonValue* fr = kv.second.find("frames");
                    if (fr && fr->is_arr() && !fr->arr.empty()) { c.first = fr->arr.front().as_int(); c.count = int(fr->arr.size()); }
                    c.fps = kv.second.int_at("fps", 0);
                    c.loop = kv.second.find("loop") && kv.second.find("loop")->boolean;
                    out.clips.push_back(c);
                }
            if (const phxtool::JsonValue* tr = root.find("transitions"); tr && tr->is_arr())
                for (const phxtool::JsonValue& t : tr->arr) {
                    const std::string from = t.str_at("from");
                    out.trans.push_back(SprEdge{ from.empty() ? std::string("*") : from, t.str_at("to"), t.str_at("on") });
                }
        } else {
            size_t p = 0;
            while (p < text.size()) {
                size_t e = text.find('\n', p);
                if (e == std::string::npos) e = text.size();
                std::string line = text.substr(p, e - p);
                p = e + 1;
                if (!line.empty() && line.back() == '\r') line.pop_back();
                const size_t hash = line.find('#');
                if (hash != std::string::npos) line.resize(hash);
                char a[256], b[256], c[256];
                int v1, v2, v3, v4;
                if (std::sscanf(line.c_str(), " sheet %255s %d %d", a, &v1, &v2) == 3) { out.sheet = a; out.frame_w = v1; out.frame_h = v2; }
                else if (std::sscanf(line.c_str(), " clip %255s %d %d %d %d", b, &v1, &v2, &v3, &v4) == 5)
                    out.clips.push_back(SprClip{ b, v1, v2, v3, v4 != 0 });
                else if (std::sscanf(line.c_str(), " trans %255s %255s %255s", a, b, c) == 3)
                    out.trans.push_back(SprEdge{ a, b, c });
            }
        }
        if (out.sheet.empty()) return fail(as_json ? "missing \"image\" (the sheet PNG)" : "missing 'sheet <png> <w> <h>' line");
        if (out.frame_w <= 0 || out.frame_h <= 0) return fail("frame size must be > 0");
        return true;
    }
    static bool load(const std::string& path, SprDoc& out, std::string* err = nullptr) {
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) { if (err) *err = "cannot open " + path; return false; }
        std::string t;
        char buf[8192];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) t.append(buf, n);
        std::fclose(f);
        const bool js = path.size() > 5 && path.compare(path.size() - 5, 5, ".json") == 0;
        return parse(t, js, out, err);
    }

    std::string save_text() const {
        std::string o;
        if (json) {
            o = "{ \"image\": \"" + sheet + "\", \"frame_w\": " + std::to_string(frame_w) +
                ", \"frame_h\": " + std::to_string(frame_h) + ",\n  \"animations\": {";
            for (size_t i = 0; i < clips.size(); ++i) {
                const SprClip& c = clips[i];
                o += i ? ",\n    " : "\n    ";
                o += "\"" + c.name + "\": { \"frames\": [";
                for (int k = 0; k < c.count; ++k) { if (k) o += ","; o += std::to_string(c.first + k); }
                o += "], \"fps\": " + std::to_string(c.fps) + ", \"loop\": " + (c.loop ? "true" : "false") + " }";
            }
            o += clips.empty() ? "}" : "\n  }";
            if (!trans.empty()) {
                o += ",\n  \"transitions\": [";
                for (size_t i = 0; i < trans.size(); ++i) {
                    const SprEdge& t = trans[i];
                    o += i ? ",\n    " : "\n    ";
                    o += "{ \"from\": \"" + t.from + "\", \"to\": \"" + t.to + "\", \"on\": \"" + t.trigger + "\" }";
                }
                o += "\n  ]";
            }
            o += "\n}\n";
        } else {
            o = "# sprite definition (phxsprite) - edited in Phosphorus Studio\n";
            o += "sheet " + sheet + " " + std::to_string(frame_w) + " " + std::to_string(frame_h) + "\n";
            for (const SprClip& c : clips)
                o += "clip " + c.name + " " + std::to_string(c.first) + " " + std::to_string(c.count) + " " +
                     std::to_string(c.fps) + " " + (c.loop ? "1" : "0") + "\n";
            for (const SprEdge& t : trans) o += "trans " + t.from + " " + t.to + " " + t.trigger + "\n";
        }
        return o;
    }
    bool save(const std::string& path, std::string* err = nullptr) {
        const std::string t = save_text();
        FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) { if (err) *err = "cannot write " + path; return false; }
        const bool ok = std::fwrite(t.data(), 1, t.size(), f) == t.size();
        std::fclose(f);
        if (ok) dirty = false;
        else if (err) *err = "short write to " + path;
        return ok;
    }

    // The sheet's path on disk, resolved the way the bake resolves it (a bare name sits next to
    // the def; anything with a '/' is relative to the working directory).
    static std::string resolve_sheet(const std::string& def_path, const std::string& sheet) {
        if (sheet.empty() || is_abs_path(sheet) || sheet.find('/') != std::string::npos) return sheet;
        const size_t sl = def_path.find_last_of('/');
        return sl == std::string::npos ? sheet : def_path.substr(0, sl + 1) + sheet;
    }

    // Frame grid helpers over a sheet of sw x sh pixels.
    int cols(int sw) const { return frame_w > 0 ? std::max(0, sw / frame_w) : 0; }
    int rows(int sh) const { return frame_h > 0 ? std::max(0, sh / frame_h) : 0; }
    int frame_count(int sw, int sh) const { return cols(sw) * rows(sh); }
    void frame_xy(int i, int sw, int& x, int& y) const {
        const int c = std::max(1, cols(sw));
        x = (i % c) * frame_w; y = (i / c) * frame_h;
    }
    int find_clip(const std::string& n) const {
        for (size_t i = 0; i < clips.size(); ++i) if (clips[i].name == n) return int(i);
        return -1;
    }
    // Rename a clip, and every transition that names it.
    void rename_clip(size_t i, const std::string& to) {
        if (i >= clips.size()) return;
        const std::string from = clips[i].name;
        clips[i].name = to;
        for (SprEdge& t : trans) { if (t.from == from) t.from = to; if (t.to == from) t.to = to; }
    }
    // Remove a clip, and the transitions into or out of it.
    void remove_clip(size_t i) {
        if (i >= clips.size()) return;
        const std::string n = clips[i].name;
        clips.erase(clips.begin() + std::ptrdiff_t(i));
        trans.erase(std::remove_if(trans.begin(), trans.end(), [&](const SprEdge& t) { return t.from == n || t.to == n; }),
                    trans.end());
    }
    // The events the engine sends by itself (behaviours.h): what most transitions are keyed on.
    static const std::vector<std::string>& stock_triggers() {
        static const std::vector<std::string> k = { "jump", "fall", "land", "move", "stop", "hurt", "done" };
        return k;
    }
    // A new transition out of clip `from`: to the next clip, on "done" when `from` does not loop.
    SprEdge suggest_edge(int from) const {
        SprEdge e{ "*", clips.empty() ? std::string() : clips.front().name, "jump" };
        if (from >= 0 && from < int(clips.size())) {
            e.from = clips[size_t(from)].name;
            e.to = clips.size() > 1 ? clips[size_t((from + 1) % int(clips.size()))].name : e.from;
            e.trigger = clips[size_t(from)].loop ? "move" : "done";
        }
        return e;
    }
    // A clip name that is not taken yet ("clip", "clip2", ...).
    std::string fresh_name(const std::string& base) const {
        if (find_clip(base) < 0) return base;
        for (int k = 2;; ++k) { const std::string n = base + std::to_string(k); if (find_clip(n) < 0) return n; }
    }
    // Problems the bake (or the runtime) would hit, as human-readable lines.
    std::vector<std::string> validate(int sw, int sh) const {
        std::vector<std::string> out;
        if (sw % std::max(1, frame_w) || sh % std::max(1, frame_h))
            out.push_back("sheet " + std::to_string(sw) + "x" + std::to_string(sh) + " is not a whole number of " +
                          std::to_string(frame_w) + "x" + std::to_string(frame_h) + " frames");
        const int n = frame_count(sw, sh);
        for (const SprClip& c : clips) {
            if (c.name.empty()) out.push_back("a clip has no name");
            if (c.count <= 0) out.push_back("clip '" + c.name + "' has no frames");
            if (c.first < 0 || c.first + c.count > n) out.push_back("clip '" + c.name + "' runs past the last frame (" + std::to_string(n) + ")");
            if (c.fps <= 0 && c.count > 1) out.push_back("clip '" + c.name + "' has fps 0 (it will never advance)");
            if (c.name.find_first_of(" \t#\"") != std::string::npos) out.push_back("clip '" + c.name + "' has spaces/quotes in its name");
        }
        for (const SprEdge& t : trans) {
            const std::string what = "transition " + t.from + " -> " + t.to;
            if (t.from != "*" && find_clip(t.from) < 0) out.push_back(what + ": no clip '" + t.from + "'");
            if (find_clip(t.to) < 0) out.push_back(what + ": no clip '" + t.to + "'");
            if (t.trigger.empty()) out.push_back(what + " has no trigger");
            else if (t.trigger.find_first_of(" \t#\"") != std::string::npos) out.push_back(what + ": the trigger has spaces/quotes");
        }
        return out;
    }
};

} // namespace phxstudio
#endif // PHX_TOOLS_PHXSTUDIO_PIXELDOC_H
