// engine/runtime/src/devtools.cpp — see phx/runtime/devtools.h. Host-only (it reads the desktop
// event stream and draws into the software framebuffer): linked by desktop game builds.
#include "phx/runtime/devtools.h"
#include "phx/runtime/level.h"
#include "phx/ecs/reflect.h"
#include "phx/core/log.h"
#include "phx/platform/desktop.h"
#include "phx/platform/gfx_soft.h"
#include "phx/platform/platform.h"

#include "dev_font.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace phx {
namespace {

constexpr uint32_t kMaxList  = 1024;
constexpr uint32_t kRing     = 120;          // frames of history in the frame-time graph
constexpr uint32_t kMaxEdits = 48;

struct DevState {
    bool        shown  = false;
    bool        paused = false;
    int         step   = 0;                  // single steps requested while paused
    ecs::Entity sel    = ecs::kInvalid;
    uint64_t    frame  = 0;
    bool        boxes  = false;              // F2: every collider + the level's collision tiles
    int         slow   = 0;                  // F3: 0 = full speed, 1 = 1/2, 2 = 1/4
    bool        halt_on_warn = false;        // F9: pause when the engine logs a warning / error
    uint32_t    warn_seen = 0;
    const char* halt = nullptr;              // why it paused itself
    int         cursor = 0;                  // PgUp / PgDn: the field -/= changes
    // the frame-time history (µs), newest at ring_at - 1
    uint32_t    frame_us[kRing]{}, update_us[kRing]{}, render_us[kRing]{};
    uint32_t    ring_at = 0, ring_n = 0;
    // PHX_TRACE=file: a per-frame timing trace (Phosphorus Studio's profiler reads it)
    const char* trace_path = nullptr;
    FILE*       trace = nullptr;
};
DevState g_dev;

// The entities worth inspecting: every one with a Transform, in store order.
uint32_t list_entities(ecs::World& w, ecs::Entity* out, uint32_t cap) {
    uint32_t n = 0;
    w.each<Transform>([&](ecs::Entity e, Transform&) { if (n < cap) out[n++] = e; });
    return n;
}

void select_step(App& app, int dir) {
    static ecs::Entity list[kMaxList];
    const uint32_t n = list_entities(app.world(), list, kMaxList);
    if (!n) { g_dev.sel = ecs::kInvalid; return; }
    int at = -1;
    for (uint32_t i = 0; i < n; ++i) if (list[i] == g_dev.sel) at = int(i);
    at = at < 0 ? 0 : (at + dir + int(n)) % int(n);
    g_dev.sel = list[at];
    g_dev.cursor = 0;
}

// The entity under framebuffer pixel (x, y): its collider (or an 8 px box round its position).
void select_at(App& app, int x, int y) {
    const Camera2D& cam = app.render().camera();
    const scalar z = cam.zoom > s_from_int(0) ? cam.zoom : s_from_int(1);
    const scalar wx = cam.pos.x + s_from_int(x) / z, wy = cam.pos.y + s_from_int(y) / z;
    ecs::World& w = app.world();
    ecs::Entity best = ecs::kInvalid;
    w.each<Transform>([&](ecs::Entity e, Transform& t) {
        const AABBColl* c = w.get<AABBColl>(e);
        const scalar hx = c ? c->half.x : s_from_int(4), hy = c ? c->half.y : s_from_int(4);
        if (wx >= t.pos.x - hx && wx <= t.pos.x + hx && wy >= t.pos.y - hy && wy <= t.pos.y + hy) best = e;
    });
    if (best != g_dev.sel) g_dev.cursor = 0;
    g_dev.sel = best;
}

// ---- live editing: the selected entity's editable values, in inspector order ----
struct EditRef {
    const char* comp;          // the component ("Transform", a reflected name)
    const char* name;          // the field
    FieldType   type;
    void*       ptr;
};
uint32_t collect_edits(ecs::World& w, ecs::Entity e, EditRef* out, uint32_t cap) {
    uint32_t n = 0;
    auto add = [&](const char* c, const char* f, FieldType t, void* p) { if (n < cap) out[n++] = EditRef{ c, f, t, p }; };
    if (e == ecs::kInvalid || !w.is_alive(e)) return 0;
    if (Transform* t = w.get<Transform>(e)) { add("Transform", "x", FieldType::Scalar, &t->pos.x); add("Transform", "y", FieldType::Scalar, &t->pos.y); }
    if (Body* b = w.get<Body>(e)) { add("Body", "vx", FieldType::Scalar, &b->vel.x); add("Body", "vy", FieldType::Scalar, &b->vel.y); }
    for (uint32_t i = 0; i < reflected_count(); ++i) {
        const ComponentInfo& ci = *reflected_at(i);
        uint8_t* d = static_cast<uint8_t*>(ci.get(w, e));
        if (!d) continue;
        for (uint8_t f = 0; f < ci.field_count; ++f)
            if (ci.fields[f].type != FieldType::Hash) add(ci.name, ci.fields[f].name, ci.fields[f].type, d + ci.fields[f].offset);
    }
    return n;
}

template <class T> void bump_int(void* p, long d, long lo, long hi) {
    T v; std::memcpy(&v, p, sizeof(T));
    long x = long(v) + d;
    x = x < lo ? lo : x > hi ? hi : x;
    v = T(x);
    std::memcpy(p, &v, sizeof(T));
}
void edit_value(const EditRef& r, int dir, bool big) {
    const long d = long(dir) * (big ? 10 : 1);
    switch (r.type) {
        case FieldType::I8:  bump_int<int8_t>(r.ptr, d, -128, 127); break;
        case FieldType::U8:  bump_int<uint8_t>(r.ptr, d, 0, 255); break;
        case FieldType::I16: bump_int<int16_t>(r.ptr, d, -32768, 32767); break;
        case FieldType::U16: bump_int<uint16_t>(r.ptr, d, 0, 65535); break;
        case FieldType::I32: bump_int<int32_t>(r.ptr, d, -2147483647L, 2147483647L); break;
        case FieldType::U32: bump_int<uint32_t>(r.ptr, d, 0, 2147483647L); break;
        case FieldType::Bool: { bool v; std::memcpy(&v, r.ptr, sizeof(v)); v = !v; std::memcpy(r.ptr, &v, sizeof(v)); break; }
        case FieldType::Scalar: {
            scalar v; std::memcpy(&v, r.ptr, sizeof(v));
            v = v + s_from_int(int(d));
            std::memcpy(r.ptr, &v, sizeof(v));
            break;
        }
        case FieldType::Hash: break;
    }
}

void trace_frame(App& app, int steps) {
    if (!g_dev.trace_path) return;
    if (!g_dev.trace) {
        g_dev.trace = std::fopen(g_dev.trace_path, "wb");
        if (!g_dev.trace) { g_dev.trace_path = nullptr; return; }
        std::fprintf(g_dev.trace, "frame,update_us,render_us,present_us,frame_us,budget_us,steps,entities,sprites\n");
    }
    const FrameProfile& p = app.profile();                   // the previous frame's phases
    const uint32_t sprites = app.render().stats().sprites_submitted;   // ... and its sprites
    std::fprintf(g_dev.trace, "%lu,%u,%u,%u,%u,%u,%d,%u,%u\n", (unsigned long)app.frame(), unsigned(p.update_us),
                 unsigned(p.render_us), unsigned(p.present_us), unsigned(p.frame_us), unsigned(p.budget_us), steps,
                 unsigned(app.world().count()), unsigned(sprites));
    if (g_dev.frame % 60 == 0) std::fflush(g_dev.trace);
}

int dev_frame(void*, App& app, int steps) {
    ++g_dev.frame;
    // the frame-time history (the previous frame's)
    if (app.frame() > 0) {
        const FrameProfile& p = app.profile();
        g_dev.frame_us[g_dev.ring_at] = p.frame_us;
        g_dev.update_us[g_dev.ring_at] = p.update_us;
        g_dev.render_us[g_dev.ring_at] = p.render_us;
        g_dev.ring_at = (g_dev.ring_at + 1) % kRing;
        if (g_dev.ring_n < kRing) ++g_dev.ring_n;
    }
    phx_desktop_event ev;
    while (phx_desktop_poll(&ev)) {
        if (ev.kind == PHX_DEV_KEY_DOWN) {
            const bool shift = (ev.mods & PHX_MOD_SHIFT) != 0;
            if (!ev.repeat) switch (ev.key) {
                case PHX_KEY_F1: g_dev.shown = !g_dev.shown; break;
                case PHX_KEY_F2: g_dev.boxes = !g_dev.boxes; g_dev.shown = true; break;
                case PHX_KEY_F3: g_dev.slow = (g_dev.slow + 1) % 3; g_dev.shown = true; break;
                case PHX_KEY_F5: g_dev.paused = !g_dev.paused; g_dev.halt = nullptr; g_dev.shown = true; break;
                case PHX_KEY_F6: g_dev.paused = true; g_dev.shown = true; ++g_dev.step; break;
                case PHX_KEY_F7: select_step(app, -1); g_dev.shown = true; break;
                case PHX_KEY_F8: select_step(app, +1); g_dev.shown = true; break;
                case PHX_KEY_F9: g_dev.halt_on_warn = !g_dev.halt_on_warn; g_dev.shown = true; break;
                default: break;
            }
            if (g_dev.shown) {                                   // the inspector's field cursor + edits
                EditRef refs[kMaxEdits];
                const int n = int(collect_edits(app.world(), g_dev.sel, refs, kMaxEdits));
                if (ev.key == PHX_KEY_PAGE_UP && n) g_dev.cursor = (g_dev.cursor + n - 1) % n;
                if (ev.key == PHX_KEY_PAGE_DOWN && n) g_dev.cursor = (g_dev.cursor + 1) % n;
                if ((ev.key == '-' || ev.key == '=') && n && g_dev.cursor < n)
                    edit_value(refs[g_dev.cursor], ev.key == '=' ? 1 : -1, shift);
            }
        } else if (ev.kind == PHX_DEV_MOUSE_DOWN && ev.button == PHX_MOUSE_LEFT && g_dev.shown) {
            select_at(app, ev.x, ev.y);
        }
    }
    // pause when the engine logs a warning or an error (F9)
    const uint32_t warns = log_count(LogLevel::Warn) + log_count(LogLevel::Error);
    if (g_dev.halt_on_warn && warns > g_dev.warn_seen) {
        g_dev.paused = true; g_dev.shown = true;
        g_dev.halt = log_count(LogLevel::Error) ? "halted: the engine logged an error" : "halted: the engine logged a warning";
    }
    g_dev.warn_seen = warns;

    int run = steps;
    if (g_dev.paused) run = g_dev.step > 0 ? (--g_dev.step, 1) : 0;
    else if (g_dev.slow) run = (g_dev.frame % (1u << g_dev.slow)) == 0 ? steps : 0;   // 1/2, 1/4 speed
    trace_frame(app, run);
    return run;
}

// ---- drawing into the software framebuffer ----
struct Canvas {
    phx_soft_fb fb;
    void shade(int x0, int y0, int x1, int y1) {                  // darken a box (a backdrop)
        for (int y = y0 < 0 ? 0 : y0; y < y1 && y < fb.h; ++y)
            for (int x = x0 < 0 ? 0 : x0; x < x1 && x < fb.w; ++x) {
                uint32_t& p = fb.pixels[size_t(y) * size_t(fb.w) + size_t(x)];
                p = ((p >> 2) & 0x003F3F3Fu) | 0xFF000000u;
            }
    }
    void put(int x, int y, uint32_t c) {
        if (x >= 0 && y >= 0 && x < fb.w && y < fb.h) fb.pixels[size_t(y) * size_t(fb.w) + size_t(x)] = c;
    }
    void fill(int x0, int y0, int x1, int y1, uint32_t c) {
        for (int y = y0; y <= y1; ++y) for (int x = x0; x <= x1; ++x) put(x, y, c);
    }
    void box(int x0, int y0, int x1, int y1, uint32_t c) {
        for (int x = x0; x <= x1; ++x) { put(x, y0, c); put(x, y1, c); }
        for (int y = y0; y <= y1; ++y) { put(x0, y, c); put(x1, y, c); }
    }
    int text(int x, int y, const char* s, uint32_t c) {
        for (; *s; ++s, x += 6) {
            const int g = (*s >= 32 && *s < 127) ? *s - 32 : 95;
            for (int r = 0; r < 7; ++r)
                for (int b = 0; b < 5; ++b)
                    if (devfont::kGlyph5x7[g][r] & (0x10 >> b)) put(x + b, y + r, c);
        }
        return x;
    }
};

// 0xAABBGGRR (the soft framebuffer's RGBA bytes)
constexpr uint32_t kWhite = 0xFFFFFFFFu, kDim = 0xFFB0B0B0u, kAccent = 0xFF3088FFu, kGood = 0xFF60D060u,
                   kWarn = 0xFF30D0FFu, kBad = 0xFF3030FFu, kSky = 0xFFFFC040u, kGrey = 0xFF808080u;
constexpr uint32_t kLayerColour[8] = { 0xFF60D060u, 0xFF30D0FFu, 0xFF3030FFu, 0xFFFFC040u,
                                       0xFFFF60E0u, 0xFF3088FFu, 0xFFFFFFFFu, 0xFFA0A0FFu };

double sd(scalar v) { return s_to_double(v); }

// One reflected field's value as text.
void field_text(const FieldInfo& f, const uint8_t* d, char* out, size_t cap) {
    const uint8_t* p = d + f.offset;
    switch (f.type) {
        case FieldType::I8:   { int8_t v;   std::memcpy(&v, p, 1); std::snprintf(out, cap, "%d", int(v)); break; }
        case FieldType::U8:   { uint8_t v;  std::memcpy(&v, p, 1); std::snprintf(out, cap, "%u", unsigned(v)); break; }
        case FieldType::I16:  { int16_t v;  std::memcpy(&v, p, 2); std::snprintf(out, cap, "%d", int(v)); break; }
        case FieldType::U16:  { uint16_t v; std::memcpy(&v, p, 2); std::snprintf(out, cap, "%u", unsigned(v)); break; }
        case FieldType::I32:  { int32_t v;  std::memcpy(&v, p, 4); std::snprintf(out, cap, "%ld", long(v)); break; }
        case FieldType::U32:  { uint32_t v; std::memcpy(&v, p, 4); std::snprintf(out, cap, "%lu", (unsigned long)v); break; }
        case FieldType::Bool: { bool v;     std::memcpy(&v, p, sizeof(v)); std::snprintf(out, cap, "%s", v ? "true" : "false"); break; }
        case FieldType::Scalar: { scalar v; std::memcpy(&v, p, sizeof(v)); std::snprintf(out, cap, "%.2f", sd(v)); break; }
        case FieldType::Hash: { uint32_t v; std::memcpy(&v, p, 4); std::snprintf(out, cap, "#%08lx", (unsigned long)v); break; }
    }
}

// F2: every collider (coloured by its lowest layer bit) and the level's collision tiles.
void draw_boxes(App& app, Canvas& cv) {
    const Camera2D& cam = app.render().camera();
    const scalar z = cam.zoom > s_from_int(0) ? cam.zoom : s_from_int(1);
    auto sx = [&](scalar wx) { return s_to_int((wx - cam.pos.x) * z); };
    auto sy = [&](scalar wy) { return s_to_int((wy - cam.pos.y) * z); };
    if (const Level* lv = Level::active()) {
        const TileGrid& g = lv->grid();
        if (g.tiles && g.tile_w > 0 && g.tile_h > 0) {
            const int tx0 = s_to_int(cam.pos.x) / g.tile_w - 1, ty0 = s_to_int(cam.pos.y) / g.tile_h - 1;
            const int tx1 = tx0 + s_to_int(s_from_int(cv.fb.w) / z) / g.tile_w + 3;
            const int ty1 = ty0 + s_to_int(s_from_int(cv.fb.h) / z) / g.tile_h + 3;
            for (int ty = ty0 < 0 ? 0 : ty0; ty < ty1 && ty < g.h; ++ty)
                for (int tx = tx0 < 0 ? 0 : tx0; tx < tx1 && tx < g.w; ++tx) {
                    const uint8_t f = g.flags_at(tx, ty);
                    if (!f) continue;
                    const int x0 = sx(s_from_int(tx * g.tile_w)), y0 = sy(s_from_int(ty * g.tile_h));
                    const int x1 = sx(s_from_int((tx + 1) * g.tile_w)) - 1, y1 = sy(s_from_int((ty + 1) * g.tile_h)) - 1;
                    if (f & kTileHazard) cv.box(x0, y0, x1, y1, kBad);
                    else if (f & kTileSolid) cv.box(x0, y0, x1, y1, kGrey);
                    else if (f & kTileOneWay) for (int x = x0; x <= x1; ++x) cv.put(x, y0, kSky);
                }
        }
    }
    ecs::World& w = app.world();
    w.each<Transform>([&](ecs::Entity e, Transform& t) {
        const AABBColl* c = w.get<AABBColl>(e);
        if (!c) return;
        int bit = 0;
        while (bit < 7 && !(c->layer & (1u << bit))) ++bit;
        cv.box(sx(t.pos.x - c->half.x), sy(t.pos.y - c->half.y), sx(t.pos.x + c->half.x) - 1, sy(t.pos.y + c->half.y) - 1,
               kLayerColour[bit]);
    });
}

// The frame-time graph (bottom right): one bar per frame of WORK (update + render: the frame minus
// the present, which holds the vsync wait), the step budget as a line.
void draw_graph(App& app, Canvas& cv) {
    const int gw = int(kRing), gh = 28;
    const int x0 = cv.fb.w - gw - 2, y1 = cv.fb.h - 2, y0 = y1 - gh;
    if (x0 < 0 || y0 < 10) return;
    cv.shade(x0 - 1, y0 - 10, cv.fb.w, cv.fb.h);
    const uint32_t budget = app.profile().budget_us ? app.profile().budget_us : 16667u;
    const uint32_t scale = budget * 2;                          // the graph's top = 2 budgets
    uint32_t worst = 0, sum = 0;
    for (uint32_t i = 0; i < g_dev.ring_n; ++i) {
        const uint32_t k = (g_dev.ring_at + kRing - g_dev.ring_n + i) % kRing;
        const uint32_t us = g_dev.update_us[k] + g_dev.render_us[k];
        worst = us > worst ? us : worst;
        sum += us;
        const int h = int((uint64_t(us < scale ? us : scale) * uint64_t(gh)) / scale);
        const uint32_t c = us <= budget ? kGood : us <= budget + budget / 2 ? kWarn : kBad;
        cv.fill(x0 + int(i), y1 - h, x0 + int(i), y1, c);
    }
    const int by = y1 - gh / 2;
    for (int x = x0; x < x0 + gw; x += 2) cv.put(x, by, kWhite);
    char line[48];
    const uint32_t last = g_dev.ring_n ? g_dev.frame_us[(g_dev.ring_at + kRing - 1) % kRing] : 0;
    const uint32_t lu = g_dev.ring_n ? g_dev.update_us[(g_dev.ring_at + kRing - 1) % kRing] : 0;
    const uint32_t lr = g_dev.ring_n ? g_dev.render_us[(g_dev.ring_at + kRing - 1) % kRing] : 0;
    std::snprintf(line, sizeof(line), "work %.2fms (u%.2f r%.2f) / %.1f", (lu + lr) / 1000.0, lu / 1000.0, lr / 1000.0,
                  last / 1000.0);
    cv.text(x0, y0 - 9, line, g_dev.ring_n && sum / g_dev.ring_n > budget ? kBad : kDim);
    (void)worst;
}

void dev_overlay(void*, App& app) {
    if (!g_dev.shown || !app.platform() || !app.platform()->gfx) return;
    Canvas cv{ phx_gfx_soft_lock(app.platform()->gfx()) };
    if (!cv.fb.pixels || cv.fb.w < 60 || cv.fb.h < 40) return;
    ecs::World& w = app.world();
    if (g_dev.sel != ecs::kInvalid && !w.is_alive(g_dev.sel)) g_dev.sel = ecs::kInvalid;
    if (g_dev.sel == ecs::kInvalid) select_step(app, +1);
    if (g_dev.boxes) draw_boxes(app, cv);

    // the selected entity's collider, outlined in the world
    const Camera2D& cam = app.render().camera();
    const scalar z = cam.zoom > s_from_int(0) ? cam.zoom : s_from_int(1);
    if (const Transform* t = g_dev.sel != ecs::kInvalid ? w.get<Transform>(g_dev.sel) : nullptr) {
        const AABBColl* c = w.get<AABBColl>(g_dev.sel);
        const scalar hx = c ? c->half.x : s_from_int(4), hy = c ? c->half.y : s_from_int(4);
        cv.box(s_to_int((t->pos.x - hx - cam.pos.x) * z), s_to_int((t->pos.y - hy - cam.pos.y) * z),
               s_to_int((t->pos.x + hx - cam.pos.x) * z) - 1, s_to_int((t->pos.y + hy - cam.pos.y) * z) - 1, kAccent);
    }
    if (cv.fb.w >= 200 && cv.fb.h >= 80) draw_graph(app, cv);

    // the panel: status, then the inspector
    char line[64];
    const int pw = cv.fb.w < 200 ? cv.fb.w : 200;
    int y = 2;
    const int lines_max = (cv.fb.h - 4) / 8;
    int shown_lines = 0;
    auto say = [&](const char* s, uint32_t c) {
        if (shown_lines >= lines_max) return;
        cv.shade(0, y - 1, pw, y + 7);
        cv.text(2, y, s, c);
        y += 8; ++shown_lines;
    };
    static const char* const kSpeed[3] = { "", " x1/2", " x1/4" };
    std::snprintf(line, sizeof(line), "%s%s%s%s", g_dev.paused ? "PAUSED" : "DEV", kSpeed[g_dev.slow],
                  g_dev.boxes ? " boxes" : "", g_dev.halt_on_warn ? " halt-on-warn" : "");
    say(line, g_dev.paused ? kAccent : kGood);
    if (g_dev.halt) say(g_dev.halt, kBad);
    say("F2 box F3 slow F5 pause F6 step", kDim);          // (<= 32 columns: the panel's width)
    say("F7/8 pick F9 halt PgUp/Dn -= edit", kDim);
    std::snprintf(line, sizeof(line), "frame %lu  entities %u", (unsigned long)app.frame(), unsigned(w.count()));
    say(line, kDim);
    const ecs::Entity e = g_dev.sel;
    if (e == ecs::kInvalid) { say("no entity selected", kDim); return; }

    const PrefabRef* pr = w.get<PrefabRef>(e);
    const Level* lv = Level::active();
    const char* type = pr && lv ? lv->prefabs().get_str(pr->row, "type"_hash) : nullptr;
    if (pr) std::snprintf(line, sizeof(line), "#%lu %s  spawn %u", (unsigned long)(e & 0xFFFFFFu), type ? type : "?", unsigned(pr->spawn));
    else    std::snprintf(line, sizeof(line), "#%lu", (unsigned long)(e & 0xFFFFFFu));
    say(line, kWhite);

    // the editable values, with the cursor (PgUp / PgDn) and -/= to change the one under it
    EditRef refs[kMaxEdits];
    const uint32_t n = collect_edits(w, e, refs, kMaxEdits);
    if (n && g_dev.cursor >= int(n)) g_dev.cursor = int(n) - 1;
    const char* comp = nullptr;
    for (uint32_t i = 0; i < n; ++i) {
        if (!comp || std::strcmp(comp, refs[i].comp) != 0) { comp = refs[i].comp; say(comp, kGood); }
        char val[32];
        const FieldInfo fi{ refs[i].name, refs[i].type, 0 };
        field_text(fi, static_cast<const uint8_t*>(refs[i].ptr), val, sizeof(val));
        const bool at = int(i) == g_dev.cursor;
        std::snprintf(line, sizeof(line), "%s %s %s", at ? ">" : " ", refs[i].name, val);
        say(line, at ? kAccent : kDim);
    }
    if (const AABBColl* c = w.get<AABBColl>(e)) {
        std::snprintf(line, sizeof(line), " box %.0fx%.0f layer %u mask %u", sd(c->half.x) * 2, sd(c->half.y) * 2, unsigned(c->layer), unsigned(c->mask));
        say(line, kDim);
    }
    if (const Animator* an = w.get<Animator>(e)) {
        std::snprintf(line, sizeof(line), " clip %u frame %u", unsigned(an->clip), unsigned(an->frame));
        say(line, kDim);
    }
}

void dev_stop(void*, App&) {
    if (g_dev.trace) { std::fclose(g_dev.trace); g_dev.trace = nullptr; }
}

} // namespace

void install_devtools(App& app) {
    g_dev = DevState{};
    const char* tp = std::getenv("PHX_TRACE");
    g_dev.trace_path = tp && *tp ? tp : nullptr;
    g_dev.warn_seen = log_count(LogLevel::Warn) + log_count(LogLevel::Error);   // count from here on
    DevHooks h;
    h.frame = &dev_frame;
    h.overlay = &dev_overlay;
    h.stop = &dev_stop;
    app.set_dev_hooks(h);
}

} // namespace phx
