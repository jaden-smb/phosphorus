// tests/suites/editors_test.cpp — the EDITORS suite: everything behind Phosphorus Studio and the
// standalone editors that can be proven without a window.
//
//   - the desktop seam extension over the null backend's scripted queue (phx/platform/desktop.h)
//   - the tool widget kit's logic, headless (twk.h: buttons, text fields, number fields,
//     dropdowns, menus, modal blocking, LineEdit)
//   - the code editor's document (textdoc.h) and lexers (syntax.h)
//   - the PNG writer round-tripped through the pipeline's own decoder (png_write.h / png.h)
//   - the sprite/pixel documents (pixeldoc.h), with every saved sprite def re-read by the BAKE's
//     loaders (builders.h) so "what the editor saves, phxsprite accepts" is a gate
//   - the new map/table model operations (tools/phxtmap/editor.h, tools/phxentity/editor.h),
//     again re-read by the bake (tiled_load, build_bin)
//   - the workspace helpers (project.h: kinds, tree, fuzzy match, compiler diagnostics) and the
//     zoom/pan canvas math (host.h)
//
// Self-registering PHX_TEST cases (tests/phx_test.h) with this binary's own runner. Fixtures go to
// literal build/ like the pipeline suite's.
#include "phx_test.h"

#include "phx/platform/desktop.h"
#include "phx/platform/gfx_soft.h"
#include "phx/render/renderer.h"
#include "phx/input/input.h"
#include "twk.h"
#include "ttf_text.h"                               // the Studio's TrueType text (tools/common)
#include "builders.h"                               // the bake's loaders (sprite defs, bin tables)
#include "tiled.h"
#include "png.h"
#include "png_write.h"
#include "editor.h"                                 // TmapDoc (tools/phxtmap on the include path)
#include "../../tools/phxentity/editor.h"           // BinDoc
#include "../../tools/phxstudio/textdoc.h"
#include "../../tools/phxstudio/syntax.h"
#include "../../tools/phxstudio/pixeldoc.h"
#include "../../tools/phxstudio/project.h"
#include "../../tools/phxstudio/host.h"
#include "../../tools/phxstudio/projectdoc.h"
#include "../../tools/phxstudio/components.h"
#include "../../tools/phxstudio/spawnart.h"
#include "../../tools/phxstudio/settings.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

extern "C" void phx_null_desktop_push(const phx_desktop_event* e);
extern "C" void phx_null_desktop_set_mouse(int x, int y, uint32_t buttons, uint16_t mods);
extern "C" const uint32_t* phx_null_overlay_peek(int* w, int* h);

using namespace phxstudio;

namespace {

phx_desktop_event ev(uint8_t kind) { phx_desktop_event e{}; e.kind = kind; return e; }

void drain_desktop() { phx_desktop_event e; while (phx_desktop_poll(&e)) {} }

bool write_file(const std::string& p, const std::string& s) {
    FILE* f = std::fopen(p.c_str(), "wb");
    if (!f) return false;
    std::fwrite(s.data(), 1, s.size(), f);
    std::fclose(f);
    return true;
}

// One headless GUI frame: begin (no renderer), run `body`, end.
template <class F> void frame(twk::Gui& g, twk::Input& in, F body) {
    g.begin(nullptr, nullptr, in, 640, 360);
    body();
    g.end();
    in.reset_frame();
}

} // namespace

// =============================================================================================
// desktop seam (null backend = scripted queue)
// =============================================================================================
PHX_TEST(desktop_queue_order_and_overflow) {
    drain_desktop();
    phx_desktop_event e = ev(PHX_DEV_KEY_DOWN);
    e.key = 'a'; e.mods = PHX_MOD_CTRL;
    phx_null_desktop_push(&e);
    e = ev(PHX_DEV_TEXT);
    std::snprintf(e.text, sizeof(e.text), "hi");
    phx_null_desktop_push(&e);
    phx_desktop_event o;
    CHECK(phx_desktop_poll(&o) == 1 && o.kind == PHX_DEV_KEY_DOWN && o.key == 'a' && o.mods == PHX_MOD_CTRL);
    CHECK(phx_desktop_poll(&o) == 1 && o.kind == PHX_DEV_TEXT && std::strcmp(o.text, "hi") == 0);
    CHECK(phx_desktop_poll(&o) == 0);                          // empty
    // bounded ring: 300 pushes keep the NEWEST 256
    for (int i = 0; i < 300; ++i) { e = ev(PHX_DEV_KEY_DOWN); e.key = i; phx_null_desktop_push(&e); }
    int n = 0, first = -1;
    while (phx_desktop_poll(&o)) { if (first < 0) first = o.key; ++n; }
    CHECK_EQ(n, 256);
    CHECK_EQ(first, 300 - 256);
}

PHX_TEST(desktop_clipboard_mouse_and_available) {
    phx_desktop_clipboard_set("copied text");
    CHECK(std::strcmp(phx_desktop_clipboard_get(), "copied text") == 0);
    phx_null_desktop_set_mouse(12, 34, 1u << PHX_MOUSE_RIGHT, PHX_MOD_SHIFT);
    int x = 0, y = 0;
    uint32_t b = 0;
    phx_desktop_mouse(&x, &y, &b);
    CHECK(x == 12 && y == 34 && b == (1u << PHX_MOUSE_RIGHT));
    CHECK(phx_desktop_mods() == PHX_MOD_SHIFT);
    CHECK(phx_desktop_available() == 0);                       // null = scripted, no window
    phx_null_desktop_set_mouse(-1, -1, 0, 0);
}

PHX_TEST(twk_collect_desktop_translates_events) {
    drain_desktop();
    phx_desktop_event e = ev(PHX_DEV_MOUSE_DOWN);
    e.button = PHX_MOUSE_LEFT; e.clicks = 2;
    phx_null_desktop_push(&e);
    e = ev(PHX_DEV_MOUSE_UP); e.button = PHX_MOUSE_LEFT;
    phx_null_desktop_push(&e);
    e = ev(PHX_DEV_WHEEL); e.wheel_y = -2;
    phx_null_desktop_push(&e);
    e = ev(PHX_DEV_TEXT);
    std::snprintf(e.text, sizeof(e.text), "a\xC3\xA9z");          // UTF-8 e-acute is filtered (ASCII fonts)
    phx_null_desktop_push(&e);
    e = ev(PHX_DEV_KEY_DOWN); e.key = PHX_KEY_ENTER;
    phx_null_desktop_push(&e);
    e = ev(PHX_DEV_QUIT);
    phx_null_desktop_push(&e);
    phx_null_desktop_set_mouse(5, 6, 0, 0);
    twk::Input in;
    twk::collect_desktop(in);
    CHECK((in.pressed & twk::kMouseL) && (in.released & twk::kMouseL) && in.clicks == 2);
    CHECK_EQ(in.wheel_y, -2);
    CHECK(in.text == "az");
    CHECK(in.keys.size() == 1 && in.keys[0].key == PHX_KEY_ENTER);
    CHECK(in.quit);
    CHECK(in.mx == 5 && in.my == 6);
    phx_null_desktop_set_mouse(-1, -1, 0, 0);
}

// =============================================================================================
// widget kit (headless)
// =============================================================================================
PHX_TEST(twk_geometry_and_cut) {
    twk::Rect r{ 0, 0, 100, 50 };
    const twk::Rect l = twk::cut_left(r, 20);
    const twk::Rect t = twk::cut_top(r, 10);
    CHECK(l == (twk::Rect{ 0, 0, 20, 50 }) && t == (twk::Rect{ 20, 0, 80, 10 }) && r == (twk::Rect{ 20, 10, 80, 40 }));
    CHECK(twk::intersect(twk::Rect{ 0, 0, 10, 10 }, twk::Rect{ 5, 5, 10, 10 }) == (twk::Rect{ 5, 5, 5, 5 }));
    CHECK(twk::intersect(twk::Rect{ 0, 0, 10, 10 }, twk::Rect{ 20, 20, 5, 5 }).empty());
    CHECK_EQ(twk::clamp_scroll(50, 10, 4), 6);
    const twk::Rect f = twk::fit_rect(16, 8, twk::Rect{ 0, 0, 100, 100 }, 8);
    CHECK(f.w == 96 && f.h == 48);                                // 6x integer upscale, centred
}

PHX_TEST(twk_button_and_modal_blocking) {
    twk::Gui g;
    twk::Input in;
    const twk::Rect b{ 10, 10, 50, 12 };
    int hits = 0;
    in.click_at(20, 15);
    frame(g, in, [&] { if (g.button(b, "ok")) ++hits; });
    CHECK_EQ(hits, 1);
    // a modal drawn this frame blocks the button beneath it from the NEXT frame on
    in.mx = 20; in.my = 15;
    frame(g, in, [&] {
        (void)g.button(b, "ok");
        (void)g.begin_modal("dialog", 200, 100);
        g.end_modal();
    });
    in.click_at(20, 15);
    frame(g, in, [&] { if (g.button(b, "ok")) ++hits; (void)g.begin_modal("dialog", 200, 100); g.end_modal(); });
    CHECK_EQ(hits, 1);                                              // swallowed by the modal
    // without the modal the click lands again
    in.mx = -1; in.my = -1;
    frame(g, in, [&] {});
    in.click_at(20, 15);
    frame(g, in, [&] { if (g.button(b, "ok")) ++hits; });
    CHECK_EQ(hits, 2);
}

PHX_TEST(twk_text_field_edit_commit_and_escape) {
    twk::Gui g;
    twk::Input in;
    const uint32_t id = 77;
    const twk::Rect r{ 0, 0, 120, 13 };
    std::string s = "abc";
    in.click_at(200, 5);                                            // (outside)
    frame(g, in, [&] { g.text_field(id, r, s); });
    CHECK(g.focus() == 0);
    in.click_at(100, 5);                                            // click inside: focus, caret at the end
    frame(g, in, [&] { g.text_field(id, r, s); });
    CHECK(g.focus() == id && g.text_focus());
    in.type("de");
    in.key(PHX_KEY_BACKSPACE);                                      // keys run before the typed text
    bool changed = false;
    frame(g, in, [&] { changed = g.text_field(id, r, s); });
    CHECK(!changed && s == "abc");                                  // not committed yet
    CHECK(g.edit().buf == "abde");
    in.key(PHX_KEY_ENTER);
    frame(g, in, [&] { changed = g.text_field(id, r, s); });
    CHECK(changed && s == "abde" && g.focus() == 0);
    // Esc reverts
    in.click_at(100, 5);
    frame(g, in, [&] { g.text_field(id, r, s); });
    in.type("zzz");
    frame(g, in, [&] { g.text_field(id, r, s); });
    in.key(PHX_KEY_ESCAPE);
    frame(g, in, [&] { changed = g.text_field(id, r, s); });
    CHECK(!changed && s == "abde" && g.focus() == 0);
    // live fields write every keystroke
    std::string q;
    in.click_at(10, 5);
    frame(g, in, [&] { g.text_field(9, r, q, "find", twk::kFieldLive); });
    in.type("x");
    frame(g, in, [&] { changed = g.text_field(9, r, q, "find", twk::kFieldLive); });
    CHECK(changed && q == "x");
}

PHX_TEST(twk_line_edit_ops) {
    twk::LineEdit e;
    e.set("hello world_2 x", false);
    e.move_to(e.word_left(e.caret), false);
    CHECK_EQ(e.caret, 14);                                          // before "x"
    e.move_to(e.word_left(e.caret), false);
    CHECK_EQ(e.caret, 6);                                           // before "world_2" (underscore = word)
    e.move_to(e.word_right(e.caret), true);
    CHECK(e.selection() == "world_2");
    e.insert("W");
    CHECK(e.buf == "hello W x");
    e.backspace(true);
    CHECK(e.buf == "hello  x");
    e.select_all(); e.insert("new\x01text");                        // control chars are dropped
    CHECK(e.buf == "newtext");
    e.select_word_at(1);
    CHECK(e.selection() == "newtext");
}

PHX_TEST(twk_int_field_type_wheel_and_clamp) {
    twk::Gui g;
    twk::Input in;
    const twk::Rect r{ 0, 0, 60, 13 };
    int64_t v = 5;
    in.mx = 10; in.my = 5; in.wheel_y = 3;
    frame(g, in, [&] { g.int_field(3, r, v, 0, 7, 1); });
    CHECK_EQ(v, 7);                                                 // wheel, clamped to the range
    // a click without dragging switches to typing; typed values clamp too
    in.mx = 10; in.my = 5; in.pressed = twk::kMouseL; in.held = twk::kMouseL;
    frame(g, in, [&] { g.int_field(3, r, v, 0, 7, 1); });
    in.mx = 10; in.my = 5; in.released = twk::kMouseL; in.held = 0;
    frame(g, in, [&] { g.int_field(3, r, v, 0, 7, 1); });
    CHECK(g.focus() == 3);
    in.type("99"); in.key(PHX_KEY_ENTER);
    bool ch = false;
    frame(g, in, [&] { ch = g.int_field(3, r, v, 0, 7, 1); });
    CHECK(!ch || v == 7);
    CHECK_EQ(v, 7);
    in.click_at(10, 5);
    frame(g, in, [&] { g.int_field(3, r, v, -100, 100, 1); });
    in.released = twk::kMouseL;
    frame(g, in, [&] { g.int_field(3, r, v, -100, 100, 1); });
    in.type("-42"); in.key(PHX_KEY_ENTER);
    frame(g, in, [&] { g.int_field(3, r, v, -100, 100, 1); });
    CHECK_EQ(v, -42);
}

PHX_TEST(twk_dropdown_and_menu) {
    twk::Gui g;
    twk::Input in;
    const std::vector<std::string> items = { "red", "green", "blue" };
    int sel = 0;
    const twk::Rect r{ 10, 10, 80, 12 };
    in.click_at(20, 15);                                            // open
    frame(g, in, [&] { g.dropdown(5, r, items, sel); });
    CHECK(g.popup_open(5));
    bool changed = false;
    frame(g, in, [&] { changed = g.dropdown(5, r, items, sel); });  // draws the list (rows at y 24..)
    in.click_at(20, 24 + 12 + 4);                                   // the second row
    frame(g, in, [&] { changed = g.dropdown(5, r, items, sel); });
    CHECK(changed && sel == 1 && !g.popup_open(5));
    // a menu: an outside click closes it and is swallowed
    g.open_popup(9, twk::Rect{ 100, 100, 30, 12 });
    int hit = -2, under = 0;
    frame(g, in, [&] { hit = g.menu(9, { twk::MenuItem{ "one" }, twk::MenuItem{ "two" } }); });
    CHECK_EQ(hit, -1);
    in.click_at(400, 300);
    frame(g, in, [&] { if (g.clicked(twk::Rect{ 390, 290, 30, 30 })) ++under; hit = g.menu(9, { twk::MenuItem{ "one" } }); });
    CHECK(!g.popup_open(9) && under == 0);
    // choosing an item returns its index and closes
    g.open_popup(9, twk::Rect{ 100, 100, 30, 12 });
    frame(g, in, [&] { g.menu(9, { twk::MenuItem{ "one" }, twk::MenuItem{ "two" } }); });
    in.click_at(110, 112 + 2 + 13 + 3);                             // row 2
    frame(g, in, [&] { hit = g.menu(9, { twk::MenuItem{ "one" }, twk::MenuItem{ "two" } }); });
    CHECK_EQ(hit, 1);
    // Esc closes an open menu even when an editor's Esc hotkey (clear the selection) runs first
    g.open_context(9);
    bool canvas_esc = false;
    in.key(PHX_KEY_ESCAPE);
    frame(g, in, [&] { canvas_esc = g.hotkey(PHX_KEY_ESCAPE); hit = g.menu(9, { twk::MenuItem{ "one" } }); });
    CHECK(!canvas_esc && hit == -1 && !g.popup_open(9));
    in.key(PHX_KEY_ESCAPE);                                         // no menu: the hotkey gets it
    frame(g, in, [&] { canvas_esc = g.hotkey(PHX_KEY_ESCAPE); });
    CHECK(canvas_esc);
}

PHX_TEST(twk_key_matching_folds_cmd_into_ctrl) {
    twk::Gui g;
    twk::Input in;
    in.key('s', PHX_MOD_GUI);                                       // Cmd+S on a Mac
    bool saved = false;
    frame(g, in, [&] { saved = g.key('s', twk::kCtrl); });
    CHECK(saved);
    in.key('s');
    frame(g, in, [&] { saved = g.key('s', twk::kCtrl); });
    CHECK(!saved);                                                  // exact modifiers
}

// =============================================================================================
// native-resolution text: the TrueType rasterizer + twk::Gui's overlay path
// =============================================================================================
PHX_TEST(ttf_text_rasterizes_on_the_canvas_grid) {
    phxtool::TtfText t;
    CHECK(t.ok());
    for (int scale = 1; scale <= 4; ++scale) {
        const twk::RasterGlyph* a = t.glyph('A', scale);
        CHECK(a && a->cov && a->w > 0 && a->h > 0);
        if (!a) continue;
        // em = 10 canvas px: a capital is ~7.3 px tall, sits on the baseline, and fits the 6 px advance
        CHECK(a->h >= 7 * scale - 1 && a->h <= 8 * scale + 1);
        CHECK(a->top < 0 && a->top + a->h <= 1);
        CHECK(a->left >= 0 && a->left + a->w <= twk::kAdv * scale);
        int lit = 0;
        for (int i = 0; i < a->w * a->h; ++i) lit += a->cov[i] != 0;
        CHECK(lit > a->w);                                          // has ink, and is anti-aliased (not a box)
        const twk::RasterGlyph* g = t.glyph('g', scale);
        CHECK(g && g->top + g->h > 1);                              // descenders drop below the baseline
    }
    CHECK(t.glyph(' ', 2) == nullptr);                              // nothing to draw
    CHECK(t.glyph(200, 2) != nullptr && t.glyph(-5, 2) != nullptr); // outside ASCII: the missing-glyph box
    CHECK(t.glyph('A', 2) == t.glyph('A', 2));                      // cached
    CHECK(t.glyph('A', 0) == nullptr);
}

namespace {
// Lit-pixel count and max alpha of the null backend's overlay inside a window-pixel rect.
struct Ink { int lit = 0; int max_a = 0; };
Ink overlay_ink(int x0, int y0, int x1, int y1) {
    int w = 0, h = 0;
    const uint32_t* px = phx_null_overlay_peek(&w, &h);
    Ink k;
    if (!px) return k;
    for (int y = std::max(0, y0); y < std::min(h, y1); ++y)
        for (int x = std::max(0, x0); x < std::min(w, x1); ++x) {
            const int a = int(px[size_t(y) * size_t(w) + size_t(x)] >> 24);
            if (a) { ++k.lit; k.max_a = std::max(k.max_a, a); }
        }
    return k;
}
} // namespace

PHX_TEST(twk_native_text_draws_at_window_resolution_and_respects_covers) {
    const phx_platform* plat = phx_platform_get();
    phx_platform_desc desc{};
    desc.title = "editors_test"; desc.width = 128; desc.height = 64;
    CHECK_EQ(plat->init(&desc), 0);
    phx_desktop_set_scale(2);
    static uint8_t arena_buf[8 << 20];
    phx::ArenaAllocator arena;
    arena.init(arena_buf, sizeof(arena_buf));
    auto rr = phx::Renderer::create(plat->gfx(), arena, phx::caps());
    CHECK(rr.ok());
    if (!rr.ok()) { plat->shutdown(); return; }
    phx::Renderer* r = rr.unwrap();

    phxtool::TtfText ttf;
    twk::Gui g;
    twk::Input in;
    phx::InputState app_in{};
    g.init(*r);
    const phx::Rgba white = phx::rgba(255, 255, 255);
    const phx::Rgba dark = phx::rgba(20, 20, 30);
    auto draw = [&](int w, int h, auto body) {
        r->begin_frame(phx::Camera2D{});
        g.begin(r, &app_in, in, w, h);
        body();
        g.end();
        r->end_frame();
        in.reset_frame();
    };

    // no rasterizer set: the bitmap font, no overlay use
    draw(128, 64, [&] { CHECK(!g.native_text()); });

    g.set_text_raster(&ttf);
    // "HI" at canvas (4,4): window (scale 2) cells x 8..32, baseline y = (4+8)*2 = 24, no descenders
    draw(128, 64, [&] { CHECK(g.native_text()); g.text(4, 4, "HI", white); });
    int w = 0, h = 0;
    CHECK(phx_null_overlay_peek(&w, &h) != nullptr && w == 256 && h == 128);   // one pixel per WINDOW pixel
    const Ink text = overlay_ink(0, 0, 256, 128);
    CHECK(text.lit > 40 && text.max_a == 255);
    CHECK_EQ(overlay_ink(8, 8, 32, 26).lit, text.lit);              // nothing outside the cells / below the baseline

    // the clip rect trims glyphs in window pixels: a 6 px (canvas) clip keeps only the start of the 'H'
    draw(128, 64, [&] { g.push_clip(twk::Rect{ 0, 0, 6, 64 }); g.text(4, 4, "HI", white); g.pop_clip(); });
    const Ink clipped = overlay_ink(0, 0, 256, 128);
    CHECK(clipped.lit > 0 && clipped.lit < text.lit / 2 && overlay_ink(12, 0, 256, 128).lit == 0);

    // an opaque rect on a HIGHER plane hides the text under it, with no leftover pixels
    draw(128, 64, [&] {
        g.text(4, 4, "HI", white);
        g.set_plane(1);
        g.rect(twk::Rect{ 0, 0, 20, 64 }, dark, twk::kSubFill);
        g.set_plane(0);
    });
    CHECK_EQ(overlay_ink(0, 0, 256, 128).lit, 0);
    // ... text ON that plane, above the rect's sub-layer, is shown
    draw(128, 64, [&] {
        g.set_plane(1);
        g.rect(twk::Rect{ 0, 0, 20, 64 }, dark, twk::kSubFill);
        g.text(4, 4, "HI", white);
        g.set_plane(0);
    });
    CHECK_EQ(overlay_ink(0, 0, 256, 128).lit, text.lit);
    // same plane: a rect on a HIGHER sub-layer (a bar over a scrolling body) covers the text too ...
    draw(128, 64, [&] {
        g.text(4, 4, "HI", white);
        g.rect(twk::Rect{ 0, 0, 20, 64 }, dark, twk::kSubTop);
    });
    CHECK_EQ(overlay_ink(0, 0, 256, 128).lit, 0);
    // ... and one on a LOWER sub-layer (a panel behind it) does not
    draw(128, 64, [&] {
        g.text(4, 4, "HI", white);
        g.rect(twk::Rect{ 0, 0, 20, 64 }, dark, twk::kSubFill);
    });
    CHECK_EQ(overlay_ink(0, 0, 256, 128).lit, text.lit);
    // only the covered part goes: a rect over the 'I' (canvas x >= 10) leaves the 'H'
    draw(128, 64, [&] {
        g.text(4, 4, "HI", white);
        g.set_plane(1);
        g.rect(twk::Rect{ 10, 0, 20, 64 }, dark, twk::kSubFill);
        g.set_plane(0);
    });
    const Ink half = overlay_ink(0, 0, 256, 128);
    CHECK(half.lit > 0 && half.lit < text.lit && overlay_ink(20, 0, 256, 128).lit == 0);

    // a modal's stipple backdrop dims what is under it to half strength
    draw(128, 64, [&] {
        g.text(4, 4, "HI", white);
        g.begin_modal("m", 60, 30);                                 // centred: x 34..94, clear of the text
        g.end_modal();
    });
    const Ink dimmed = overlay_ink(8, 8, 32, 26);
    CHECK(dimmed.lit == text.lit && dimmed.max_a > 0 && dimmed.max_a <= 128);

    // a canvas size that disagrees with the overlay (mid-resize) falls back to the bitmap font
    draw(100, 64, [&] { CHECK(!g.native_text()); });

    g.set_text_raster(nullptr);
    draw(128, 64, [&] { CHECK(!g.native_text()); });
    plat->shutdown();
}

// =============================================================================================
// TextDoc
// =============================================================================================
PHX_TEST(textdoc_load_save_round_trip) {
    TextDoc d = TextDoc::from_text("a\r\nb\r\n");
    CHECK(d.crlf && d.final_newline && d.line_count() == 2);
    CHECK(d.to_text() == "a\r\nb\r\n");
    TextDoc e = TextDoc::from_text("x\ny");
    CHECK(!e.final_newline && e.to_text() == "x\ny");
    TextDoc z = TextDoc::from_text("");
    CHECK(z.line_count() == 1 && z.to_text().empty());
    CHECK(!d.dirty);
}

PHX_TEST(textdoc_typing_undo_groups_and_dirty) {
    TextDoc d = TextDoc::from_text("int x;\n");
    d.move_to(TextPos{ 0, 3 }, false);
    for (char c : std::string(" foo")) d.insert(std::string(1, c));
    CHECK(d.line(0) == "int foo x;");
    CHECK(d.dirty);
    CHECK(d.undo());                                                // the word " foo" is one step
    CHECK(d.line(0) == "int x;");
    CHECK(!d.dirty);                                                // back at the saved state
    CHECK(d.redo() && d.line(0) == "int foo x;");
    d.mark_saved();
    d.insert("!");
    CHECK(d.dirty);
    d.undo();
    CHECK(!d.dirty);
}

PHX_TEST(textdoc_newline_indent_and_braces) {
    TextDoc d = TextDoc::from_text("    if (x) {}\n");
    d.move_to(TextPos{ 0, 12 }, false);                             // between { and }
    d.newline();
    CHECK(d.line_count() == 3);
    CHECK(d.line(1) == "        ");                                 // one level deeper
    CHECK(d.line(2) == "    }");                                    // closer at the outer indent
    CHECK(d.caret == (TextPos{ 1, 8 }));
    d.backspace();                                                  // back to the previous indent stop
    CHECK(d.line(1) == "    ");
}

PHX_TEST(textdoc_tab_comment_duplicate_move) {
    TextDoc d = TextDoc::from_text("a\nb\nc\n");
    d.anchor = TextPos{ 0, 0 }; d.caret = TextPos{ 1, 1 };
    d.tab(false);
    CHECK(d.line(0) == "    a" && d.line(1) == "    b" && d.line(2) == "c");
    d.tab(true);
    CHECK(d.line(0) == "a" && d.line(1) == "b");
    d.toggle_comment("//");
    CHECK(d.line(0) == "// a" && d.line(1) == "// b");
    d.toggle_comment("//");
    CHECK(d.line(0) == "a" && d.line(1) == "b");
    d.move_to(TextPos{ 2, 0 }, false);
    d.duplicate_lines();
    CHECK(d.line_count() == 4 && d.line(3) == "c");
    d.move_to(TextPos{ 0, 0 }, false);
    d.move_lines(1);
    CHECK(d.line(0) == "b" && d.line(1) == "a" && d.caret.line == 1);
    d.delete_lines();
    CHECK(d.line_count() == 3 && d.line(1) == "c");
}

PHX_TEST(textdoc_find_replace) {
    TextDoc d = TextDoc::from_text("foo Foo food\nbar foo\n");
    CHECK_EQ(d.count("foo", false, false), 4);
    CHECK_EQ(d.count("foo", true, false), 3);
    CHECK_EQ(d.count("foo", false, true), 3);                       // whole word: not "food"
    CHECK(d.find("bar", true, false, false) && d.selected_text() == "bar");
    CHECK(d.find("foo", true, true, false) && d.sel_lo() == (TextPos{ 1, 4 }));
    CHECK(d.find("foo", true, true, false) && d.sel_lo() == (TextPos{ 0, 0 }));   // wraps
    const int k = d.replace_all("foo", "x", false, true);
    CHECK_EQ(k, 3);
    CHECK(d.line(0) == "x x food" && d.line(1) == "bar x");
    CHECK(d.undo() && d.line(0) == "foo Foo food");                 // replace-all is one step
}

PHX_TEST(textdoc_utf8_and_tabs) {
    TextDoc d = TextDoc::from_text("a\xE2\x80\x94" "b\n\tx\n");     // a EM-DASH b
    CHECK_EQ(d.display_col(0, 5), 3);                               // 5 bytes, 3 columns
    d.move_to(TextPos{ 0, 1 }, false);
    d.right(false, false);
    CHECK_EQ(d.caret.col, 4);                                       // over the whole code point
    d.left(false, false);
    CHECK_EQ(d.caret.col, 1);
    d.move_to(TextPos{ 0, 4 }, false);
    d.backspace();
    CHECK(d.line(0) == "ab");                                       // deleted the code point
    CHECK_EQ(d.display_col(1, 1), 4);                               // tab to the next stop
    CHECK_EQ(d.col_from_display(1, 3), 1);                          // nearer the tab's right edge
    CHECK_EQ(d.col_from_display(1, 1), 0);
}

// =============================================================================================
// syntax
// =============================================================================================
PHX_TEST(syntax_cpp_tokens_and_block_state) {
    std::vector<Span> sp;
    auto tok_at = [&](int col) { for (const Span& s : sp) if (col >= s.start && col < s.start + s.len) return s.tok; return Tok::Text; };
    int st = highlight_line(Lang::Cpp, "int x = 42; // hi", 0, sp);
    CHECK(st == 0 && tok_at(0) == Tok::Type && tok_at(8) == Tok::Number && tok_at(13) == Tok::Comment);
    st = highlight_line(Lang::Cpp, "return \"s\" /* open", 0, sp);
    CHECK(st == 1 && tok_at(0) == Tok::Keyword && tok_at(7) == Tok::String && tok_at(12) == Tok::Comment);
    st = highlight_line(Lang::Cpp, "still */ foo(1)", st, sp);
    CHECK(st == 0 && tok_at(0) == Tok::Comment && tok_at(9) == Tok::Func);
    highlight_line(Lang::Cpp, "#include <vector>", 0, sp);
    CHECK(tok_at(0) == Tok::Preproc && tok_at(10) == Tok::String);
    highlight_line(Lang::Cpp, "PHX_ASSERT(kMax)", 0, sp);
    CHECK(tok_at(0) == Tok::Constant && tok_at(11) == Tok::Constant);
}

PHX_TEST(syntax_other_languages) {
    std::vector<Span> sp;
    auto tok_at = [&](int col) { for (const Span& s : sp) if (col >= s.start && col < s.start + s.len) return s.tok; return Tok::Text; };
    highlight_line(Lang::Json, "{ \"hp\": 3, \"n\": \"x\", \"ok\": true }", 0, sp);
    CHECK(tok_at(2) == Tok::Key && tok_at(8) == Tok::Number && tok_at(16) == Tok::String && tok_at(27) == Tok::Keyword);
    highlight_line(Lang::Make, "studio: $(OBJ) # build", 0, sp);
    CHECK(tok_at(0) == Tok::Func && tok_at(8) == Tok::Variable && tok_at(16) == Tok::Comment);
    int st = highlight_line(Lang::Python, "x = \"\"\"doc", 0, sp);
    CHECK_EQ(st, 2);
    st = highlight_line(Lang::Python, "end\"\"\" # c", st, sp);
    CHECK(st == 0 && tok_at(0) == Tok::String && tok_at(7) == Tok::Comment);
    st = highlight_line(Lang::Markdown, "```cpp", 0, sp);
    CHECK_EQ(st, 4);
    highlight_line(Lang::Markdown, "# Title", 0, sp);
    CHECK(tok_at(2) == Tok::Heading);
    highlight_line(Lang::Sprdef, "clip walk 0 4 10 1", 0, sp);
    CHECK(tok_at(0) == Tok::Keyword && tok_at(10) == Tok::Number);
    CHECK(lang_for_path("a/b/Makefile") == Lang::Make && lang_for_path("x.HPP") == Lang::Cpp &&
          lang_for_path("t.tmj") == Lang::Json && lang_for_path("hero.sprdef") == Lang::Sprdef);
    CHECK(std::string(comment_prefix(Lang::Cpp)) == "//" && std::string(comment_prefix(Lang::Json)).empty());
}

PHX_TEST(syntax_bracket_matching) {
    const std::vector<std::string> lines = { "f(a[1], {", "  x", "})" };
    int ml = -1, mc = -1;
    CHECK(match_bracket(lines, 0, 1, ml, mc) && ml == 2 && mc == 1);
    CHECK(match_bracket(lines, 0, 8, ml, mc) && ml == 2 && mc == 0);
    CHECK(match_bracket(lines, 2, 0, ml, mc) && ml == 0 && mc == 8);
    CHECK(!match_bracket(lines, 1, 2, ml, mc));
}

// =============================================================================================
// PNG writer
// =============================================================================================
PHX_TEST(png_write_round_trips_indexed_and_rgba) {
    // few colours -> indexed; transparent pixels come back as 0
    std::vector<uint32_t> px(size_t(37) * 19);
    for (size_t i = 0; i < px.size(); ++i) px[i] = (i % 7 == 0) ? 0x00123456u : (i % 3 ? 0xFF2040E0u : 0x80FFFFFFu);
    std::vector<uint8_t> png = phxtool::png_encode(px.data(), 37, 19);
    CHECK(png.size() > 8 && png[25] == 3);                          // IHDR colour type 3 (indexed)
    std::vector<uint32_t> back;
    uint16_t w = 0, h = 0;
    CHECK(phxtool::png_decode(png.data(), png.size(), back, w, h) && w == 37 && h == 19);
    bool same = back.size() == px.size();
    for (size_t i = 0; same && i < px.size(); ++i) same = back[i] == ((px[i] >> 24) ? px[i] : 0u);
    CHECK(same);
    // many colours -> RGBA with filters
    std::vector<uint32_t> g(size_t(64) * 48);
    for (int y = 0; y < 48; ++y) for (int x = 0; x < 64; ++x) g[size_t(y) * 64 + size_t(x)] = px_rgba(x * 4, y * 5, (x * y) & 255, 255);
    png = phxtool::png_encode(g.data(), 64, 48);
    CHECK(png[25] == 6);
    CHECK(phxtool::png_decode(png.data(), png.size(), back, w, h) && back == g);
    CHECK(png.size() < g.size() * 4);                               // actually compressed
    // deterministic
    CHECK(phxtool::png_encode(g.data(), 64, 48) == png);
}

// =============================================================================================
// PixelDoc / SprDoc
// =============================================================================================
PHX_TEST(pixeldoc_tools) {
    const uint32_t R = px_rgba(255, 0, 0), B = px_rgba(0, 0, 255);
    PixelDoc d = PixelDoc::blank(16, 16);
    d.dab(5, 5, R, 3);
    int n = 0;
    for (uint32_t p : d.px) n += p == R;
    CHECK_EQ(n, 9);
    d = PixelDoc::blank(16, 16);
    d.line(0, 0, 15, 15, R);
    CHECK(d.get(7, 7) == R && d.get(15, 15) == R && d.get(1, 0) == 0u);
    d = PixelDoc::blank(8, 8);
    d.rect(1, 1, 6, 4, R, false);
    CHECK(d.get(1, 1) == R && d.get(6, 4) == R && d.get(3, 3) == 0u);
    d.rect(1, 1, 6, 4, R, true);
    CHECK(d.get(3, 3) == R);
    CHECK_EQ(d.fill(0, 0, B, true), 64 - 24);                       // the connected outside
    CHECK_EQ(d.fill(3, 3, B, false), 24);                           // global replace of red
    d = PixelDoc::blank(9, 9);
    d.ellipse(0, 0, 8, 8, R, false);
    CHECK(d.get(4, 0) == R && d.get(0, 4) == R && d.get(8, 4) == R && d.get(4, 8) == R && d.get(4, 4) == 0u);
    for (int y = 0; y < 9; ++y) for (int x = 0; x < 9; ++x) CHECK(d.get(x, y) == d.get(8 - x, y));   // symmetric
}

PHX_TEST(pixeldoc_regions_transforms_undo) {
    const uint32_t R = px_rgba(255, 0, 0), G = px_rgba(0, 255, 0);
    PixelDoc d = PixelDoc::blank(4, 4);
    d.set(0, 0, R); d.set(1, 0, G);
    d.push_undo();
    d.flip_h(0, 0, 4, 4);
    CHECK(d.get(3, 0) == R && d.get(2, 0) == G);
    d.flip_v(0, 0, 4, 4);
    CHECK(d.get(3, 3) == R);
    CHECK(d.undo() && d.get(0, 0) == R && d.get(3, 3) == 0u);
    CHECK(d.redo() && d.get(3, 3) == R);
    d = PixelDoc::blank(4, 4);
    d.set(0, 0, R);
    CHECK(d.rotate90(0, 0, 4, true) && d.get(3, 0) == R);
    d.shift(0, 0, 4, 4, 1, 1);                                      // wraps around
    CHECK(d.get(0, 1) == R);
    PixelDoc::Region reg = d.copy(0, 1, 2, 2);
    d.clear_rect(0, 0, 4, 4);
    d.paste(reg, 2, 2, true);
    CHECK(d.get(2, 2) == R);
    d.resize(6, 6, 1, 1);                                           // centred: (2,2) -> (3,3)
    CHECK(d.w == 6 && d.get(3, 3) == R);
    d.scale_up(2);
    CHECK(d.w == 12 && d.get(6, 6) == R && d.get(7, 7) == R);
    d.push_undo();
    CHECK(d.same_as_last_snapshot());
    d.set(0, 0, G);
    CHECK(!d.same_as_last_snapshot());
}

PHX_TEST(pixeldoc_gba_checks_and_colour_math) {
    CHECK(snap555(px_rgba(255, 128, 7)) == px_rgba(255, 132, 0));
    CHECK(snap555(snap555(px_rgba(90, 91, 92))) == snap555(px_rgba(90, 91, 92)));
    uint32_t c = 0;
    CHECK(parse_hex_rgb("#1A2B3C", c) && c == px_rgba(0x1A, 0x2B, 0x3C) && hex_rgb(c) == "1A2B3C");
    CHECK(!parse_hex_rgb("xyz", c));
    int h, s, v;
    rgb_to_hsv(hsv_to_rgb(120, 255, 255), h, s, v);
    CHECK(h == 120 && s == 255 && v == 255);
    PixelDoc d = PixelDoc::blank(16, 8);
    for (int i = 0; i < 20; ++i) d.set(i % 8, i / 8, px_rgba(i * 12, 0, 0));   // 20 colours in tile 0
    CHECK(d.tile_colors(0, 0) == 20 && d.tile_colors(1, 0) == 0);
    CHECK_EQ(d.tiles_over_budget(), 1);
    CHECK_EQ(int(d.unique_colors().size()), 20);
}

PHX_TEST(pixeldoc_png_file_round_trip) {
    PixelDoc d = PixelDoc::blank(24, 10);
    d.rect(2, 2, 20, 7, px_rgba(10, 200, 30), true);
    std::string err;
    CHECK(d.save_png("build/e_pixel.png", &err));
    PixelDoc r;
    CHECK(PixelDoc::load_png("build/e_pixel.png", r, &err) && r.w == 24 && r.h == 10 && r.px == d.px && !r.dirty);
}

PHX_TEST(studio_settings_round_trip_and_environment) {
    using phxstudio::StudioSettings;
    StudioSettings s;
    s.scale = 3; s.restore_session = false;
    s.devkitarm = "/opt/dkp/devkitARM"; s.pspdev = "/opt/pspdev"; s.mgba = "/usr/bin/mgba-qt";
    const StudioSettings r = StudioSettings::parse(s.to_text());
    CHECK(r.scale == 3 && !r.restore_session && r.devkitarm == s.devkitarm && r.pspdev == s.pspdev && r.mgba == s.mgba &&
          r.devkitpro.empty() && r.ppsspp.empty());
    CHECK(StudioSettings::parse("# comment\r\nscale=99\r\nnonsense\r\n").scale == 8);   // clamped, CRLF-tolerant
    CHECK(StudioSettings{}.env().empty() && s.env().size() == 3 && s.env()[0].first == "DEVKITARM");
    StudioSettings e;
    e.devkitpro = "/phx/test/devkitpro"; e.pspdev = "/phx/test/pspdev";
    e.apply_env();
    const char* dkp = std::getenv("DEVKITPRO");
    const char* path = std::getenv("PATH");
    CHECK(dkp && std::string(dkp) == "/phx/test/devkitpro" && path && std::string(path).find("/phx/test/pspdev/bin") != std::string::npos);
    e.apply_env();                                                 // idempotent: PATH gains the bin once
    const std::string p2 = std::getenv("PATH");
    CHECK(p2.find("/phx/test/pspdev/bin") == p2.rfind("/phx/test/pspdev/bin"));
}

PHX_TEST(sprdoc_round_trips_through_the_bake) {
    SprDoc s;
    s.sheet = "e_sheet.png"; s.frame_w = 8; s.frame_h = 8;
    s.clips = { SprClip{ "walk", 0, 4, 10, true }, SprClip{ "jump", 4, 2, 12, false } };
    PixelDoc sheet = PixelDoc::blank(48, 8, px_rgba(1, 2, 3));
    std::string err;
    CHECK(sheet.save_png("build/e_sheet.png", &err));
    // .sprdef
    CHECK(s.save("build/e_sprite.sprdef", &err));
    SprDoc r;
    CHECK(SprDoc::load("build/e_sprite.sprdef", r, &err) && r.clips.size() == 2 && r.clips[1].name == "jump" && !r.clips[1].loop);
    phxtool::SprDef bake;
    CHECK(phxtool::load_sprdef("build/e_sprite.sprdef", bake) && bake.fw == 8 && bake.clips.size() == 2 &&
          bake.clips[0].name == phx::fnv1a("walk") && bake.clips[0].count == 4 && bake.clips[1].fps == 12);
    // .json sidecar
    s.json = true;
    CHECK(s.save("build/e_sprite.json", &err));
    CHECK(SprDoc::load("build/e_sprite.json", r, &err) && r.json && r.clips[0].name == "walk" && r.clips[0].count == 4);
    phxtool::SprDef bj;
    CHECK(phxtool::load_sprjson("build/e_sprite.json", bj, &err) && bj.clips.size() == 2 && bj.clips[1].first == 4);
    // helpers + validation
    CHECK_EQ(s.frame_count(48, 8), 6);
    int fx, fy;
    s.frame_xy(5, 48, fx, fy);
    CHECK(fx == 40 && fy == 0);
    CHECK(s.fresh_name("walk") == "walk2" && s.fresh_name("run") == "run");
    CHECK(s.validate(48, 8).empty());
    s.clips.push_back(SprClip{ "bad", 5, 3, 0, true });
    CHECK(s.validate(48, 8).size() == 2);                           // past the end + fps 0
    CHECK(SprDoc::resolve_sheet("a/b/x.sprdef", "s.png") == "a/b/s.png" && SprDoc::resolve_sheet("a/x.sprdef", "art/s.png") == "art/s.png");
}

PHX_TEST(sprdoc_transitions_round_trip_and_follow_clip_edits) {
    SprDoc s;
    s.sheet = "e_sheet.png"; s.frame_w = 8; s.frame_h = 8;
    s.clips = { SprClip{ "idle", 0, 1, 1, true }, SprClip{ "walk", 1, 3, 10, true }, SprClip{ "hit", 4, 2, 12, false } };
    s.trans = { SprEdge{ "idle", "walk", "move" }, SprEdge{ "*", "hit", "hurt" }, SprEdge{ "hit", "idle", "done" } };
    std::string err;
    PixelDoc sheet = PixelDoc::blank(48, 8, px_rgba(1, 2, 3));
    CHECK(sheet.save_png("build/e_sheet.png", &err));
    for (int js = 0; js < 2; ++js) {                                // .sprdef, then the .json sidecar
        s.json = js != 0;
        const std::string path = js ? "build/e_trans.json" : "build/e_trans.sprdef";
        CHECK(s.save(path, &err));
        SprDoc r;
        CHECK(SprDoc::load(path, r, &err) && r.trans.size() == 3 && r.trans[1] == s.trans[1] && r.trans[2] == s.trans[2]);
        phxtool::SprDef bake;                                        // the bake reads what the Studio wrote
        CHECK(js ? phxtool::load_sprjson(path, bake, &err) : phxtool::load_sprdef(path, bake));
        std::vector<phx::SpriteTransDef> tr;
        CHECK(phxtool::sprite_transitions(bake, tr, &err) && tr.size() == 3 && tr[0].from == 0 && tr[0].to == 1 &&
              tr[0].trigger == phx::fnv1a("move") && tr[1].from == phx::kSpriteTransAny && tr[1].to == 2);
        phxtool::BundleWriter w(2);                                  // and builds the sprite with them
        CHECK(phxtool::build_sprite(w, path, "e_trans"));
    }
    CHECK(s.validate(48, 8).empty());
    s.rename_clip(2, "ouch");                                        // edges follow a rename...
    CHECK(s.trans[1].to == "ouch" && s.trans[2].from == "ouch" && s.validate(48, 8).empty());
    s.remove_clip(0);                                                // ...and go with a removed clip
    CHECK(s.trans.size() == 1 && s.trans[0].to == "ouch");
    s.trans.push_back(SprEdge{ "walk", "nope", "" });
    CHECK(s.validate(48, 8).size() == 2);                            // unknown clip + no trigger
    phxtool::SprDef bad;                                             // the bake refuses a dangling edge
    bad.clip_names = { "idle" }; bad.trans = { phxtool::SprTrans{ "idle", "run", "move" } };
    std::vector<phx::SpriteTransDef> tr;
    CHECK(!phxtool::sprite_transitions(bad, tr, &err) && err.find("run") != std::string::npos);
    CHECK(s.suggest_edge(1).from == "ouch" && s.suggest_edge(1).trigger == "done");   // a one-shot: "done"
}

// =============================================================================================
// TmapDoc (new operations) — re-read by the bake's tiled_load
// =============================================================================================
PHX_TEST(tmapdoc_tileset_image_layers_resize) {
    using phxtool::TmapDoc;
    TmapDoc d = TmapDoc::blank(4, 3, 8, 8, "tiles");
    d.tileset_image = "../art/tiles.png";
    d.tileset_cols = 4; d.tileset_count = 8; d.tileset_img_w = 32; d.tileset_img_h = 16;
    d.add_layer("front");
    d.rename_layer(0, "sky");
    d.set_parallax(0, 0.25, 0.5);
    d.set_tile(1, 3, 2, 5);
    d.add_spawn("coin", 24, 16);
    CHECK(d.move_layer(0, 1) && d.layer_names[1] == "sky" && d.layer_parallax[1].first == 0.25);
    CHECK(!d.remove_layer(5));
    TmapDoc r;
    std::string err;
    CHECK(TmapDoc::load(d.save_tmj(), r, &err));
    CHECK(r.tileset_image == "../art/tiles.png" && r.tileset_cols == 4 && r.tileset_count == 8 && r.tileset == "tiles");
    CHECK(r.layer_names[1] == "sky" && r.layer_parallax[1].second == 0.5 && r.tile(0, 3, 2) == 5);
    phxtool::TiledMap tm;                                            // the bake still accepts it
    CHECK(phxtool::tiled_load(d.save_tmj(), tm, &err) && tm.tileset == "tiles" && tm.layers.size() == 2);
    // resize anchored bottom-right: old (3,2) -> (5,4); the spawn moves with it
    d.push_undo();
    d.resize(6, 5, 2, 2);
    CHECK(d.width == 6 && d.height == 5 && d.tile(0, 5, 4) == 5 && d.spawns[0].x == 40 && d.spawns[0].y == 32);
    CHECK(d.undo() && d.width == 4 && d.tile(0, 3, 2) == 5);        // resize is undoable
    d.resize(2, 2, 0, 0);                                            // shrink drops the far spawn
    CHECK(d.spawns.empty());
}

PHX_TEST(tmapdoc_stamps_spawns_usage) {
    using phxtool::TmapDoc;
    TmapDoc d = TmapDoc::blank(6, 4, 8, 8, "tiles");
    d.set_tile(0, 0, 0, 1); d.set_tile(0, 1, 0, 2); d.set_tile(0, 0, 1, 3);
    TmapDoc::Stamp st = d.copy_stamp(0, 1, 1, 0, 0);                 // either corner order
    CHECK(st.w == 2 && st.h == 2 && st.gids[0] == 1 && st.gids[1] == 2 && st.gids[2] == 3 && st.gids[3] == 0);
    CHECK_EQ(d.paint_stamp(0, 4, 2, st, true), 3);                  // the 0 cell is transparent
    CHECK(d.tile(0, 5, 2) == 2 && d.tile(0, 5, 3) == 0);
    CHECK_EQ(d.replace_gid(-1, 2, 7), 2);
    const std::vector<int> u = d.gid_usage(0);
    CHECK(u.size() == 8 && u[7] == 2 && u[1] == 2);
    d.add_spawn("coin", 16, 8);
    d.spawns.back().w = 16;
    CHECK(d.spawn_at(20, 10) == 0 && d.spawn_at(31, 10) == 0 && d.spawn_at(33, 10) == -1);
    d.move_spawn(0, 0, 0);
    CHECK(d.spawn_at(1, 1) == 0);
    d.spawns[0].name = "coin";
    CHECK(d.fresh_spawn_name("coin") == "coin2" && d.fresh_spawn_name("key") == "key");
    d.remove_spawn(0);
    CHECK(d.spawns.empty());
}

// =============================================================================================
// BinDoc (new operations) — re-read by the bake's build_bin
// =============================================================================================
PHX_TEST(bindoc_typed_cells_and_schema_edits) {
    using phxtool::BinDoc;
    BinDoc d;
    std::string err;
    CHECK(BinDoc::blank("Enemy", { "type:str8", "hp:u8", "speed:f32" }, d, &err));
    d.add_record(size_t(-1));
    CHECK(d.set_cell_text(0, 0, "a-very-long-name"));               // clips to char[8] - 1
    CHECK(d.str_cell(0, 0) == "a-very-");
    CHECK(d.set_cell_text(0, 1, "300") && d.records[0][1] == 255);  // clamps to u8
    CHECK(d.set_cell_text(0, 1, "0x10") && d.records[0][1] == 16);
    CHECK(!d.set_cell_text(0, 1, "fast"));
    CHECK(d.set_cell_text(0, 2, "1.5") && d.flt_cell(0, 2) == 1.5);
    CHECK(d.cell_text(0, 2) == "1.5");
    CHECK(d.rename_field(1, "health") && !d.rename_field(1, "speed") && !d.rename_field(1, "2bad"));
    CHECK(d.move_field(2, -1) && d.fields[1].name == "speed" && d.flt_cell(0, 1) == 1.5);
    CHECK(d.set_field_type(1, "i16") && d.records[0][1] == 1);       // 1.5 -> integer
    CHECK(d.set_field_type(1, "str16") && d.str_cell(0, 1) == "1");
    d.push_undo();
    d.insert_record(0, false);
    CHECK(d.records.size() == 2 && d.str_cell(1, 0) == "a-very-");
    CHECK(d.move_record(0, 1) && d.str_cell(0, 0) == "a-very-");
    CHECK(d.undo() && d.records.size() == 1);
    CHECK(d.redo() && d.records.size() == 2);
    CHECK(BinDoc::valid_ident("_ok9") && !BinDoc::valid_ident("9no") && !BinDoc::valid_ident("a-b"));
    CHECK(d.fresh_field_name("health") == "health2");
}

PHX_TEST(bindoc_f32_round_trip_and_bake) {
    using phxtool::BinDoc;
    BinDoc d;
    std::string err;
    CHECK(BinDoc::blank("Tune", { "name:str16", "gravity:f32", "jumps:u8" }, d, &err));
    d.add_record(size_t(-1));
    d.set_cell_text(0, 0, "hero");
    d.set_cell_text(0, 1, "-9.81");
    d.set_cell_text(0, 2, "2");
    d.add_record(0);
    d.set_cell_text(1, 0, "hero");
    CHECK(d.duplicate_names().size() == 1 && d.duplicate_names()[0] == "hero");
    BinDoc r;
    CHECK(BinDoc::load(d.save_json(), r, &err));
    CHECK(float(r.flt_cell(0, 1)) == -9.81f && r.records[0][2] == 2);   // f32 survives (it was truncated before)
    CHECK(write_file("build/e_tune.json", d.save_json()));
    phxtool::BundleWriter w(2);
    CHECK(phxtool::build_bin(w, "build/e_tune.json", "e_tune", "build/e_tune.gen.h"));
    CHECK(r.name_field() == 0 && r.name_column().size() == 2);
}

// =============================================================================================
// project helpers + canvas math
// =============================================================================================
PHX_TEST(project_kinds_fuzzy_and_diagnostics) {
    CHECK(kind_for("a.png") == FileKind::Image && kind_for("x.sprdef") == FileKind::Sprite && kind_for("m.tmj") == FileKind::Map);
    CHECK(kind_for("t.json", "{\"struct\":\"A\",\"fields\":[],\"records\":[]}") == FileKind::Table);
    CHECK(kind_for("s.json", "{\"image\":\"a.png\",\"animations\":{}}") == FileKind::Sprite);
    CHECK(kind_for("p.json", "{\"a\":1}") == FileKind::Code && kind_for("Makefile") == FileKind::Code);
    CHECK(kind_for("README.md") == FileKind::Text && kind_for("b.phxp") == FileKind::Bundle);
    CHECK(fuzzy_score("rend", "engine/render/src/renderer.cpp") > fuzzy_score("rend", "docs/03-rendering-notes-and-more.md"));
    CHECK(fuzzy_score("xyz", "engine/render.cpp") < 0 && fuzzy_score("", "a") == 0);
    Diag dg;
    CHECK(parse_diag("tools/phxstudio/ed_map.cpp:12:5: error: expected ';'", dg) && dg.line == 12 && dg.col == 5 &&
          dg.severity == 2 && dg.file == "tools/phxstudio/ed_map.cpp" && dg.msg == "expected ';'");
    CHECK(parse_diag("a.h:7: warning: unused", dg) && dg.line == 7 && dg.col == 0 && dg.severity == 1);
    CHECK(parse_diag("x.cpp:3:1: fatal error: y.h: No such file", dg) && dg.severity == 2);
    CHECK(!parse_diag("make: *** [Makefile:12: studio] Error 1", dg) && !parse_diag("PASS 12 checks", dg));
    CHECK(code_template("engine/core/include/phx/core/foo.h").find("#ifndef ENGINE_CORE_INCLUDE_PHX_CORE_FOO_H") != std::string::npos);
    CHECK(stem_of("a/b/hero.sheet.png") == "hero.sheet" && dir_name("a/b/c.txt") == "a/b" && join_path("a", "b") == "a/b");
}

PHX_TEST(project_file_tree) {
    namespace fs = std::filesystem;
    const std::string root = fs::absolute("build/e_tree").string();
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root + "/zeta/inner", ec);
    fs::create_directories(root + "/.git", ec);
    fs::create_directories(root + "/obj-pc-r0", ec);
    write_file(root + "/b.cpp", "x");
    write_file(root + "/a.png", "x");
    write_file(root + "/zeta/inner/t.json", "{\"struct\":\"T\",\"fields\":[{\"name\":\"a\",\"type\":\"u8\"}],\"records\":[]}");
    FileTree t;
    t.open(root);
    const auto rows = t.rows();
    CHECK(rows.size() == 3);                                        // .git + obj-* hidden
    CHECK(rows[0].node->name == "zeta" && rows[0].node->dir);       // folders first
    CHECK(rows[1].node->name == "a.png" && rows[1].node->kind == FileKind::Image);
    t.reveal("zeta/inner/t.json");
    const auto rows2 = t.rows();
    CHECK(rows2.size() == 5 && rows2[2].node->kind == FileKind::Table && rows2[2].depth == 2);
    const auto all = t.all_files();
    CHECK(all.size() == 3 && all[2] == "zeta/inner/t.json");
}

PHX_TEST(asset_source_matches_stem_and_editor_kind) {
    namespace fs = std::filesystem;
    const std::string root = fs::absolute("build/e_src").string();
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root + "/assets/sfx", ec);
    fs::create_directories(root + "/build/intermediate", ec);
    const std::string table = "{\"struct\":\"T\",\"fields\":[{\"name\":\"a\",\"type\":\"u8\"}],\"records\":[]}";
    const std::string sprite = "{\"image\":\"hero.png\",\"frame_w\":16,\"frame_h\":16,\"animations\":{}}";
    write_file(root + "/assets/hero.sprdef", "sheet hero.png\n");
    write_file(root + "/assets/hero.png", "x");
    write_file(root + "/build/hero.png", "x");                      // a staged copy
    write_file(root + "/hero.ppm", "x");                            // a screenshot
    write_file(root + "/assets/items.json", table);
    write_file(root + "/assets/boss.json", sprite);
    write_file(root + "/assets/level.tmj", "{}");
    write_file(root + "/assets/sfx/jump.wav", "x");
    FileTree t;
    t.open(root);
    const auto files = t.all_files();
    using V = std::vector<std::string>;
    CHECK(asset_source_candidates(files, root, "hero", FileKind::Image) == V({ "assets/hero.png", "build/hero.png" }));
    CHECK(asset_source_candidates(files, root, "hero", FileKind::Sprite) == V({ "assets/hero.sprdef" }));
    CHECK(asset_source_candidates(files, root, "boss", FileKind::Sprite) == V({ "assets/boss.json" }));   // JSON sidecar
    CHECK(asset_source_candidates(files, root, "items", FileKind::Table) == V({ "assets/items.json" }));
    CHECK(asset_source_candidates(files, root, "items", FileKind::Sprite).empty());                       // a table isn't a sprite
    CHECK(asset_source_candidates(files, root, "level", FileKind::Map) == V({ "assets/level.tmj" }));
    CHECK(asset_source_candidates(files, root, "jump", FileKind::Sound) == V({ "assets/sfx/jump.wav" }));
    CHECK(asset_source_candidates(files, root, "nope", FileKind::Image).empty());
}

PHX_TEST(canvas_zoom_math) {
    Canvas c;
    c.zoom = 4; c.ox = 100; c.oy = 50;
    CHECK(c.to_cx(100) == 0 && c.to_cx(103) == 0 && c.to_cx(104) == 1 && c.to_cx(99) == -1);
    const int cx = c.to_cx(140), cy = c.to_cy(90);
    c.zoom_at(140, 90, 8);                                          // the point under the pointer stays
    CHECK(c.zoom == 8 && c.to_cx(140) == cx && c.to_cy(90) == cy);
    CHECK(Canvas::step_zoom(4, 1) == 5 && Canvas::step_zoom(4, -1) == 3 && Canvas::step_zoom(64, 1) == 64 && Canvas::step_zoom(1, -1) == 1);
    c.fit(twk::Rect{ 0, 0, 200, 100 }, 16, 8, 16);
    CHECK(c.zoom == 10 && c.fitted);                                // 16*(10+1) fits 184, 8*11 overflows 84
}

// =============================================================================================
// game projects + the project boundary (projectdoc.h)
// =============================================================================================
PHX_TEST(projectdoc_parse_save_round_trip) {
    ProjectDoc p;
    std::string err;
    CHECK(ProjectDoc::parse("{ \"name\": \"Ember\", \"assets\": [\"art\", \"maps\"], \"bundles\": [\"build/e.phxp\"],"
                            " \"launches\": [ { \"label\": \"Play\", \"command\": \"make play\", \"needs\": [\"sdl2-config\"],"
                            " \"windowed\": true }, { \"label\": \"Test\", \"group\": \"test\", \"command\": \"make t\" } ] }",
                            "/tmp/ember", p, &err));
    CHECK(p.name == "Ember" && p.dir == "/tmp/ember" && p.assets.size() == 2 && p.source.size() == 1 && p.source[0] == "src");
    CHECK(p.launches.size() == 2 && p.launches[0].group == "play" && p.launches[0].windowed && p.launches[0].needs[0] == "sdl2-config");
    CHECK(p.launches[1].group == "test" && !p.launches[1].windowed);
    ProjectDoc r;
    CHECK(ProjectDoc::parse(p.to_json(), p.dir, r, &err) && r.name == "Ember" && r.bundles == p.bundles &&
          r.launches.size() == 2 && r.launches[1].command == "make t" && r.launches[0].windowed);
    CHECK(!ProjectDoc::parse("{ \"launches\": [ { \"label\": \"x\" } ] }", "/tmp/a", r, &err) && !err.empty());   // no command
    CHECK(!ProjectDoc::parse("[1]", "/tmp/a", r, &err));
    CHECK(ProjectDoc::parse("{}", "/tmp/untitled", r, &err) && r.name == "untitled");                           // folder name
    CHECK(project_slug("My Game 2!") == "my_game_2" && project_slug("") .empty());
}

PHX_TEST(access_policy_is_the_project_boundary) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const std::string base = canon_path("build/e_access");
    fs::remove_all(base, ec);
    for (const char* d : { "engine/render/include/phx/render", "engine/render/src", "tools/phxstudio", "docs",
                           "tests", "examples/game/src", "examples/other" })
        fs::create_directories(base + "/" + d, ec);
    for (const char* f : { "engine/render/include/phx/render/renderer.h", "engine/render/src/renderer.cpp",
                           "tools/phxstudio/instructions.md", "tools/phxstudio/main.cpp", "docs/03-rendering.md",
                           "Makefile", "README.md", "examples/game/src/main.cpp", "examples/other/x.cpp" })
        write_file(base + "/" + f, "x");
    AccessPolicy pol;
    pol.engine_root = base;
    pol.project_dir = base + "/examples/game";
    CHECK(pol.access(base + "/examples/game/src/main.cpp") == Access::Write);
    CHECK(pol.access(base + "/examples/game/src/new_file.cpp") == Access::Write);                  // not yet on disk
    CHECK(pol.access(base + "/engine/render/include/phx/render/renderer.h") == Access::Read);      // public API
    CHECK(pol.access(base + "/docs/03-rendering.md") == Access::Read);
    CHECK(pol.access(base + "/tools/phxstudio/instructions.md") == Access::Read);
    CHECK(pol.access(base + "/README.md") == Access::Read);
    CHECK(pol.access(base + "/engine/render/src/renderer.cpp") == Access::None);                   // engine source
    CHECK(pol.access(base + "/tools/phxstudio/main.cpp") == Access::None);
    CHECK(pol.access(base + "/Makefile") == Access::None);
    CHECK(pol.access(base + "/examples/other/x.cpp") == Access::None);                              // another project
    CHECK(pol.access(base + "/examples/game/../../engine/render/src/renderer.cpp") == Access::None); // traversal
    CHECK(pol.access("/etc/passwd") == Access::None);
    fs::create_directory_symlink(base + "/engine/render/src", base + "/examples/game/src/engine_src", ec);
    if (!ec) CHECK(pol.access(base + "/examples/game/src/engine_src/renderer.cpp") == Access::None);   // symlink escape
    AccessPolicy none = pol;
    none.project_dir.clear();                                                                       // picker: nothing
    CHECK(none.access(base + "/examples/game/src/main.cpp") == Access::None);
    AccessPolicy dev = pol;
    dev.engine_dev = true;                                                                          // --engine-dev
    CHECK(dev.access(base + "/engine/render/src/renderer.cpp") == Access::Write);
    // where a project may live
    CHECK(pol.project_dir_problem(base + "/examples/game").empty());
    CHECK(pol.project_dir_problem("/tmp/elsewhere").empty());
    CHECK(!pol.project_dir_problem(base).empty());                                                  // the checkout itself
    CHECK(!pol.project_dir_problem(base + "/engine/render").empty());
    CHECK(!pol.project_dir_problem(base + "/tools").empty() && !pol.project_dir_problem(base + "/tests").empty());
    CHECK(!pol.project_dir_problem(fs::path(base).parent_path().string()).empty());                // contains the engine
    CHECK(path_within("/a/bc", "/a/b") == false && path_within("/a/b/c", "/a/b") && path_within("/a/b", "/a/b"));
}

PHX_TEST(project_launch_command_and_discovery) {
    ProjectLaunch l;
    l.label = "Play"; l.command = "make -C \"$PHX_ROOT\" play PROJECT=\"$PHX_PROJECT\"";
    const std::string c = launch_shell_command(l, "/src/phosphorus", "/games/it's mine");
    CHECK(c.find("export PHX_ROOT='/src/phosphorus' PHX_PROJECT='/games/it'\\''s mine'") == 0);
    CHECK(c.find("cd '/games/it'\\''s mine' && make -C") != std::string::npos);
    namespace fs = std::filesystem;
    std::error_code ec;
    const std::string base = canon_path("build/e_disc");
    fs::remove_all(base, ec);
    fs::create_directories(base + "/examples/a", ec);
    fs::create_directories(base + "/examples/b", ec);
    write_file(base + "/examples/a/phxproject.json", "{ \"name\": \"A\" }");
    const std::vector<std::string> found = discover_projects(base);
    CHECK(found.size() == 1 && found[0] == base + "/examples/a");
    CHECK(ProjectDoc::is_project_dir(base + "/examples/a") && !ProjectDoc::is_project_dir(base + "/examples/b"));
    const std::vector<ProjectLaunch> std_l = ProjectDoc::standard_launches();
    CHECK(std_l.size() == 11 && std_l[0].windowed && std_l[0].command.find("play PROJECT=") != std::string::npos &&
          std_l[5].label == "Export for PC" && std_l[5].command.find("game-export PROJECT=") != std::string::npos &&
          std_l[5].command.find("EXPORT=pc") != std::string::npos && std_l[8].label == "Measure budgets" &&
          std_l[8].command.find("project-budget PROJECT=") != std::string::npos);
    CHECK(std_l[3].group == "console" && std_l[3].command.find("play-gba PROJECT=") != std::string::npos &&
          std_l[4].group == "console" && std_l[4].command.find("play-psp PROJECT=") != std::string::npos);
}

PHX_TEST(map_editor_draws_spawns_as_their_prefab_sprites) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const std::string dir = canon_path("build/e_spawnart");
    fs::remove_all(dir, ec);
    std::string err;
    CHECK(create_project(dir, "Art Check", &err));
    std::vector<std::string> files;                          // root-relative, like the Studio's list
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec)) files.push_back(fs::relative(it->path(), dir, ec).generic_string());
    const std::map<std::string, SpawnArt> art = resolve_spawn_art(dir, files);
    CHECK(art.size() == 5 && art.count("player") && art.count("coin") && art.count("slime") && art.count("door") && art.count("sign"));
    if (art.count("door")) CHECK(art.at("door").w == 16 && art.at("door").h == 16);   // a plain PNG: the whole image
    if (art.count("player")) {
        const SpawnArt& p = art.at("player");                // hero.sprdef: 16x16 frames, "idle" starts at 0
        CHECK(p.sheet == dir + "/assets/hero.png" && p.sx == 0 && p.sy == 0 && p.w == 16 && p.h == 16);
    }
    if (art.count("coin")) CHECK(art.at("coin").sheet == dir + "/assets/coin.png" && art.at("coin").w == 8);
    // a prefab whose sprite is a plain PNG draws the whole image; a type with no sprite has no art
    const std::string pj = "{ \"struct\":\"P\", \"fields\":[{\"name\":\"type\",\"type\":\"str16\"},{\"name\":\"sprite\",\"type\":\"str16\"}],"
                           " \"records\":[{\"type\":\"crate\",\"sprite\":\"tiles\"},{\"type\":\"ghost\",\"sprite\":\"\"}] }";
    if (FILE* f = std::fopen((dir + "/assets/more.json").c_str(), "wb")) { std::fwrite(pj.data(), 1, pj.size(), f); std::fclose(f); }
    files.push_back("assets/more.json");
    const std::map<std::string, SpawnArt> art2 = resolve_spawn_art(dir, files);
    CHECK(art2.count("crate") && art2.at("crate").w == 64 && art2.at("crate").h == 8 && !art2.count("ghost"));
}

PHX_TEST(prefab_inspector_applies_reflected_components) {
    // what component_schema.cpp writes for a game's PHX_COMPONENTs
    const char* json = "{ \"components\": [\n"
                       "  { \"name\": \"Enemy\", \"size\": 8, \"fields\": [\n"
                       "    { \"name\": \"range\", \"type\": \"i16\", \"default\": 24 },\n"
                       "    { \"name\": \"speed\", \"type\": \"scalar\", \"default\": 1.5 },\n"
                       "    { \"name\": \"angry\", \"type\": \"bool\", \"default\": 1 },\n"
                       "    { \"name\": \"target\", \"type\": \"hash\", \"default\": 0 } ] },\n"
                       "  { \"name\": \"Coin\", \"size\": 2, \"fields\": [ { \"name\": \"value\", \"type\": \"i16\", \"default\": 1 } ] } ] }";
    CompSchema s;
    CHECK(CompSchema::parse(json, s) && s.comps.size() == 2 && s.find("Enemy") && s.find("Enemy")->fields.size() == 4);
    CHECK(s.find("Enemy")->fields[1].type == "scalar" && s.find("Enemy")->fields[1].def == 1.5 && !s.find("Nope"));
    CHECK(!CompSchema::parse("{}", s) && !CompSchema::parse("nope", s));
    CompSchema::parse(json, s);
    CHECK(comp_column_type("scalar") == "f32" && comp_column_type("hash") == "str16" && comp_column_type("bool") == "u8" &&
          comp_column_type("i16") == "i16");
    CHECK(split_components(" Enemy,Coin  X ").size() == 3 && split_components("").empty());

    phxtool::BinDoc d;
    CHECK(phxtool::BinDoc::load("{ \"struct\":\"Prefab\", \"fields\":[{\"name\":\"type\",\"type\":\"str16\"}],"
                                " \"records\":[{\"type\":\"enemy\"},{\"type\":\"coin\"}] }", d));
    const CompSchema::Comp& enemy = *s.find("Enemy");
    CHECK(set_record_component(d, 0, enemy, true));                   // adds `components` + 4 typed columns
    CHECK(!set_record_component(d, 0, enemy, true));                  // already on: no change
    CHECK(d.fields.size() == 6 && components_field(d) == 1 && d.str_cell(0, 1) == "Enemy");
    CHECK(d.fields[2].name == "Enemy_range" && d.fields[2].type == "i16" && d.records[0][2] == 24);
    CHECK(d.fields[3].type == "f32" && d.flt_cell(0, 3) == 1.5 && d.fields[4].type == "u8" && d.records[0][4] == 1);
    CHECK(d.fields[5].name == "Enemy_target" && d.fields[5].type == "str16");
    CHECK(d.records[1][2] == 0 && !record_has_component(d, 1, "Enemy"));   // the other record is untouched
    CHECK(set_record_component(d, 0, *s.find("Coin"), true) && d.str_cell(0, 1) == "Enemy Coin" && d.fields.size() == 7);
    CHECK(set_record_component(d, 0, enemy, false) && d.str_cell(0, 1) == "Coin" && d.fields.size() == 7);  // columns stay
    CHECK(!set_record_component(d, 1, enemy, false));                 // not on: no change
    // the result still bakes through phxbin's own dialect
    phxtool::BinDoc back;
    CHECK(phxtool::BinDoc::load(d.save_json(), back) && back.fields.size() == 7 && back.str_cell(0, 1) == "Coin");
}

PHX_TEST(new_project_template_is_complete_and_bakeable) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const std::string dir = "build/e_newproj";
    fs::remove_all(dir, ec);
    std::string err;
    CHECK(create_project(dir, "Test Quest", &err));
    CHECK(!create_project(dir, "Test Quest", &err) && !err.empty());                                // refuses a non-empty folder
    ProjectDoc p;
    CHECK(ProjectDoc::load(dir, p, &err) && p.name == "Test Quest" && p.launches.size() == 11 &&
          p.bundles.size() == 1 && p.bundles[0] == "build/test_quest.phxp");
    std::string main_cpp;
    {
        FILE* f = std::fopen((dir + "/src/main.cpp").c_str(), "rb");
        CHECK(f != nullptr);
        if (f) { char b[65536]; const size_t n = std::fread(b, 1, sizeof(b), f); main_cpp.assign(b, n); std::fclose(f); }
    }
    CHECK(main_cpp.find("struct TestQuestGame final : Game") != std::string::npos);
    CHECK(main_cpp.find("PHX_GAME(TestQuestGame);") != std::string::npos);                          // the engine's main()
    CHECK(main_cpp.find("int main(") == std::string::npos && main_cpp.find("phx/runtime/main.h") != std::string::npos);
    CHECK(main_cpp.find("\"build/test_quest.phxp\"") != std::string::npos);
    CHECK(main_cpp.find("@") == std::string::npos);                                                  // every placeholder filled
    CHECK(main_cpp.find("#include \"phx/") != std::string::npos && main_cpp.find("src/") != std::string::npos);
    // the template assets are exactly what the bake reads
    phxtool::SprDef sd;
    CHECK(phxtool::load_sprdef(dir + "/assets/hero.sprdef", sd) && sd.fw == 16 && sd.clips.size() == 4 && sd.trans.size() == 6);
    {
        std::vector<phx::SpriteTransDef> tr;
        CHECK(phxtool::sprite_transitions(sd, tr) && tr.size() == 6 && tr[2].from == phx::kSpriteTransAny &&
              tr[5].from == 3 && tr[5].to == 0 && tr[5].trigger == phx::fnv1a("done"));   // land -done-> idle
    }
    PixelDoc hero, tiles;
    CHECK(PixelDoc::load_png(dir + "/assets/hero.png", hero, &err) && hero.w == 96 && hero.h == 16);
    CHECK(PixelDoc::load_png(dir + "/assets/tiles.png", tiles, &err) && tiles.w == 64 && tiles.h == 8);
    std::string tmj;
    {
        FILE* f = std::fopen((dir + "/assets/level.tmj").c_str(), "rb");
        if (f) { char b[65536]; size_t n; while ((n = std::fread(b, 1, sizeof(b), f)) > 0) tmj.append(b, n); std::fclose(f); }
    }
    phxtool::TiledMap tm;
    CHECK(phxtool::tiled_load(tmj, tm, &err) && tm.tileset == "tiles" && tm.layers.size() == 2 && tm.spawns.size() == 7);
    {   // the prefab table names every spawn type the level places, and the map editor offers them
        std::string pj;
        FILE* f = std::fopen((dir + "/assets/prefabs.json").c_str(), "rb");
        if (f) { char b[8192]; size_t n; while ((n = std::fread(b, 1, sizeof(b), f)) > 0) pj.append(b, n); std::fclose(f); }
        phxtool::BinDoc pd;
        CHECK(phxtool::BinDoc::load(pj, pd));
        const std::vector<std::string> types = pd.name_column();
        CHECK(types.size() == 5 && types[0] == "player" && types[1] == "coin" && types[2] == "slime" && types[3] == "door" &&
              types[4] == "sign");
        // the game flow: title -> level1 (the map) -> end, with a game over that retries
        std::string fj;
        if (FILE* ff = std::fopen((dir + "/assets/flow.json").c_str(), "rb")) {
            char b[8192]; size_t n; while ((n = std::fread(b, 1, sizeof(b), ff)) > 0) fj.append(b, n); std::fclose(ff);
        }
        phxtool::BinDoc fd;
        CHECK(phxtool::BinDoc::load(fj, fd) && fd.records.size() == 4 && fd.str_cell(0, 0) == "title" &&
              fd.str_cell(1, 2) == "level" && fd.str_cell(0, 3) == "Test Quest|a Phosphorus game");
        CHECK(fs::exists(dir + "/assets/font.png") && fs::exists(dir + "/assets/door.png") &&
              main_cpp.find("flow.update(app, dt)") != std::string::npos);
        for (const auto& s : tm.spawns) CHECK(std::find(types.begin(), types.end(), s.type) != types.end());
        phxtool::SprDef cd, sd2;
        CHECK(phxtool::load_sprdef(dir + "/assets/coin.sprdef", cd) && cd.fw == 8 && cd.clips.size() == 1);
        CHECK(phxtool::load_sprdef(dir + "/assets/slime.sprdef", sd2) && sd2.fw == 16);
        // the prefabs are stock behaviours, listed by name (a str64 fits the player's two)
        const size_t cf = components_field(pd);
        CHECK(cf < pd.fields.size() && pd.fields[cf].type == "str64" &&
              pd.str_cell(0, cf) == "PlatformerController CameraFollow" && pd.str_cell(2, cf) == "Patrol Hazard");
    }
    {   // the sounds are .sfx documents and the music a .song, which phxsnd bakes (the stock
        // behaviours play "jump" and "coin" by name; the flow's title screen starts "theme")
        phxtool::BundleWriter sw(2);
        CHECK(phxtool::build_sfx(sw, dir + "/assets/jump.sfx") && phxtool::build_sfx(sw, dir + "/assets/coin.sfx") &&
              phxtool::build_song(sw, dir + "/assets/theme.song"));
        phxtool::SfxParams jp;
        CHECK(phxtool::sfx_from_json(read_head(dir + "/assets/jump.sfx", 1u << 20), jp) && jp == tmpl::jump_sfx());
        phxtool::Song song;
        CHECK(phxtool::song_from_json(read_head(dir + "/assets/theme.song", 1u << 20), song) &&
              song.instruments.size() == 4 && song.order.size() == 2 && song.total_rows() == 32);
        CHECK(!fs::exists(dir + "/assets/jump.wav"));
        phxtool::FontDef fdef;                             // the flow's font: proportional, over font.png
        CHECK(phxtool::load_fontdef(read_head(dir + "/assets/font.font", 1u << 20), dir + "/assets/font.font", fdef) &&
              fdef.proportional && fdef.image == "font.png" && phxtool::build_font(sw, dir + "/assets/font.font"));
        CHECK(kind_for("assets/font.font") == FileKind::Font && kind_for("x.fnt") == FileKind::Text);
        phxtool::DlgDoc dd;                                // the sign's conversation (the Talk prefab names it)
        CHECK(phxtool::dlg_from_json(read_head(dir + "/assets/dialogue.dlg", 1u << 20), dd) && dd.find_conv("sign") == 0 &&
              !phxtool::dlg_has_errors(phxtool::dlg_validate(dd)) && phxtool::build_dialogue(sw, dir + "/assets/dialogue.dlg"));
        CHECK(kind_for("assets/dialogue.dlg") == FileKind::Dialogue);
        phxtool::BinDoc fd;
        CHECK(phxtool::BinDoc::load(read_head(dir + "/assets/flow.json", 1u << 20), fd));
        size_t music = fd.fields.size();
        for (size_t i = 0; i < fd.fields.size(); ++i) if (fd.fields[i].name == "music") music = i;
        CHECK(music < fd.fields.size() && fd.str_cell(0, music) == "theme");
    }
    phxtool::TmapDoc md;
    CHECK(phxtool::TmapDoc::load(tmj, md, &err) && md.tileset_image == "tiles.png" && md.has_tile_flags());
    phxtool::BundleWriter w(2);
    CHECK(phxtool::build_sprite(w, dir + "/assets/hero.sprdef") && phxtool::build_tmj(w, dir + "/assets/level.tmj") &&
          phxtool::build_png(w, dir + "/assets/tiles.png"));
}

int main() {
    using namespace phxtest;
    std::printf("\nPhosphorus Engine — editors suite (%d cases)\n", count());
    for (int i = 0; i < count(); ++i) {
        std::printf("  . %s\n", storage()[i].name);
        storage()[i].fn();
    }
    std::printf("editors_test: %d checks, %d failures\n", checks(), failures());
    std::printf(failures() ? "EDITORS FAIL\n" : "EDITORS PASS\n");
    return failures() ? 1 : 0;
}
