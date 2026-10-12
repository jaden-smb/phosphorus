// tests/suites/flow_test.cpp — the game flow (phx/runtime/flow.h) as data: a flow table (title ->
// level one -> level two with one life -> gameover -> end), two maps and a font, baked by the real
// converters and played with scripted input. Checks the screens chain as authored (START leaves a
// title, an Exit ends a level, running out of lives goes to "gameover", `next` names the retry),
// counters bank across levels, a revisited map reuses its tilemap slot, START pauses a level, the
// HUD draws, and an end screen restarts the game with the totals cleared. Both scalar tiers.
#include "phx/runtime/app.h"
#include "phx/runtime/flow.h"
#include "phx/runtime/font.h"
#include "phx/platform/gfx_soft.h"

#include "builders.h"                 // tools/phxpack: the converters' bake logic
#include "editor.h"                   // tools/phxtmap: TmapDoc
#include "ascii_font.h"               // tools/common: the 5x7 font atlas
#include "png_write.h"                // tools/common

#include <cstdio>
#include <cstring>
#include <vector>
#include <string>

extern "C" void phx_null_set_max_frames(uint64_t n);
extern "C" void phx_null_set_button_script(const uint32_t* masks, uint32_t n);

using namespace phx;

namespace {
int g_checks = 0, g_fail = 0;
void check(bool ok, const char* what) { ++g_checks; if (!ok) { ++g_fail; std::printf("    FAIL %s\n", what); } }
void write_file(const char* path, const char* s) {
    if (FILE* f = std::fopen(path, "wb")) { std::fwrite(s, 1, std::strlen(s), f); std::fclose(f); }
}

const char* kFlow =
"{ \"struct\":\"Screen\","
"  \"fields\":[ {\"name\":\"name\",\"type\":\"str16\"}, {\"name\":\"kind\",\"type\":\"str16\"}, {\"name\":\"map\",\"type\":\"str16\"},"
"              {\"name\":\"text\",\"type\":\"str64\"}, {\"name\":\"next\",\"type\":\"str16\"}, {\"name\":\"lives\",\"type\":\"u8\"} ],"
"  \"records\":[ {\"name\":\"title\",\"kind\":\"title\",\"text\":\"FLOW TEST|press start\"},"
"               {\"name\":\"one\",\"kind\":\"level\",\"map\":\"l1\",\"text\":\"LEVEL 1\"},"
"               {\"name\":\"two\",\"kind\":\"level\",\"map\":\"l2\",\"lives\":1},"
"               {\"name\":\"gameover\",\"kind\":\"title\",\"text\":\"GAME OVER\",\"next\":\"two\"},"
"               {\"name\":\"end\",\"kind\":\"end\",\"text\":\"THE END\"} ] }";
const char* kPrefabs =
"{ \"struct\":\"Prefab\","
"  \"fields\":[ {\"name\":\"type\",\"type\":\"str16\"}, {\"name\":\"w\",\"type\":\"u8\"}, {\"name\":\"h\",\"type\":\"u8\"},"
"              {\"name\":\"body\",\"type\":\"u8\"}, {\"name\":\"layer\",\"type\":\"u16\"}, {\"name\":\"mask\",\"type\":\"u16\"},"
"              {\"name\":\"components\",\"type\":\"str64\"}, {\"name\":\"Pickup_value\",\"type\":\"i16\"} ],"
"  \"records\":[ {\"type\":\"player\",\"w\":6,\"h\":8,\"body\":1,\"layer\":1,\"mask\":14,\"components\":\"PlatformerController\"},"
"               {\"type\":\"coin\",\"w\":4,\"h\":4,\"layer\":2,\"mask\":1,\"components\":\"Pickup\",\"Pickup_value\":2},"
"               {\"type\":\"door\",\"w\":4,\"h\":8,\"layer\":8,\"mask\":1,\"components\":\"Exit\"},"
"               {\"type\":\"spike\",\"w\":4,\"h\":8,\"layer\":4,\"mask\":1,\"components\":\"Hazard\"} ] }";

struct HudCase {
    std::string label, expected;
    int32_t value = 0, lives = 0, deaths = 0;
    const char* expected_lives = nullptr;
};
const HudCase kHudCases[] = {
    { "", "COINS 0" },
    { "COINS", "COINS -1", -1 },
    { std::string(15, 'A'), std::string(15, 'A') + " 2147483647", INT32_MAX }, // template str16 maximum
    { std::string(31, 'B'), std::string(31, 'B') + " 0" }, // str32 maximum
    { std::string(63, 'C'), std::string(27, 'C') + " -2147483648", INT32_MIN }, // maximum authorable str64
    { std::string(26, 'D'), std::string(26, 'D') + " -2147483648", INT32_MIN },
    { std::string(27, 'E'), std::string(27, 'E') + " -2147483648", INT32_MIN }, // exactly 39 bytes
    { std::string(28, 'F'), std::string(27, 'F') + " -2147483648", INT32_MIN }, // one byte over
    { std::string(28, 'G'), std::string(28, 'G') + " 2147483647", INT32_MAX },
    { std::string(29, 'H'), std::string(28, 'H') + " 2147483647", INT32_MAX },
    { std::string(37, 'I'), std::string(37, 'I') + " 0" },
    { std::string(38, 'J'), std::string(37, 'J') + " 0" },
    { "COINS", "COINS 0", 0, 1, 1, "LIVES 0" },
    { "COINS", "COINS 0", 0, 1, 2, "LIVES -1" },
    { "COINS", "COINS 0", 0, INT32_MAX, 0, "LIVES 2147483647" },
    { "COINS", "COINS 0", 0, INT32_MIN, 0, "LIVES -2147483648" },
    { "COINS", "COINS 0", 0, INT32_MAX, INT32_MIN, "LIVES 4294967295" },
    { "COINS", "COINS 0", 0, INT32_MIN, INT32_MAX, "LIVES -4294967295" },
};

bool bake_hud(phxtool::BundleWriter& w) {
    // Author JSON directly: exercise runtime safety without Studio validation (including
    // i32 lives, rather than the template's u8). The real baker supplies str64 termination.
    std::string json = "{\"struct\":\"Screen\",\"fields\":["
        "{\"name\":\"name\",\"type\":\"str16\"},{\"name\":\"kind\",\"type\":\"str16\"},"
        "{\"name\":\"map\",\"type\":\"str16\"},{\"name\":\"label\",\"type\":\"str64\"},"
        "{\"name\":\"lives\",\"type\":\"i32\"}],\"records\":[";
    for (size_t i = 0; i < sizeof(kHudCases) / sizeof(kHudCases[0]); ++i) {
        if (i) json += ',';
        json += "{\"name\":\"hud" + std::to_string(i) + "\",\"kind\":\"level\",\"map\":\"l2\",\"label\":\"" +
            kHudCases[i].label + "\",\"lives\":" + std::to_string(kHudCases[i].lives) + "}";
    }
    json += "]}";
    write_file("build/f_hud.json", json.c_str());
    return phxtool::build_bin(w, "build/f_hud.json", "hudflow");
}

bool bake() {
    std::vector<uint32_t> atlas(size_t(phxtool::kAsciiFontW) * phxtool::kAsciiFontH);
    phxtool::build_ascii_font(atlas.data());
    if (!phxtool::png_write_file("build/f_font.png", atlas.data(), phxtool::kAsciiFontW, phxtool::kAsciiFontH)) return false;
    write_file("build/f_flow.json", kFlow);
    write_file("build/f_prefabs.json", kPrefabs);
    auto map = [](const char* path, bool one) {                // a floor, then the spawns
        phxtool::TmapDoc d = phxtool::TmapDoc::blank(16, 6, 8, 8, "font");
        for (int x = 0; x < 16; ++x) d.set_tile(0, x, 5, 1);
        d.set_tile_flag(1, kTileSolid);
        d.add_spawn("player", 20, 36);
        if (one) { d.add_spawn("coin", 40, 36); d.add_spawn("door", 64, 36); }
        else     { d.add_spawn("spike", 60, 36); }
        return d.save_file(path);
    };
    if (!map("build/f_l1.tmj", true) || !map("build/f_l2.tmj", false)) return false;
    phxtool::BundleWriter w(2);
    return phxtool::build_png(w, "build/f_font.png", "font") && phxtool::build_tmj(w, "build/f_l1.tmj", "l1") &&
           phxtool::build_tmj(w, "build/f_l2.tmj", "l2") && phxtool::build_bin(w, "build/f_flow.json", "flow") &&
           phxtool::build_bin(w, "build/f_prefabs.json", "prefabs") && bake_hud(w) && w.write("build/f_flow.phxp");
}

constexpr uint32_t kRight = 1u << 3, kStart = 1u << 10;
uint32_t g_script[300];

struct FlowGame final : Game {
    ResourceCache* res = nullptr;
    GameFlow flow;
    Status started = Status::NotFound;
    NameHash seen[16]{};
    uint32_t seen_n = 0, frame = 0;
    int32_t coins_in_two = -1;
    TilemapId two_ids[4]{};
    uint32_t two_n = 0;
    bool paused_seen = false, paused_still = true, hud_drawn = false, restarted = false;
    scalar paused_x{};
    void on_start(App& app) override {
        res = ResourceCache::create(app.mem().persistent()).unwrap();
        check(res->mount(app.platform(), "build/f_flow.phxp") == Status::Ok, "mount the baked flow");
        started = flow.start(app, *res);
        seen[seen_n++] = flow.screen();
    }
    void on_fixed_update(App& app, scalar dt) override {
        ++frame;
        if (frame == 210) check(flow.go(app, "end"_hash), "go() jumps to a named screen");
        flow.update(app, dt);
        if (flow.screen() != seen[seen_n - 1] && seen_n < 16) {
            seen[seen_n++] = flow.screen();
            if (flow.screen() == "two"_hash && two_n < 4) two_ids[two_n++] = flow.level().map();
            if (flow.screen() == "two"_hash && coins_in_two < 0) coins_in_two = flow.total("coins"_hash);
            if (flow.screen() == "title"_hash && seen_n > 2) restarted = flow.total("coins"_hash) == 0;
        }
        if (flow.paused()) {
            const Transform* t = app.world().get<Transform>(flow.behaviours().player());
            if (!paused_seen && t) paused_x = t->pos.x;
            paused_seen = true;
            if (t && t->pos.x != paused_x) paused_still = false;
        }
    }
    void on_render(App& app, scalar) override {
        flow.render(app);
        if (flow.in_level() && app.render().stats().sprites_submitted > 0) hud_drawn = true;
    }
};

struct HudGame final : Game {
    ResourceCache* res = nullptr;
    GameFlow flow;
    BitmapFont font;
    UI ui;
    void on_start(App& app) override {
        res = ResourceCache::create(app.mem().persistent()).unwrap();
        check(res->mount(app.platform(), "build/f_flow.phxp") == Status::Ok, "mount HUD fixtures");
        FlowOptions opt; opt.table = "hudflow"_hash;
        check(flow.start(app, *res, opt) == Status::Ok, "start HUD fixtures");
        check(load_font(app.render(), *res, "font"_hash, font), "load reference HUD font");
    }
    void on_render(App& app, scalar) override {
        for (size_t i = 0; i < sizeof(kHudCases) / sizeof(kHudCases[0]); ++i) {
            const HudCase& c = kHudCases[i];
            const std::string name = "hud" + std::to_string(i);
            check(flow.go(app, fnv1a_n(name.c_str(), name.size())), "enter HUD fixture level");
            flow.vars().set("coins"_hash, c.value);
            flow.behaviours().set_counter("deaths"_hash, c.deaths);
            flow.render(app);
            const phx_soft_fb fb = phx_gfx_soft_lock(app.platform()->gfx());
            const std::vector<uint32_t> actual(fb.pixels, fb.pixels + size_t(fb.w) * fb.h);

            // Independently render the expected strings over the same scene. Comparing the
            // complete frame catches lost digits, unterminated text and alignment changes.
            Renderer& r = app.render();
            const Camera2D cam = flow.behaviours().camera();
            auto at = [&](int x, int y) { return cam.pos + vec2{ s_from_int(x), s_from_int(y) }; };
            r.begin_frame(cam);
            flow.level().draw(r);
            draw_sprites(app.world(), r);
            ui.begin(r, app.input());
            ui.text(at(4, 4), font, c.expected.c_str(), rgba(255, 190, 80));
            if (c.expected_lives)
                ui.text(at(fb.w - 4 - UI::text_width(font, c.expected_lives), 4), font,
                        c.expected_lives, rgba(255, 190, 80));
            ui.end();
            r.end_frame();
            check(std::memcmp(actual.data(), fb.pixels, actual.size() * sizeof(uint32_t)) == 0, name.c_str());
        }
    }
};
} // namespace

int main() {
    check(bake(), "bake the flow, two maps and a font with the real converters");
    for (uint32_t f = 0; f < 300; ++f) {
        uint32_t m = f > 20 ? kRight : 0u;
        if (f == 20 || f == 150 || f == 158 || f == 228) m |= kStart;   // (a screen ignores START for 10 steps)
        g_script[f] = m;
    }
    phx_null_set_button_script(g_script, 300);
    phx_null_set_max_frames(240);

    Config cfg = Config::from_defaults();
    cfg.title = "flow_test"; cfg.width = 128; cfg.height = 48;
    cfg.total_ram = 8u << 20; cfg.frame_scratch = 256u << 10; cfg.max_entities = 64;
    App app(cfg);
    static FlowGame g;
    check(app.run(&g) == 0, "the App runs the flow");

    check(g.started == Status::Ok, "start(): the flow table is read");
    check(g.seen_n >= 6 && g.seen[0] == "title"_hash && g.seen[1] == "one"_hash && g.seen[2] == "two"_hash &&
          g.seen[3] == "gameover"_hash && g.seen[4] == "two"_hash,
          "title -START-> one -Exit-> two -out of lives-> gameover -START, next=two-> two");
    check(g.coins_in_two == 2, "the coin picked up in level one is banked into the totals");
    check(g.two_n >= 2 && g.two_ids[0] == g.two_ids[1] && g.two_ids[0] != kNoTilemap,
          "revisiting a map reuses its tilemap slot (renderer slots are never freed)");
    check(g.paused_seen && g.paused_still, "START pauses a level: the player stays put while paused");
    check(g.hud_drawn, "the HUD draws in a level");
    bool ended = false;
    for (uint32_t i = 0; i < g.seen_n; ++i) ended = ended || g.seen[i] == "end"_hash;
    check(ended && g.restarted, "an end screen's START restarts at the first row, totals cleared");

    phx_null_set_button_script(nullptr, 0);
    phx_null_set_max_frames(1);
    cfg.width = 512; // Full 39-character counter plus a separate right-aligned lives readout.
    App hud_app(cfg);
    static HudGame hud;
    check(hud_app.run(&hud) == 0, "render all HUD boundary fixtures");

    std::printf("flow_test: %d checks, %d failures\n", g_checks, g_fail);
    std::printf(g_fail ? "FLOW FAIL\n\n" : "FLOW PASS\n\n");
    return g_fail ? 1 : 0;
}
