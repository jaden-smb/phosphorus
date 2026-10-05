// tools/common/twk.h — the TOOL WIDGET KIT: a pointer + keyboard immediate-mode GUI for the
// engine-native editors (Phosphorus Studio, phxtmap, phxentity), drawn ONLY with phx::UI's
// rect/image primitives through the engine's own software golden renderer
// (docs/gui-editor-feasibility.md, Phase 2). It lives in tools/, never in engine/ui: the in-game
// UI stays console-sized (focus ring, no pointer, no text entry), exactly as the guardrails ask.
//
// What it adds over phx::UI:
//   - Input: one frame of pointer (L/M/R, wheel, double-click) + key events + typed text,
//     collected from the desktop seam extension (phx/platform/desktop.h) or filled by tests.
//   - Clipping without a scissor: rects intersect the clip; images trim their SOURCE rect
//     (texel-exact for integer scales, which is every zoomed canvas and every glyph).
//   - Planes: base / popup / modal / modal-popup draw on separate sprite-layer bands, and a
//     higher plane's rect (from the previous frame) blocks the pointer for widgets beneath it.
//   - Widgets: buttons, icon buttons, checkboxes, single-line text fields (caret, selection,
//     clipboard, word motion), int/float fields (type, wheel, drag-scrub), dropdowns, sliders,
//     scrollbars, splitters, tabs, menus, context menus, modal dialogs, tooltips.
//
// The soft renderer alpha-TESTS (it never blends), so "translucent" overlays are stippled with a
// checker texture — a retro look that is also exactly what the GBA can show.
//
// Everything interactive works with `ui == nullptr` (no drawing), so widget logic is unit-tested
// headlessly in the editors suite. Host-only (STL fine).
#ifndef PHX_TOOLS_TWK_H
#define PHX_TOOLS_TWK_H

#include "phx/ui/ui.h"
#include "phx/render/renderer.h"
#include "phx/input/input.h"
#include "phx/platform/desktop.h"

#include "ascii_font.h"
#include "text_raster.h"
#include "twk_geom.h"
#include "twk_icons.h"

#include <algorithm>
#include <climits>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace twk {

using phx::Rgba;
using phx::rgba;
using phx::TextureId;
using phx::kNoTexture;

inline std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
inline std::string fmt(const char* f, ...) {
    char b[1024];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(b, sizeof(b), f, ap);
    va_end(ap);
    return b;
}

// ============================================================================================
// input
// ============================================================================================
constexpr uint32_t kMouseL = 1u << PHX_MOUSE_LEFT;
constexpr uint32_t kMouseM = 1u << PHX_MOUSE_MIDDLE;
constexpr uint32_t kMouseR = 1u << PHX_MOUSE_RIGHT;

constexpr uint16_t kShift = PHX_MOD_SHIFT, kCtrl = PHX_MOD_CTRL, kAlt = PHX_MOD_ALT, kGui = PHX_MOD_GUI;

// Ctrl on Linux/Windows == Cmd on macOS: shortcut matching folds GUI into CTRL.
inline uint16_t norm_mods(uint16_t m) {
    if (m & kGui) m = uint16_t((m & ~kGui) | kCtrl);
    return m;
}

struct KeyEvent {
    int32_t  key = 0;
    uint16_t mods = 0;
    bool     repeat = false;
    bool     used = false;       // consumed by a widget / shortcut this frame
};

struct Input {
    int      mx = -1, my = -1;
    uint32_t held = 0;           // bitmask of kMouse*
    uint32_t pressed = 0;        // went down this frame (kept even for a press+release in one frame)
    uint32_t released = 0;
    int      clicks = 0;         // click count of this frame's left press (2 = double-click)
    int      wheel_x = 0, wheel_y = 0;
    uint16_t mods = 0;
    std::vector<KeyEvent> keys;  // key-DOWN events (auto-repeat included)
    std::string text;            // typed text (printable ASCII; the fonts are ASCII)
    bool     quit = false;       // the window close was vetoed (confirm-quit mode)
    int      resize_w = 0, resize_h = 0;
    std::vector<std::string> drops;

    void reset_frame() {
        pressed = released = 0; clicks = 0; wheel_x = wheel_y = 0;
        keys.clear(); text.clear(); quit = false; resize_w = resize_h = 0; drops.clear();
    }
    // Test helpers (the headless suite scripts frames with these).
    void key(int32_t k, uint16_t m = 0) { keys.push_back(KeyEvent{ k, m, false, false }); }
    void type(const char* s) { text += s; }
    void click_at(int x, int y, int n = 1) { mx = x; my = y; pressed |= kMouseL; released |= kMouseL; clicks = n; }
};

// Drain the desktop seam into one Input frame (call once per rendered frame).
inline void collect_desktop(Input& in) {
    in.reset_frame();
    phx_desktop_event e;
    while (phx_desktop_poll(&e)) {
        switch (e.kind) {
        case PHX_DEV_KEY_DOWN: in.keys.push_back(KeyEvent{ e.key, e.mods, e.repeat != 0, false }); break;
        case PHX_DEV_TEXT:
            for (const char* p = e.text; *p; ++p)
                if (static_cast<unsigned char>(*p) >= 32 && static_cast<unsigned char>(*p) < 127) in.text += *p;
            break;
        case PHX_DEV_MOUSE_DOWN:
            in.pressed |= 1u << e.button;
            if (e.button == PHX_MOUSE_LEFT) in.clicks = e.clicks;
            break;
        case PHX_DEV_MOUSE_UP:   in.released |= 1u << e.button; break;
        case PHX_DEV_WHEEL:      in.wheel_x += e.wheel_x; in.wheel_y += e.wheel_y; break;
        case PHX_DEV_RESIZE:     in.resize_w = e.x; in.resize_h = e.y; break;
        case PHX_DEV_DROP_FILE:  in.drops.push_back(phx_desktop_drop_path()); break;
        case PHX_DEV_QUIT:       in.quit = true; break;
        default: break;
        }
    }
    int x = -1, y = -1;
    uint32_t b = 0;
    phx_desktop_mouse(&x, &y, &b);
    in.mx = x; in.my = y; in.held = b;
    in.mods = phx_desktop_mods();
}

// ============================================================================================
// LineEdit — a single-line text buffer with caret + selection (the text field's model)
// ============================================================================================
struct LineEdit {
    std::string buf;
    int caret = 0, anchor = 0;   // selection = [min(caret,anchor), max(...))
    int scroll_px = 0;           // horizontal scroll of the display

    void set(const std::string& s, bool select_all) {
        buf = s;
        caret = int(buf.size());
        anchor = select_all ? 0 : caret;
        scroll_px = 0;
    }
    bool has_sel() const { return caret != anchor; }
    int sel_lo() const { return std::min(caret, anchor); }
    int sel_hi() const { return std::max(caret, anchor); }
    std::string selection() const { return buf.substr(size_t(sel_lo()), size_t(sel_hi() - sel_lo())); }
    void delete_sel() {
        if (!has_sel()) return;
        buf.erase(size_t(sel_lo()), size_t(sel_hi() - sel_lo()));
        caret = anchor = sel_lo();
    }
    void insert(const std::string& s) {
        delete_sel();
        std::string clean;
        for (char c : s) if (static_cast<unsigned char>(c) >= 32 && static_cast<unsigned char>(c) < 127) clean += c;
        buf.insert(size_t(caret), clean);
        caret += int(clean.size());
        anchor = caret;
    }
    static bool word_char(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }
    int word_left(int p) const {
        while (p > 0 && !word_char(buf[size_t(p - 1)])) --p;
        while (p > 0 && word_char(buf[size_t(p - 1)])) --p;
        return p;
    }
    int word_right(int p) const {
        const int n = int(buf.size());
        while (p < n && !word_char(buf[size_t(p)])) ++p;
        while (p < n && word_char(buf[size_t(p)])) ++p;
        return p;
    }
    void move_to(int p, bool extend) {
        caret = std::max(0, std::min(p, int(buf.size())));
        if (!extend) anchor = caret;
    }
    void backspace(bool word) {
        if (has_sel()) { delete_sel(); return; }
        const int to = word ? word_left(caret) : caret - 1;
        if (to < 0 || to == caret) return;
        buf.erase(size_t(to), size_t(caret - to));
        caret = anchor = to;
    }
    void del(bool word) {
        if (has_sel()) { delete_sel(); return; }
        const int to = word ? word_right(caret) : caret + 1;
        if (to > int(buf.size()) || to == caret) return;
        buf.erase(size_t(caret), size_t(to - caret));
        anchor = caret;
    }
    void select_all() { anchor = 0; caret = int(buf.size()); }
    void select_word_at(int p) {
        p = std::max(0, std::min(p, int(buf.size())));
        int a = p, b = p;
        while (a > 0 && word_char(buf[size_t(a - 1)])) --a;
        while (b < int(buf.size()) && word_char(buf[size_t(b)])) ++b;
        anchor = a; caret = b;
    }
};

// ============================================================================================
// theme + sprite-layer bands
// ============================================================================================
struct Theme {
    Rgba bg     = rgba(30, 30, 46);
    Rgba bar    = rgba(19, 19, 29);
    Rgba panel  = rgba(40, 41, 58);
    Rgba panel2 = rgba(50, 52, 72);
    Rgba hover  = rgba(68, 70, 98);
    Rgba line   = rgba(72, 74, 100);
    Rgba text   = rgba(230, 230, 240);
    Rgba dim    = rgba(146, 148, 172);
    Rgba faint  = rgba(96, 98, 124);
    Rgba accent = rgba(255, 138, 48);    // phosphorus orange
    Rgba good   = rgba(96, 206, 126);
    Rgba bad    = rgba(240, 86, 76);
    Rgba warn   = rgba(238, 194, 76);
    Rgba info   = rgba(112, 174, 246);
    Rgba violet = rgba(182, 134, 242);
    Rgba field  = rgba(24, 24, 36);      // text-field well
    Rgba sel    = rgba(64, 86, 140);     // text selection
    Rgba caret  = rgba(255, 200, 120);
};

inline Rgba scale_rgb(Rgba c, int num, int den) {
    auto ch = [&](uint32_t v) { return uint8_t(std::min<uint32_t>(255u, v * uint32_t(num) / uint32_t(den))); };
    return rgba(ch(phx::rgba_r(c)), ch(phx::rgba_g(c)), ch(phx::rgba_b(c)));
}
inline Rgba mix_rgb(Rgba a, Rgba b, int t256) {
    auto ch = [&](uint32_t x, uint32_t y) { return uint8_t((x * uint32_t(256 - t256) + y * uint32_t(t256)) >> 8); };
    return rgba(ch(phx::rgba_r(a), phx::rgba_r(b)), ch(phx::rgba_g(a), phx::rgba_g(b)), ch(phx::rgba_b(a), phx::rgba_b(b)));
}

// Within a plane, draws order by sub-layer: fill < widget < image < overlay < text < top. Planes
// stack: base (0) < popup (1) < modal (2) < modal popup (3) < tooltip (4). The renderer sorts
// sprites by (layer, z, TEXTURE), so content made of several textures that must stack in a set
// order (a map's tile layers) takes one sub-layer each: kSubImage + 0..kSubImageSpan-1.
enum Sub : uint8_t { kSubBg = 0, kSubFill = 2, kSubWidget = 4, kSubImage = 6, kSubOver = 22, kSubText = 24, kSubTop = 26 };
constexpr int kSubImageSpan = 16;
constexpr uint8_t kPlaneBase[5] = { 100, 130, 160, 190, 220 };
enum Plane : int { kPlaneMain = 0, kPlanePopup = 1, kPlaneModal = 2, kPlaneModalPopup = 3, kPlaneTip = 4 };

// Text metrics on the canvas grid: a 6px pitch, a 10px line, an 8px glyph cell whose baseline is at
// its bottom. The 5x7 ASCII bitmap font is drawn in that cell, and so is a TextRaster's face
// (Gui::set_text_raster), whose em is 10px so its advance is the same 6px.
constexpr int kAdv = 6, kLineH = 10, kGlyph = 8;

struct MenuItem {
    std::string label{}, shortcut{};
    bool enabled = true, checked = false, separator = false;
    int  icon = 0;
    static MenuItem sep() { MenuItem m; m.separator = true; return m; }
};

// Button options (designated-style: Btn b; b.icon = kIconSave; b.help = "...").
struct Btn {
    bool on = false, enabled = true, flat = false;
    int  icon = 0;
    const char* help = nullptr;
    Rgba tint = rgba(255, 138, 48);
    Rgba label = rgba(0, 0, 0, 0);    // alpha 0 = theme default
    bool left = false;                // left-align icon + label (list-style buttons)
};

enum FieldFlags : uint32_t {
    kFieldLive = 1,          // write every keystroke to the bound string (search boxes)
    kFieldSelectAll = 2,     // focusing selects everything (numeric fields)
    kFieldReadOnly = 4,
    kFieldMono = 8,          // draw in the plain text colour (no placeholder styling)
};

// ============================================================================================
// Gui
// ============================================================================================
class Gui {
public:
    Theme th;
    Input* in = nullptr;
    int W = 640, H = 360;
    uint64_t frame = 0;
    int ox = 0, oy = 0;          // camera compensation (sprites are camera-relative)
    std::string hint;            // status-bar help for whatever is hovered this frame
    int cursor = PHX_CURSOR_ARROW;

    TextureId font = kNoTexture, icons = kNoTexture, checker = kNoTexture;

    // Draw text with a TrueType-style rasterizer at WINDOW resolution instead of the 5x7 bitmap font
    // (null = the bitmap font, the default). Glyphs go to the platform's native-resolution overlay
    // (phx_desktop_overlay_begin), so they are smooth at any UI scale; where the platform has none
    // (headless, GL tier) or `ui == nullptr`, text falls back to the bitmap font for that frame.
    // The layout is identical either way: same 6px advance, same 10px line.
    void set_text_raster(TextRaster* r) { raster_ = r; }
    bool native_text() const { return native_; }   // this frame's text goes to the overlay

    // ---- setup -------------------------------------------------------------------------------
    // Build the font / icon / stipple textures (static storage: the soft backend samples RGBA8
    // zero-copy, so the pixels must outlive the renderer).
    void init(phx::Renderer& r) {
        static uint32_t font_px[phxtool::kAsciiFontW * phxtool::kAsciiFontH];
        static uint32_t icon_px[kIconAtlasW * kIconAtlasH];
        static uint32_t checker_px[64 * 64];
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x) checker_px[y * 64 + x] = ((x + y) & 1) ? 0u : 0xFFFFFFFFu;
        phxtool::build_ascii_font(font_px);
        build_icon_atlas(icon_px);
        phx::TextureDesc d{};
        d.pixels = font_px; d.width = phxtool::kAsciiFontW; d.height = phxtool::kAsciiFontH;
        font = r.load_texture(d);
        d.pixels = icon_px; d.width = kIconAtlasW; d.height = kIconAtlasH;
        icons = r.load_texture(d);
        d.pixels = checker_px; d.width = 64; d.height = 64;
        checker = r.load_texture(d);
    }

    // Begin a frame. `ui_r` may be null (headless tests: logic only, nothing drawn).
    void begin(phx::Renderer* ui_r, const phx::InputState* app_in, Input& frame_in, int w, int h,
               int cam_x = 0, int cam_y = 0) {
        in = &frame_in;
        W = w; H = h;
        ox = cam_x; oy = cam_y;
        if (ui_r && app_in) { ui_.begin(*ui_r, *app_in); draw_ = true; r_ = ui_r; }
        else { draw_ = false; r_ = nullptr; }
        native_ = false;
        nglyphs_.clear();
        covers_.clear();
        if (draw_ && raster_) {
            const int sc = phx_desktop_scale();
            phx_overlay o{};
            // the layer is (framebuffer x scale); a canvas size that disagrees (mid-resize) draws
            // this one frame with the bitmap font rather than misplace the text
            if (phx_desktop_overlay_begin(&o) && sc >= 1 && o.w == W * sc && o.h == H * sc) {
                ov_ = o; ov_scale_ = sc; native_ = true;
            }
        }
        ++frame;
        hint.clear();
        cursor = PHX_CURSOR_ARROW;
        clips_.clear();
        plane_ = 0;
        prev_overlays_.swap(overlays_);
        overlays_.clear();
        id_stack_.clear();
        // A capture whose widget stopped being drawn (view switched mid-drag) must not stick.
        if (active_ && !(in->held & drag_btn_) && !(in->released & drag_btn_)) active_ = 0;
        tip_text_.clear();

        // A press outside the open popup (and its anchor) closes it and is swallowed, so the
        // click that dismisses a menu never also activates whatever is beneath it.
        if (popup_ && popup_opened_ != frame - 1 && (in->pressed & (kMouseL | kMouseR))) {
            const bool inside = popup_rect_.contains(in->mx, in->my) || popup_anchor_.contains(in->mx, in->my);
            if (!inside) {
                close_popup();
                in->pressed &= ~(kMouseL | kMouseR);
            }
        }
        focus_seen_ = false;
    }

    void end() {
        // Tooltip after the pointer rests on the same widget for ~0.5 s.
        if (!tip_text_.empty()) {
            if (tip_rect_ == tip_prev_rect_ && in->mx == tip_mx_ && in->my == tip_my_) ++tip_frames_;
            else { tip_frames_ = 0; tip_mx_ = in->mx; tip_my_ = in->my; }
            tip_prev_rect_ = tip_rect_;
            if (tip_frames_ > 30 && !(in->held & kMouseL)) draw_tooltip();
        } else tip_frames_ = 0;
        flush_native_text();                       // after the tooltip: it is text too
        // A focused widget that was not drawn this frame loses focus (tab switched away).
        if (focus_ && !focus_seen_) { focus_ = 0; focus_text_ = false; }
        if (draw_) ui_.end();
        if (draw_) {
            if (want_text_ != text_on_) { phx_desktop_text_input(want_text_ ? 1 : 0); text_on_ = want_text_; }
            phx_desktop_set_cursor(cursor);
        }
        want_text_ = false;
    }

    // ---- ids ----------------------------------------------------------------------------------
    static uint32_t hash(const char* s, uint32_t h = 2166136261u) {
        for (; *s; ++s) { h ^= uint8_t(*s); h *= 16777619u; }
        return h ? h : 1u;
    }
    uint32_t id(const char* s) const { return hash(s, id_stack_.empty() ? 2166136261u : id_stack_.back()); }
    uint32_t id(const char* s, int n) const {
        uint32_t h = id(s);
        h ^= uint32_t(n) * 2654435761u;
        return h ? h : 1u;
    }
    void push_id(uint32_t v) { id_stack_.push_back(v); }
    void push_id(const char* s) { id_stack_.push_back(id(s)); }
    void pop_id() { if (!id_stack_.empty()) id_stack_.pop_back(); }

    // ---- planes + clipping ---------------------------------------------------------------------
    int  plane() const { return plane_; }
    void set_plane(int p) { plane_ = std::max(0, std::min(4, p)); }
    uint8_t layer(uint8_t sub) const { return uint8_t(kPlaneBase[plane_] + sub); }

    void push_clip(const Rect& r) { clips_.push_back(clips_.empty() ? r : intersect(clips_.back(), r)); }
    void pop_clip() { if (!clips_.empty()) clips_.pop_back(); }
    Rect clip() const { return clips_.empty() ? Rect{ -100000, -100000, 200000, 200000 } : clips_.back(); }

    // Mark `r` as covering everything on lower planes (the pointer is theirs, not beneath).
    void add_overlay(const Rect& r) { overlays_.push_back(Overlay{ r, plane_ }); }

    // ---- drawing -------------------------------------------------------------------------------
    void rect(const Rect& r0, Rgba c, uint8_t sub = kSubFill) {
        if (!draw_) return;
        const Rect r = intersect(r0, clip());
        if (r.empty()) return;
        note_cover(r, sub, false);
        ui_.rect(phx::UIRect{ v2(r.x + ox, r.y + oy), v2(r.w, r.h) }, c, layer(sub));
    }
    void frame_rect(const Rect& r, Rgba c, uint8_t sub = kSubWidget) {
        rect(Rect{ r.x, r.y, r.w, 1 }, c, sub);
        rect(Rect{ r.x, r.bottom() - 1, r.w, 1 }, c, sub);
        rect(Rect{ r.x, r.y + 1, 1, r.h - 2 }, c, sub);
        rect(Rect{ r.right() - 1, r.y + 1, 1, r.h - 2 }, c, sub);
    }
    // 50% "translucent" fill: a 1px checker in colour c (alpha-test friendly).
    void stipple(const Rect& r0, Rgba c, uint8_t sub = kSubOver) {
        if (!draw_) return;
        const Rect r = intersect(r0, clip());
        note_cover(r, sub, true);                  // native text under it shows at half strength
        // 62px blocks from the 64x64 checker, source offset by coordinate parity so the pattern
        // is anchored to the screen (adjacent stipples line up seamlessly).
        for (int y = r.y; y < r.bottom(); y += 62)
            for (int x = r.x; x < r.right(); x += 62)
                blit(Rect{ x, y, std::min(62, r.right() - x), std::min(62, r.bottom() - y) }, checker,
                     (x + ox) & 1, (y + oy) & 1, std::min(62, r.right() - x), std::min(62, r.bottom() - y), c, sub, 0);
    }
    void hline(int x, int y, int w, Rgba c, uint8_t sub = kSubWidget) { rect(Rect{ x, y, w, 1 }, c, sub); }
    void vline(int x, int y, int h, Rgba c, uint8_t sub = kSubWidget) { rect(Rect{ x, y, 1, h }, c, sub); }
    // Dashed outline (marching ants when `phase` advances).
    void dashed(const Rect& r, Rgba a, Rgba b, int phase = 0, uint8_t sub = kSubOver) {
        auto seg = [&](int x, int y, int n, bool horiz) {
            for (int i = 0; i < n; i += 2) {
                const Rgba c = (((i + phase) / 2) & 1) ? a : b;
                if (horiz) rect(Rect{ x + i, y, std::min(2, n - i), 1 }, c, sub);
                else       rect(Rect{ x, y + i, 1, std::min(2, n - i) }, c, sub);
            }
        };
        seg(r.x, r.y, r.w, true);
        seg(r.x, r.bottom() - 1, r.w, true);
        seg(r.x, r.y, r.h, false);
        seg(r.right() - 1, r.y, r.h, false);
    }
    // 1px line (Bresenham by row runs).
    void line(int x0, int y0, int x1, int y1, Rgba c, uint8_t sub = kSubOver) {
        if (y0 > y1) { std::swap(x0, x1); std::swap(y0, y1); }
        const int dy = y1 - y0;
        if (dy == 0) { rect(Rect{ std::min(x0, x1), y0, std::abs(x1 - x0) + 1, 1 }, c, sub); return; }
        int px = x0;
        for (int y = y0; y <= y1; ++y) {
            const int x = x0 + (x1 - x0) * (y - y0) / dy;
            const int a = std::min(px, x), b = std::max(px, x);
            rect(Rect{ a, y, std::max(1, b - a + (a == b ? 1 : 0)), 1 }, c, sub);
            px = x;
        }
    }

    // A clipped image. Integer scales (dw = k*sw) clip texel-exactly (a partial edge texel is
    // drawn as its own narrower quad); other scales trim the source proportionally.
    void image(const Rect& dst, TextureId t, int sx, int sy, int sw, int sh,
               Rgba tint = rgba(255, 255, 255), uint8_t sub = kSubImage, uint16_t flags = 0) {
        if (!draw_ || t == kNoTexture || dst.empty() || sw <= 0 || sh <= 0) return;
        const Rect c = intersect(dst, clip());
        if (c.empty()) return;
        if (c == dst) { blit(dst, t, sx, sy, sw, sh, tint, sub, flags); return; }
        const bool int_x = dst.w % sw == 0, int_y = dst.h % sh == 0;
        if (!int_x || !int_y || flags) {
            const int nsx = sx + int(int64_t(c.x - dst.x) * sw / dst.w);
            const int nsy = sy + int(int64_t(c.y - dst.y) * sh / dst.h);
            const int nex = sx + int((int64_t(c.right() - dst.x) * sw + dst.w - 1) / dst.w);
            const int ney = sy + int((int64_t(c.bottom() - dst.y) * sh + dst.h - 1) / dst.h);
            blit(c, t, nsx, nsy, std::max(1, nex - nsx), std::max(1, ney - nsy), tint, sub, flags);
            return;
        }
        const int kx = dst.w / sw, ky = dst.h / sh;
        // Split each axis into [partial first texel][whole texels][partial last texel].
        struct Span { int d0, dlen, s0, slen; };
        auto spans = [](int d, int k, int c0, int c1, int s, Span out[3]) {
            int n = 0;
            const int t0 = (c0 - d) / k, t1 = (c1 - d + k - 1) / k;   // texels touched [t0, t1)
            int t = t0;
            if ((c0 - d) % k) {                                         // partial first
                const int e = std::min(c1, d + (t0 + 1) * k);
                out[n++] = Span{ c0, e - c0, s + t0, 1 };
                ++t;
            }
            int full_end = t1;
            if ((c1 - d) % k && t1 - 1 >= t) --full_end;                // partial last
            if (full_end > t) out[n++] = Span{ d + t * k, (full_end - t) * k, s + t, full_end - t };
            if (full_end < t1 && full_end >= t) {
                const int st = d + full_end * k;
                out[n++] = Span{ st, c1 - st, s + full_end, 1 };
            }
            return n;
        };
        Span xs[3], ys[3];
        const int nx = spans(dst.x, kx, c.x, c.right(), sx, xs);
        const int ny = spans(dst.y, ky, c.y, c.bottom(), sy, ys);
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i)
                if (xs[i].dlen > 0 && ys[j].dlen > 0)
                    blit(Rect{ xs[i].d0, ys[j].d0, xs[i].dlen, ys[j].dlen }, t, xs[i].s0, ys[j].s0,
                         xs[i].slen, ys[j].slen, tint, sub, 0);
    }
    void icon(int x, int y, int ic, Rgba c, uint8_t sub = kSubText, int scale = 1) {
        if (ic <= 0 || ic >= kIconCount) return;
        image(Rect{ x, y, kIconSize * scale, kIconSize * scale }, icons,
              (ic % kIconCols) * kIconSize, (ic / kIconCols) * kIconSize, kIconSize, kIconSize, c, sub);
    }

    // Text on the 6px pitch; `max_w` truncates with "..". Returns the drawn width.
    int text(int x, int y, const std::string& s, Rgba c, uint8_t sub = kSubText, int max_w = INT_MAX, int scale = 1) {
        return text_n(x, y, s.data(), int(s.size()), c, sub, max_w, scale);
    }
    int text_n(int x, int y, const char* s, int n, Rgba c, uint8_t sub = kSubText, int max_w = INT_MAX, int scale = 1) {
        const int adv = kAdv * scale;
        const int fit = max_w == INT_MAX ? n : std::max(0, max_w / adv);
        const bool cut = n > fit;
        const int m = cut ? std::max(0, fit - 2) : n;
        int cx = x;
        if (draw_) {
            const Rect cl = clip();
            for (int i = 0; i < m; ++i, cx += adv) {
                if (cx + adv <= cl.x) continue;
                if (cx >= cl.right()) { cx += (m - i) * adv; break; }
                glyph(cx, y, s[i], c, sub, scale);
            }
            if (cut && fit >= 2) { glyph(cx, y, '.', th.dim, sub, scale); glyph(cx + adv, y, '.', th.dim, sub, scale); cx += 2 * adv; }
        } else {
            cx += m * adv + (cut && fit >= 2 ? 2 * adv : 0);
        }
        return cx - x;
    }
    void glyph(int x, int y, char ch, Rgba c, uint8_t sub = kSubText, int scale = 1) {
        int g = int(static_cast<unsigned char>(ch)) - 32;
        if (g == 0) return;
        if (native_) {
            // queued, then blended at window resolution in end() once every cover is known
            const Rect cell{ x, y, kAdv * scale, 12 * scale };     // 12 = the em's ascent + descent, in canvas px
            if (intersect(cell, clip()).empty()) return;
            nglyphs_.push_back(NGlyph{ x, y, static_cast<unsigned char>(ch), scale, c, plane_, sub, clip() });
            return;
        }
        if (g < 0 || g >= phxtool::kAsciiGlyphs) g = phxtool::kAsciiGlyphs - 1;
        image(Rect{ x, y, kGlyph * scale, kGlyph * scale }, font, (g % 16) * 8, (g / 16) * 8, 8, 8, c, sub);
    }
    static int text_w(const std::string& s, int scale = 1) { return int(s.size()) * kAdv * scale; }
    void text_center(const Rect& r, const std::string& s, Rgba c, uint8_t sub = kSubText) {
        const int w = std::min(text_w(s), r.w - 4);
        text(r.x + (r.w - w) / 2, r.y + (r.h - 8) / 2 + 1, s, c, sub, r.w - 4);
    }
    void text_right(int right, int y, const std::string& s, Rgba c, uint8_t sub = kSubText) {
        text(right - text_w(s), y, s, c, sub);
    }

    // Section title with a rule to the right.
    void section(int x, int y, int w, const std::string& title, Rgba c) {
        text(x, y, title, c);
        rect(Rect{ x + text_w(title) + 4, y + 4, std::max(0, w - text_w(title) - 4), 1 }, th.line, kSubWidget);
    }

    // ---- pointer -------------------------------------------------------------------------------
    int mx() const { return in ? in->mx : -1; }
    int my() const { return in ? in->my : -1; }
    // True when the pointer is over r, inside the current clip, and not covered by a higher
    // plane's overlay (from the previous frame).
    bool hover(const Rect& r) const {
        if (!in || !r.contains(in->mx, in->my) || !clip().contains(in->mx, in->my)) return false;
        for (const Overlay& o : prev_overlays_)
            if (o.plane > plane_ && o.r.contains(in->mx, in->my)) return false;
        return true;
    }
    bool pressed(const Rect& r, uint32_t b = kMouseL) const { return (in->pressed & b) && hover(r); }
    // A left press on r that no other widget has taken (consumes the press).
    bool clicked(const Rect& r) {
        if (!(in->pressed & kMouseL) || !hover(r)) return false;
        in->pressed &= ~kMouseL;
        return true;
    }
    bool right_clicked(const Rect& r) {
        if (!(in->pressed & kMouseR) || !hover(r)) return false;
        in->pressed &= ~kMouseR;
        return true;
    }
    bool double_clicked(const Rect& r) const { return in->clicks >= 2 && (in->pressed & kMouseL) && hover(r); }
    // Pointer capture: press on r with button `b` makes `wid` active until release. Returns true
    // while this widget owns the drag (including the press frame).
    bool drag(uint32_t wid, const Rect& r, uint32_t b = kMouseL) {
        if (active_ == 0 && (in->pressed & b) && hover(r)) {
            active_ = wid; drag_btn_ = b;
            drag_x0_ = in->mx; drag_y0_ = in->my;
            drag_frame_ = frame;
            in->pressed &= ~b;
            // a press+release inside one frame is still a (one-frame) drag: report it once
            return true;
        }
        if (active_ == wid) {
            if ((in->held & drag_btn_) && !(in->released & drag_btn_)) return true;
            active_ = 0;
        }
        return false;
    }
    bool dragging(uint32_t wid) const { return active_ == wid; }
    int  drag_dx() const { return in->mx - drag_x0_; }
    int  drag_dy() const { return in->my - drag_y0_; }
    uint32_t active() const { return active_; }
    void release_active() { active_ = 0; }

    // Wheel over r (consumed). Returns the notches (+ = up/away).
    int wheel(const Rect& r) {
        if (!in || !in->wheel_y || !hover(r)) return 0;
        const int w = in->wheel_y;
        in->wheel_y = 0;
        return w;
    }
    int wheel_x(const Rect& r) {
        if (!in || !in->wheel_x || !hover(r)) return 0;
        const int w = in->wheel_x;
        in->wheel_x = 0;
        return w;
    }

    // ---- keyboard ------------------------------------------------------------------------------
    // Match (and consume) a key-down with EXACTLY these modifiers (GUI folds into CTRL).
    bool key(int32_t k, uint16_t mods = 0) {
        if (!in) return false;
        for (KeyEvent& e : in->keys)
            if (!e.used && e.key == k && norm_mods(e.mods) == norm_mods(mods)) { e.used = true; return true; }
        return false;
    }
    // Same, ignoring Shift (for navigation keys that extend a selection with Shift).
    bool key_any_shift(int32_t k, uint16_t mods, bool& shift) {
        if (!in) return false;
        for (KeyEvent& e : in->keys)
            if (!e.used && e.key == k && (norm_mods(e.mods) & ~kShift) == norm_mods(mods)) {
                e.used = true; shift = (e.mods & kShift) != 0; return true;
            }
        return false;
    }
    // Plain-key shortcuts (tool letters) only fire when no text widget owns the keyboard.
    // (Esc is left to an open popup/menu, which closes on it, even when that popup draws later.)
    bool hotkey(int32_t k, uint16_t mods = 0) { return !focus_text_ && !(k == PHX_KEY_ESCAPE && popup_) && key(k, mods); }

    // ---- focus ---------------------------------------------------------------------------------
    uint32_t focus() const { return focus_; }
    bool text_focus() const { return focus_ != 0 && focus_text_; }
    void set_focus(uint32_t wid, bool text) { focus_ = wid; focus_text_ = text; focus_seen_ = true; }
    void blur() { focus_ = 0; focus_text_ = false; }
    // A widget that owns focus calls this every frame it is drawn (keeps focus alive) and
    // requests OS text input while it does.
    void keep_focus(uint32_t wid, bool text) {
        if (focus_ == wid) { focus_seen_ = true; if (text) want_text_ = true; }
    }

    // ---- tooltips + hints ----------------------------------------------------------------------
    void tip(const Rect& r, const std::string& text_) {
        if (!hover(r) || text_.empty()) return;
        hint = text_;
        tip_text_ = text_;
        tip_rect_ = r;
    }

    // ============================================================================================
    // widgets
    // ============================================================================================
    bool button(const Rect& r, const std::string& label, const Btn& o = Btn{}) {
        const bool h = o.enabled && hover(r);
        const Rgba fill = !o.enabled ? th.panel : o.on ? scale_rgb(o.tint, 1, 3) : h ? th.hover : (o.flat ? th.panel : th.panel2);
        if (!o.flat || h || o.on) rect(r, fill, kSubWidget);
        if (!o.flat) frame_rect(r, o.on ? o.tint : h ? th.line : th.panel2, kSubWidget);
        else if (o.on) rect(Rect{ r.x, r.bottom() - 2, r.w, 2 }, o.tint, kSubOver);
        const Rgba lc = !o.enabled ? th.faint : (phx::rgba_a(o.label) ? o.label : th.text);
        if (o.left) {
            int x0 = r.x + 4;
            if (o.icon) { icon(x0, r.y + (r.h - kIconSize) / 2, o.icon, lc); x0 += kIconSize + 4; }
            text(x0, r.y + (r.h - 8) / 2 + 1, label, lc, kSubText, r.right() - x0 - 2);
        } else if (o.icon) {
            const int tw = label.empty() ? 0 : text_w(label) + 3;
            const int x0 = r.x + (r.w - kIconSize - tw) / 2;
            icon(x0, r.y + (r.h - kIconSize) / 2, o.icon, lc);
            if (!label.empty()) text(x0 + kIconSize + 3, r.y + (r.h - 8) / 2 + 1, label, lc, kSubText, r.right() - x0 - kIconSize - 3);
        } else {
            text_center(r, label, lc);
        }
        if (o.help) tip(r, o.help);
        return o.enabled && clicked(r);
    }
    bool icon_button(const Rect& r, int ic, const char* help, bool on = false, bool enabled = true) {
        Btn b; b.icon = ic; b.help = help; b.on = on; b.enabled = enabled; b.flat = true;
        return button(r, "", b);
    }

    bool checkbox(const Rect& r, const std::string& label, bool& v, const char* help = nullptr) {
        const Rect box{ r.x, r.y + (r.h - 9) / 2, 9, 9 };
        const bool h = hover(r);
        rect(box, th.field, kSubWidget);
        frame_rect(box, h ? th.accent : th.line, kSubWidget);
        if (v) rect(box.inset(2), th.accent, kSubImage);
        text(box.right() + 4, r.y + (r.h - 8) / 2 + 1, label, h ? th.text : th.dim, kSubText, r.w - 13);
        if (help) tip(r, help);
        if (clicked(r)) { v = !v; return true; }
        return false;
    }

    // A single-line text field bound to `s`. Returns true when `s` changed (on commit —
    // Enter / focus loss — or every keystroke with kFieldLive).
    bool text_field(uint32_t wid, const Rect& r, std::string& s, const char* placeholder = nullptr,
                    uint32_t flags = 0, const char* help = nullptr) {
        bool changed = false;
        const bool h = hover(r);
        if (h) cursor = PHX_CURSOR_IBEAM;
        if (help) tip(r, help);
        const int tx = r.x + 3;
        const bool ro = (flags & kFieldReadOnly) != 0;

        if (focus_ == wid) {
            keep_focus(wid, true);
            // click outside commits and releases focus
            if ((in->pressed & (kMouseL | kMouseR)) && !r.contains(in->mx, in->my)) {
                if (!ro && edit_.buf != s) { s = edit_.buf; changed = true; }
                blur();
            } else {
                const std::string before = edit_.buf;
                if (edit_keys(ro)) {                              // Enter
                    if (!ro && edit_.buf != s) { s = edit_.buf; changed = true; }
                    blur();
                } else if (focus_ == 0) {                         // Esc reverted
                } else if ((flags & kFieldLive) && edit_.buf != before && !ro) {
                    s = edit_.buf; changed = true;
                }
            }
        }
        // mouse: click focuses (+ caret placement), drag selects, double-click selects a word
        if (drag(wid, r)) {
            if (focus_ != wid) {
                set_focus(wid, true);
                edit_.set(s, (flags & kFieldSelectAll) != 0);
                edit_id_ = wid;
                if (!(flags & kFieldSelectAll)) edit_.move_to(caret_from_x(in->mx - tx), false);
                select_drag_ = !(flags & kFieldSelectAll);
            } else if (just_pressed(wid)) {
                const int p = caret_from_x(in->mx - tx + edit_.scroll_px);
                select_drag_ = true;
                if (in->clicks >= 2) { edit_.select_word_at(p); select_drag_ = false; }
                else edit_.move_to(p, (in->mods & kShift) != 0);
            } else if (select_drag_) {
                edit_.move_to(caret_from_x(in->mx - tx + edit_.scroll_px), true);
            }
        }

        // draw
        const bool f = focus_ == wid;
        rect(r, th.field, kSubWidget);
        frame_rect(r, f ? th.accent : h ? th.line : th.panel2, kSubWidget);
        push_clip(r.inset(1));
        if (f) {
            // keep the caret in view
            const int cx = edit_.caret * kAdv;
            if (cx - edit_.scroll_px > r.w - 8) edit_.scroll_px = cx - (r.w - 8);
            if (cx - edit_.scroll_px < 0) edit_.scroll_px = cx;
            const int base = tx - edit_.scroll_px;
            if (edit_.has_sel())
                rect(Rect{ base + edit_.sel_lo() * kAdv, r.y + 2, (edit_.sel_hi() - edit_.sel_lo()) * kAdv, r.h - 4 }, th.sel, kSubImage);
            text(base, r.y + (r.h - 8) / 2 + 1, edit_.buf, th.text);
            if ((frame / 30) % 2 == 0 || in->keys.size())
                rect(Rect{ base + edit_.caret * kAdv - 1, r.y + 2, 1, r.h - 4 }, th.caret, kSubTop);
        } else if (s.empty() && placeholder) {
            text(tx, r.y + (r.h - 8) / 2 + 1, placeholder, th.faint, kSubText, r.w - 6);
        } else {
            text(tx, r.y + (r.h - 8) / 2 + 1, s, ro ? th.dim : th.text, kSubText, r.w - 6);
        }
        pop_clip();
        return changed;
    }

    // Integer field: type a value (click), wheel ±step, or drag horizontally to scrub.
    bool int_field(uint32_t wid, const Rect& r, int64_t& v, int64_t lo, int64_t hi, int64_t step = 1,
                   const char* help = nullptr) {
        bool changed = false;
        if (focus_ == wid) {
            std::string s = std::to_string(v);
            if (text_field(wid, r, s, nullptr, kFieldSelectAll, help)) {
                char* end = nullptr;
                const long long parsed = std::strtoll(s.c_str(), &end, 0);
                if (end && end != s.c_str()) {
                    const int64_t nv = std::max<int64_t>(lo, std::min<int64_t>(hi, parsed));
                    if (nv != v) { v = nv; changed = true; }
                }
            }
            return changed;
        }
        const bool h = hover(r);
        if (help) tip(r, help);
        if (h) cursor = PHX_CURSOR_SIZE_WE;
        if (const int w = wheel(r)) {
            const int64_t nv = std::max<int64_t>(lo, std::min<int64_t>(hi, v + w * step));
            if (nv != v) { v = nv; changed = true; }
        }
        const uint32_t sid = wid ^ 0x5C0B5C0Bu;
        const bool was = active_ == sid;
        if (drag(sid, r)) {
            if (just_pressed(sid)) { scrub_v0_ = v; scrub_moved_ = false; }
            const int dx = drag_dx();
            if (std::abs(dx) > 2) scrub_moved_ = true;
            if (scrub_moved_) {
                const int64_t nv = std::max<int64_t>(lo, std::min<int64_t>(hi, scrub_v0_ + (dx / 3) * step));
                if (nv != v) { v = nv; changed = true; }
            }
        } else if (was && !scrub_moved_) {
            // released without moving: a click, so type a value instead
            set_focus(wid, true);
            edit_.set(std::to_string(v), true);
            edit_id_ = wid;
        }
        rect(r, th.field, kSubWidget);
        frame_rect(r, h || active_ == sid ? th.line : th.panel2, kSubWidget);
        text(r.x + 3, r.y + (r.h - 8) / 2 + 1, std::to_string(v), th.text, kSubText, r.w - 6);
        return changed;
    }
    bool int_field(uint32_t wid, const Rect& r, int& v, int lo, int hi, int step = 1, const char* help = nullptr) {
        int64_t t = v;
        const bool c = int_field(wid, r, t, lo, hi, step, help);
        v = int(t);
        return c;
    }

    bool float_field(uint32_t wid, const Rect& r, double& v, double lo, double hi, double step = 0.05,
                     const char* help = nullptr) {
        bool changed = false;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%g", v);
        std::string s = buf;
        if (focus_ == wid) {
            if (text_field(wid, r, s, nullptr, kFieldSelectAll, help)) {
                char* end = nullptr;
                const double p = std::strtod(s.c_str(), &end);
                if (end && end != s.c_str()) {
                    const double nv = std::max(lo, std::min(hi, p));
                    if (nv != v) { v = nv; changed = true; }
                }
            }
            return changed;
        }
        const bool h = hover(r);
        if (help) tip(r, help);
        if (const int w = wheel(r)) {
            const double nv = std::max(lo, std::min(hi, v + w * step));
            if (nv != v) { v = nv; changed = true; }
        }
        if (clicked(r)) { set_focus(wid, true); edit_.set(s, true); edit_id_ = wid; }
        rect(r, th.field, kSubWidget);
        frame_rect(r, h ? th.line : th.panel2, kSubWidget);
        text(r.x + 3, r.y + (r.h - 8) / 2 + 1, s, th.text, kSubText, r.w - 6);
        return changed;
    }

    // Horizontal slider over [lo, hi].
    bool slider(uint32_t wid, const Rect& r, int& v, int lo, int hi, Rgba fill, const char* help = nullptr) {
        bool changed = false;
        if (help) tip(r, help);
        if (drag(wid, r)) {
            const int nv = lo + int(int64_t(std::max(0, std::min(r.w - 1, in->mx - r.x))) * (hi - lo) / std::max(1, r.w - 1));
            if (nv != v) { v = nv; changed = true; }
        }
        if (const int w = wheel(r)) {
            const int nv = std::max(lo, std::min(hi, v + w));
            if (nv != v) { v = nv; changed = true; }
        }
        rect(r, th.field, kSubWidget);
        const int fw = hi > lo ? int(int64_t(v - lo) * (r.w - 2) / (hi - lo)) : 0;
        rect(Rect{ r.x + 1, r.y + 1, fw, r.h - 2 }, fill, kSubImage);
        rect(Rect{ r.x + 1 + fw - 1, r.y, 2, r.h }, th.text, kSubOver);
        return changed;
    }

    // Vertical scrollbar for `count` rows showing `visible`; drag or click the track.
    bool scrollbar_v(uint32_t wid, const Rect& track, int count, int visible, int& scroll) {
        rect(track, th.bar, kSubWidget);
        if (count <= visible) return false;
        int pos = 0, len = 0;
        scroll_thumb(count, visible, scroll, track.h, pos, len);
        const bool h = hover(track);
        rect(Rect{ track.x + 1, track.y + pos, track.w - 2, len }, active_ == wid ? th.accent : h ? th.dim : th.faint, kSubImage);
        if (drag(wid, track)) {
            const int ns = scroll_from_track(count, visible, track.h, in->my - track.y);
            if (ns != scroll) { scroll = ns; return true; }
        }
        return false;
    }
    bool scrollbar_h(uint32_t wid, const Rect& track, int count, int visible, int& scroll) {
        rect(track, th.bar, kSubWidget);
        if (count <= visible) return false;
        int pos = 0, len = 0;
        scroll_thumb(count, visible, scroll, track.w, pos, len);
        const bool h = hover(track);
        rect(Rect{ track.x + pos, track.y + 1, len, track.h - 2 }, active_ == wid ? th.accent : h ? th.dim : th.faint, kSubImage);
        if (drag(wid, track)) {
            const int ns = scroll_from_track(count, visible, track.w, in->mx - track.x);
            if (ns != scroll) { scroll = ns; return true; }
        }
        return false;
    }
    // Wheel-scroll a list area (rows).
    void wheel_scroll(const Rect& area, int& scroll, int count, int visible, int step = 3) {
        if (const int w = wheel(area)) scroll = clamp_scroll(scroll - w * step, count, visible);
    }

    // A drag handle that moves `pos` (a coordinate) within [lo, hi]. `vertical_bar` = the handle
    // is a vertical bar dragged left/right.
    bool splitter(uint32_t wid, const Rect& handle, int& pos, int lo, int hi, bool vertical_bar = true) {
        const bool h = hover(handle);
        if (h || active_ == wid) cursor = vertical_bar ? PHX_CURSOR_SIZE_WE : PHX_CURSOR_SIZE_NS;
        bool moved = false;
        if (drag(wid, handle)) {
            if (just_pressed(wid)) split_off_ = (vertical_bar ? in->mx : in->my) - pos;
            const int np = std::max(lo, std::min(hi, (vertical_bar ? in->mx : in->my) - split_off_));
            if (np != pos) { pos = np; moved = true; }
        }
        rect(handle, active_ == wid ? th.accent : h ? th.line : th.bar, kSubWidget);
        return moved;
    }

    // A tab strip; returns the clicked tab (or -1). `close` (optional) receives a tab whose
    // close box was clicked. `marks` (optional) draws a dot (e.g. unsaved) per tab.
    int tabs(const Rect& r, const std::vector<std::string>& labels, int active, int* close = nullptr,
             const std::vector<bool>* marks = nullptr, const std::vector<int>* icons_ = nullptr) {
        int hit = -1;
        int x = r.x;
        rect(r, th.bar, kSubFill);
        push_clip(r);
        // padding shrinks (14 -> 6 px) when the labels would not fit the strip
        int need = 0;
        for (size_t i = 0; i < labels.size(); ++i)
            need += text_w(labels[i]) + ((icons_ && i < icons_->size() && (*icons_)[i]) ? kIconSize + 3 : 0) + (close ? 12 : 0) + 1;
        const int pad = labels.empty() ? 14 : std::max(6, std::min(14, (r.w - need) / int(labels.size())));
        for (size_t i = 0; i < labels.size(); ++i) {
            const int ic = icons_ && i < icons_->size() ? (*icons_)[i] : 0;
            const int w = text_w(labels[i]) + pad + (ic ? kIconSize + 3 : 0) + (close ? 12 : 0);
            const Rect tr{ x, r.y, w, r.h };
            const bool on = int(i) == active;
            const bool h = hover(tr);
            rect(tr, on ? th.panel2 : h ? th.panel : th.bar, kSubWidget);
            if (on) rect(Rect{ tr.x, tr.y, tr.w, 2 }, th.accent, kSubOver);
            int tx = tr.x + pad / 2;
            if (ic) { icon(tx, tr.y + (tr.h - kIconSize) / 2, ic, on ? th.text : th.dim); tx += kIconSize + 3; }
            text(tx, tr.y + (tr.h - 8) / 2 + 1, labels[i], on ? th.text : th.dim);
            if (close) {
                const Rect cr{ tr.right() - 13, tr.y + (tr.h - 10) / 2, 10, 10 };
                const bool dirty = marks && i < marks->size() && (*marks)[i];
                const bool ch = hover(cr);
                if (dirty && !ch) icon(cr.x, cr.y, kIconDot, th.accent);
                else if (h || on) icon(cr.x, cr.y, kIconClose, ch ? th.bad : th.faint);
                if (ch) hint = "Close";
                if (clicked(cr)) *close = int(i);
            } else if (marks && i < marks->size() && (*marks)[i]) {
                icon(tr.right() - 11, tr.y + (tr.h - 10) / 2, kIconDot, th.accent);
            }
            if (clicked(tr)) hit = int(i);
            x += w + 1;
        }
        pop_clip();
        return hit;
    }

    // ---- popups: menus, dropdowns, context menus -----------------------------------------------
    void open_popup(uint32_t pid, const Rect& anchor) {
        popup_ = pid; popup_anchor_ = anchor; popup_opened_ = frame;
        popup_plane_ = plane_ + 1;
        popup_at_mouse_ = false;
        popup_scroll_ = 0;
    }
    void open_context(uint32_t pid) {
        open_popup(pid, Rect{ in->mx, in->my, 1, 1 });
        popup_at_mouse_ = true;
    }
    bool popup_open(uint32_t pid) const { return popup_ == pid; }
    bool any_popup() const { return popup_ != 0; }
    void close_popup() { popup_ = 0; popup_rect_ = Rect{}; popup_anchor_ = Rect{}; }

    // A menu-bar / toolbar button that opens popup `pid` below itself.
    bool menu_button(uint32_t pid, const Rect& r, const std::string& label, int ic = 0) {
        const bool open = popup_ == pid;
        const bool h = hover(r);
        rect(r, open ? th.panel2 : h ? th.panel : th.bar, kSubWidget);
        int tx = r.x + 6;
        if (ic) { icon(tx, r.y + (r.h - kIconSize) / 2, ic, open || h ? th.text : th.dim); tx += kIconSize + 3; }
        text(tx, r.y + (r.h - 8) / 2 + 1, label, open || h ? th.text : th.dim);
        // hovering another menu while one is open switches to it (menu-bar feel)
        if (h && popup_ && popup_ != pid && popup_anchor_.y == r.y && !popup_at_mouse_) open_popup(pid, r);
        if (clicked(r)) {
            if (open) close_popup(); else open_popup(pid, r);
        }
        return popup_ == pid;
    }

    // Draw popup `pid` as a menu if it is open; returns the clicked item index or -1.
    int menu(uint32_t pid, const std::vector<MenuItem>& items, int min_w = 0) {
        if (popup_ != pid) return -1;
        int w = min_w, h = 4;
        for (const MenuItem& m : items) {
            if (m.separator) { h += 5; continue; }
            w = std::max(w, 12 + 14 + text_w(m.label) + (m.shortcut.empty() ? 0 : 16 + text_w(m.shortcut)) + 8);
            h += 13;
        }
        Rect pr = place_popup(w, h);
        const int saved = plane_;
        std::vector<Rect> saved_clips;
        saved_clips.swap(clips_);
        set_plane(popup_plane_);
        add_overlay(pr);
        popup_rect_ = pr;
        rect(pr, th.panel, kSubFill);
        frame_rect(pr, th.line, kSubWidget);
        rect(Rect{ pr.x + 2, pr.bottom(), pr.w, 2 }, th.bar, kSubFill);      // drop shadow
        rect(Rect{ pr.right(), pr.y + 2, 2, pr.h }, th.bar, kSubFill);
        int y = pr.y + 2, hit = -1;
        for (size_t i = 0; i < items.size(); ++i) {
            const MenuItem& m = items[i];
            if (m.separator) { rect(Rect{ pr.x + 4, y + 2, pr.w - 8, 1 }, th.line, kSubWidget); y += 5; continue; }
            const Rect ir{ pr.x + 2, y, pr.w - 4, 13 };
            const bool hv = m.enabled && hover(ir);
            if (hv) rect(ir, th.hover, kSubWidget);
            const Rgba c = m.enabled ? th.text : th.faint;
            if (m.checked) icon(ir.x + 2, ir.y + 1, kIconCheck, th.accent);
            else if (m.icon) icon(ir.x + 2, ir.y + 1, m.icon, m.enabled ? th.dim : th.faint);
            text(ir.x + 16, ir.y + 3, m.label, c);
            if (!m.shortcut.empty()) text_right(ir.right() - 4, ir.y + 3, m.shortcut, th.faint);
            if (m.enabled && clicked(ir)) hit = int(i);
            y += 13;
        }
        set_plane(saved);
        clips_.swap(saved_clips);
        if (key(PHX_KEY_ESCAPE)) close_popup();
        if (hit >= 0) close_popup();
        return hit;
    }

    // A dropdown: the field shows items[sel]; clicking opens a scrollable list. Returns true
    // when the selection changed.
    bool dropdown(uint32_t wid, const Rect& r, const std::vector<std::string>& items, int& sel,
                  const char* help = nullptr) {
        const bool h = hover(r);
        if (help) tip(r, help);
        rect(r, th.field, kSubWidget);
        frame_rect(r, popup_ == wid ? th.accent : h ? th.line : th.panel2, kSubWidget);
        const std::string cur = sel >= 0 && sel < int(items.size()) ? items[size_t(sel)] : std::string("-");
        text(r.x + 3, r.y + (r.h - 8) / 2 + 1, cur, th.text, kSubText, r.w - 16);
        icon(r.right() - 12, r.y + (r.h - kIconSize) / 2, kIconChevronDown, th.dim);
        if (clicked(r)) { if (popup_ == wid) close_popup(); else open_popup(wid, r); }
        if (popup_ != wid) return false;
        const int rows = std::min<int>(int(items.size()), 14);
        const int rw = std::max(r.w, [&] { int m = 0; for (auto& s : items) m = std::max(m, text_w(s)); return m + 16; }());
        Rect pr = place_popup(rw, rows * 12 + 4);
        const int saved = plane_;
        std::vector<Rect> saved_clips;
        saved_clips.swap(clips_);
        set_plane(popup_plane_);
        add_overlay(pr);
        popup_rect_ = pr;
        rect(pr, th.panel, kSubFill);
        frame_rect(pr, th.line, kSubWidget);
        wheel_scroll(pr, popup_scroll_, int(items.size()), rows, 1);
        bool changed = false;
        for (int i = 0; i < rows; ++i) {
            const int k = i + popup_scroll_;
            if (k >= int(items.size())) break;
            const Rect ir{ pr.x + 2, pr.y + 2 + i * 12, pr.w - 4, 12 };
            const bool hv = hover(ir);
            if (hv || k == sel) rect(ir, hv ? th.hover : th.panel2, kSubWidget);
            text(ir.x + 3, ir.y + 2, items[size_t(k)], k == sel ? th.accent : th.text, kSubText, ir.w - 6);
            if (clicked(ir)) { if (sel != k) { sel = k; changed = true; } close_popup(); }
        }
        set_plane(saved);
        clips_.swap(saved_clips);
        if (key(PHX_KEY_ESCAPE)) close_popup();
        return changed;
    }

    // ---- modal dialogs -------------------------------------------------------------------------
    // Everything below is blocked while a modal is drawn. Returns the content rect.
    Rect begin_modal(const std::string& title, int w, int h) {
        modal_saved_plane_ = plane_;
        modal_clips_.swap(clips_);
        clips_.clear();
        set_plane(kPlaneModal);
        stipple(Rect{ 0, 0, W, H }, th.bar, kSubBg);
        Rect r{ (W - w) / 2, (H - h) / 2, w, h };
        add_overlay(Rect{ 0, 0, W, H });
        rect(r, th.panel, kSubFill);
        frame_rect(r, th.accent, kSubWidget);
        Rect body = r;
        const Rect tr = cut_top(body, 16);
        rect(tr, th.panel2, kSubWidget);
        text(tr.x + 6, tr.y + 5, title, th.text);
        return body.inset(6);
    }
    void end_modal() { set_plane(modal_saved_plane_); clips_.swap(modal_clips_); }

    // Layout helper: a row of fixed-height cells, filled left to right.
    struct Row {
        Rect r; int x; int gap;
        Row(const Rect& rr, int g = 3) : r(rr), x(rr.x), gap(g) {}
        Rect take(int w) { Rect o{ x, r.y, std::min(w, r.right() - x), r.h }; x += w + gap; return o; }
        Rect rest() const { return Rect{ x, r.y, std::max(0, r.right() - x), r.h }; }
        Rect take_right(int w) { Rect o{ r.right() - w, r.y, w, r.h }; r.w -= w + gap; return o; }
    };

    // The shared single-line editor (only one field is focused at a time).
    LineEdit& edit() { return edit_; }

    // Process keys for the focused LineEdit. Returns true on Enter. Esc reverts + blurs.
    bool edit_keys(bool ro) {
        bool enter = false;
        for (KeyEvent& e : in->keys) {
            if (e.used) continue;
            const uint16_t m = norm_mods(e.mods);
            const bool sh = (m & kShift) != 0, ctl = (m & kCtrl) != 0;
            bool used = true;
            switch (e.key) {
            case PHX_KEY_ENTER:     enter = true; break;
            case PHX_KEY_ESCAPE:    blur(); break;
            case PHX_KEY_LEFT:      edit_.move_to(ctl ? edit_.word_left(edit_.caret) : (edit_.has_sel() && !sh ? edit_.sel_lo() : edit_.caret - 1), sh); break;
            case PHX_KEY_RIGHT:     edit_.move_to(ctl ? edit_.word_right(edit_.caret) : (edit_.has_sel() && !sh ? edit_.sel_hi() : edit_.caret + 1), sh); break;
            case PHX_KEY_HOME:      edit_.move_to(0, sh); break;
            case PHX_KEY_END:       edit_.move_to(int(edit_.buf.size()), sh); break;
            case PHX_KEY_BACKSPACE: if (!ro) edit_.backspace(ctl); break;
            case PHX_KEY_DELETE:    if (!ro) edit_.del(ctl); break;
            case PHX_KEY_TAB:       enter = true; break;
            default:
                if (ctl && e.key == 'a') edit_.select_all();
                else if (ctl && e.key == 'c') { if (edit_.has_sel()) phx_desktop_clipboard_set(edit_.selection().c_str()); }
                else if (ctl && e.key == 'x') { if (edit_.has_sel() && !ro) { phx_desktop_clipboard_set(edit_.selection().c_str()); edit_.delete_sel(); } }
                else if (ctl && e.key == 'v') { if (!ro) { std::string c = phx_desktop_clipboard_get(); const size_t nl = c.find_first_of("\r\n"); if (nl != std::string::npos) c.resize(nl); edit_.insert(c); } }
                else used = (e.key >= 32 && e.key < 127 && !ctl);   // swallow plain keys (typed via text)
                break;
            }
            if (used) e.used = true;
            if (enter || focus_ == 0) break;
        }
        if (focus_ != 0 && !ro && !in->text.empty()) { edit_.insert(in->text); in->text.clear(); }
        return enter;
    }

    bool just_pressed(uint32_t wid) const { return active_ == wid && drag_frame_ == frame; }

private:
    struct Overlay { Rect r; int plane; };

    static phx::vec2 v2(int x, int y) { return phx::vec2{ phx::s_from_int(x), phx::s_from_int(y) }; }

    // What native text must respect. Text is drawn last, over the whole upscaled canvas, so it has to
    // be hidden where the renderer would have drawn something ABOVE it: a rect/image on a higher
    // plane, or on a higher sub-layer of the same plane (the bars over a scrolling body). Only those
    // can ever matter, so the rest are not recorded. A stipple only dims (half strength).
    struct NGlyph { int x, y; unsigned char ch; int k; Rgba c; int plane; uint8_t sub; Rect clip; };
    struct Cover { Rect r; int plane; uint8_t sub; bool dim; };

    void note_cover(const Rect& r, uint8_t sub, bool dim) {
        if (!native_ || r.empty() || (plane_ == 0 && sub <= kSubText)) return;
        covers_.push_back(Cover{ r, plane_, sub, dim });
    }

    // Blend every queued glyph into the overlay (window pixels): coverage * colour alpha, minus the
    // covers above it. Straight-alpha "over", since glyph edges and neighbours can overlap.
    void flush_native_text() {
        if (!native_ || !raster_) { nglyphs_.clear(); covers_.clear(); return; }
        const int sc = ov_scale_;
        std::vector<const Cover*> above;
        for (const NGlyph& g : nglyphs_) {
            const RasterGlyph* rg = raster_->glyph(g.ch, g.k * sc);
            if (!rg) continue;
            const Rect vis = intersect(g.clip, Rect{ 0, 0, W, H });
            const int gx0 = std::max(g.x * sc + rg->left, vis.x * sc);
            const int gy0 = std::max((g.y + kGlyph * g.k) * sc + rg->top, vis.y * sc);
            const int gx1 = std::min(g.x * sc + rg->left + rg->w, vis.right() * sc);
            const int gy1 = std::min((g.y + kGlyph * g.k) * sc + rg->top + rg->h, vis.bottom() * sc);
            if (gx0 >= gx1 || gy0 >= gy1) continue;
            above.clear();
            for (const Cover& cv : covers_) {
                if (cv.plane < g.plane || (cv.plane == g.plane && cv.sub <= g.sub)) continue;
                if (cv.r.x * sc >= gx1 || cv.r.right() * sc <= gx0 || cv.r.y * sc >= gy1 || cv.r.bottom() * sc <= gy0) continue;
                above.push_back(&cv);
            }
            const uint32_t cr = phx::rgba_r(g.c), cg = phx::rgba_g(g.c), cb = phx::rgba_b(g.c), ca = phx::rgba_a(g.c);
            const int ax = g.x * sc + rg->left, ay = (g.y + kGlyph * g.k) * sc + rg->top;
            for (int y = gy0; y < gy1; ++y) {
                const uint8_t* row = rg->cov + size_t(y - ay) * size_t(rg->w);
                uint32_t* dst = ov_.pixels + size_t(y) * size_t(ov_.w);
                for (int x = gx0; x < gx1; ++x) {
                    uint32_t a = row[x - ax];
                    if (!a) continue;
                    if (ca != 255) a = a * ca / 255;
                    for (const Cover* cv : above)
                        if (x >= cv->r.x * sc && x < cv->r.right() * sc && y >= cv->r.y * sc && y < cv->r.bottom() * sc) {
                            if (cv->dim) a /= 2; else { a = 0; break; }
                        }
                    if (!a) continue;
                    const uint32_t d = dst[x], da = d >> 24;
                    if (da == 0) { dst[x] = (a << 24) | (cb << 16) | (cg << 8) | cr; continue; }
                    const uint32_t oa = a * 255 + da * (255 - a);                 // out alpha * 255
                    const uint32_t wn = a * 255, wo = da * (255 - a);              // weights of new / old colour
                    auto mixc = [&](uint32_t n, uint32_t o) { return (n * wn + o * wo + oa / 2) / oa; };
                    dst[x] = (((oa + 127) / 255) << 24) | (mixc(cb, (d >> 16) & 255) << 16)
                           | (mixc(cg, (d >> 8) & 255) << 8) | mixc(cr, d & 255);
                }
            }
        }
        nglyphs_.clear();
        covers_.clear();
    }

    void blit(const Rect& d, TextureId t, int sx, int sy, int sw, int sh, Rgba tint, uint8_t sub, uint16_t flags) {
        if (d.empty()) return;
        if (t != checker) note_cover(d, sub, false);      // (a stipple is noted once, whole, by stipple())
        phx::DrawSprite s{};
        s.tex = t;
        s.sx = int16_t(sx); s.sy = int16_t(sy); s.sw = int16_t(sw); s.sh = int16_t(sh);
        s.dw = int16_t(d.w); s.dh = int16_t(d.h);
        s.pos = v2(d.x + ox, d.y + oy);
        s.tint = tint;
        s.layer = layer(sub);
        s.z = 1;
        s.flags = flags;
        if (r_) r_->draw_sprite(s);
    }

    Rect place_popup(int w, int h) const {
        Rect pr{ popup_anchor_.x, popup_anchor_.bottom(), w, h };
        if (popup_at_mouse_) pr.y = popup_anchor_.y;
        if (pr.right() > W - 2) pr.x = std::max(2, W - 2 - w);
        if (pr.bottom() > H - 2) pr.y = popup_at_mouse_ ? std::max(2, H - 2 - h) : std::max(2, popup_anchor_.y - h);
        return pr;
    }

    int caret_from_x(int px) const {
        const int p = (px + kAdv / 2) / kAdv;
        return std::max(0, std::min(p, int(edit_.buf.size())));
    }

    void draw_tooltip() {
        const int w = std::min(W - 8, text_w(tip_text_) + 8);
        Rect r{ in->mx + 10, in->my + 14, w, 13 };
        if (r.right() > W - 2) r.x = std::max(2, W - 2 - w);
        if (r.bottom() > H - 2) r.y = std::max(2, in->my - 16);
        const int saved = plane_;
        set_plane(kPlaneTip);
        clips_.clear();
        rect(r, th.bar, kSubFill);
        frame_rect(r, th.line, kSubWidget);
        text(r.x + 4, r.y + 3, tip_text_, th.text, kSubText, r.w - 6);
        set_plane(saved);
    }

    TextRaster* raster_ = nullptr;
    bool native_ = false;
    phx_overlay ov_{};
    int  ov_scale_ = 1;
    std::vector<NGlyph> nglyphs_;
    std::vector<Cover> covers_;

    phx::UI ui_;
    phx::Renderer* r_ = nullptr;
    bool draw_ = false;
    int  plane_ = 0;
    std::vector<Rect> clips_;
    std::vector<Overlay> overlays_, prev_overlays_;
    int  modal_saved_plane_ = 0;
    std::vector<Rect> modal_clips_;
    std::vector<uint32_t> id_stack_;

    uint32_t active_ = 0, drag_btn_ = kMouseL;
    int drag_x0_ = 0, drag_y0_ = 0;
    uint64_t drag_frame_ = 0;
    int split_off_ = 0;
    int64_t scrub_v0_ = 0;
    bool scrub_moved_ = false;

    uint32_t focus_ = 0, edit_id_ = 0;
    bool focus_text_ = false, focus_seen_ = false, want_text_ = false, text_on_ = true;
    bool select_drag_ = false;
    LineEdit edit_;

    uint32_t popup_ = 0;
    Rect popup_anchor_{}, popup_rect_{};
    uint64_t popup_opened_ = 0;
    int popup_plane_ = 1;
    bool popup_at_mouse_ = false;
    int popup_scroll_ = 0;

    std::string tip_text_;
    Rect tip_rect_{}, tip_prev_rect_{};
    int tip_frames_ = 0, tip_mx_ = -1, tip_my_ = -1;

};

} // namespace twk
#endif // PHX_TOOLS_TWK_H
