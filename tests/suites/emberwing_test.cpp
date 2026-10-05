// tests/emberwing_test.cpp — a headless, deterministic verification of the Emberwing vertical
// slice (examples/emberwing) under the real fixed-step loop: bundle bake+mount, ECS, physics,
// the three enemy behaviours, pickups/triggers, checkpoint respawn, the goal path, HUD render
// and the audio pipeline. Three scripted runs:
//   1. a playthrough of the opening sections (movement, embers, pause+resume, save),
//   2. a goal run (the player is placed on the final plateau; walking right must trigger the
//      Sungate: clear scene pushed, tally saved),
//   3. a fresh boot that must restore the saved best run.
// Counters are deterministic; assertions are invariants that hold on BOTH scalar tiers.
#include "../../examples/emberwing/src/game.h"
#include "../../examples/emberwing/src/bake.h"
#include "phx/platform/platform.h"
#include "phx/platform/gfx_soft.h"

#include <cstdio>
#include <string>

extern "C" void phx_null_set_step_ns(uint64_t);
extern "C" void phx_null_set_max_frames(uint64_t);
extern "C" void phx_null_set_button_script(const uint32_t*, uint32_t);

using namespace phx;
using namespace game;

namespace {
// The PPU build of this suite (phx_emberwing_ppu) bakes and saves to its own files: `make check -j`
// runs the soft and PPU builds at once, and a shared save let one run clobber the other's. The soft
// build keeps build/emberwing.phxp (Phosphorus Studio and examples/emberwing/phxproject.json open it).
std::string g_bundle = "build/emberwing.phxp", g_save = "build/emberwing.sav";
const char* kBundle = nullptr;
const char* kSave   = nullptr;
int g_fail = 0;

void CK(bool c, const char* m) { if (!c) { ++g_fail; std::printf("    FAIL %s\n", m); } }

Config make_cfg() {
    Config cfg = Config::from_defaults();
    cfg.sim_hz = 60; cfg.title = "emberwing";
    cfg.width = 240; cfg.height = 160;
    cfg.total_ram = 32u << 20; cfg.frame_scratch = 256u << 10; cfg.max_entities = 1024;
    return cfg;
}

// Run 1: title -> level, then bunny-hop right through the opening sections while holding A
// long enough for full-height jumps; pause (saves) and resume near the end.
struct PlayGame final : EmberwingGame {
    void on_stop(App& app) override {
        std::printf("    x=%d y=%d hp=%d score=%d embers=%d stomps=%d deaths=%d cp=%d sfx=%d peak=%d depth=%u\n",
                    player_x(), player_y(), health(), score(), embers(), stomps(),
                    deaths(), checkpoints(), sfx_count(), audio_peak_level(), depth());
        CK(score() > 0,           "collected something (score > 0)");
        CK(embers() >= 3,         "collected at least 3 embers");
        CK(player_x() > 400,      "progressed past the column section (x > 400)");
        CK(depth() == 1,          "scene stack back to the level after pause+resume");
        CK(sfx_count() > 0,       "SFX intents were pushed (jump/pickup)");
        CK(audio_peak_level() > 0, "the mixer produced non-silent output (music + SFX)");
        CK(health() >= 1 && health() <= 3, "health stayed in range");

        // The HUD hearts draw camera-anchored at screen (2,2): the heart sprite's known
        // texel colours must be in the framebuffer regardless of world scroll. The same
        // binary runs over the soft AND the GBA PPU backend, so accept each colour either
        // exact or as it survives the PPU's 15-bit BGR555 path.
        auto q555 = [](Rgba c) {
            auto q = [](uint32_t v) { v >>= 3; return uint8_t((v << 3) | (v >> 2)); };
            return rgba(q(rgba_r(c)), q(rgba_g(c)), q(rgba_b(c)));
        };
        const Rgba heart[4] = { rgba(255, 80, 96), q555(rgba(255, 80, 96)),
                                rgba(200, 40, 64), q555(rgba(200, 40, 64)) };
        phx_soft_fb fb = phx_gfx_soft_lock(app.platform()->gfx());
        bool hud = false;
        for (int y = 2; y < 11 && !hud; ++y)
            for (int x = 2; x < 30 && !hud; ++x)
                for (Rgba want : heart)
                    if (fb.pixels[y * fb.w + x] == want) { hud = true; break; }
        CK(hud, "HUD hearts rendered to the framebuffer");
    }
};

// Run 2: the goal path. After the level spawns, the harness moves the player to the final
// plateau (a test-only manipulation through the game-owned context); walking right must
// touch the Sungate: clear scene pushed, tally + clears persisted.
struct GoalGame final : EmberwingGame {
    void on_fixed_update(App& app, scalar dt) override {
        if (app.frame() == 30 && ctx.player != ecs::kInvalid) {
            if (Transform* t = ctx.world->get<Transform>(ctx.player))
                t->pos = vec2{ s_from_int(2390), s_from_int(80) };   // final plateau, pre-gate
            ctx.respawn = vec2{ s_from_int(2390), s_from_int(80) };
        }
        EmberwingGame::on_fixed_update(app, dt);
    }
    void on_stop(App&) override {
        std::printf("    x=%d cleared=%d depth=%u score=%d\n",
                    player_x(), int(cleared()), depth(), score());
        CK(cleared(),            "the Sungate fired (level cleared)");
        CK(depth() == 2,         "the clear tally scene is on the stack");
        CK(score() >= tune::kHpBonus, "the HP bonus was tallied into the score");
    }
};

// Run 3: a fresh boot that must restore the saved best run.
struct LoadGame final : EmberwingGame {
    void on_stop(App&) override {
        std::printf("    loaded=%d best=%u clears=%u\n",
                    int(was_loaded()), unsigned(ctx.best.best_score), unsigned(ctx.best.clears));
        CK(was_loaded(),             "a saved game was restored at boot (magic/version valid)");
        CK(ctx.best.best_score > 0,  "restored best score is non-trivial");
        CK(ctx.best.clears >= 1,     "the goal run's clear was persisted");
    }
};
} // namespace

int main(int argc, char** argv) {
    if (argc > 0 && argv[0]) {
        std::string stem = argv[0];
        const size_t sl = stem.find_last_of("/\\");
        if (sl != std::string::npos) stem = stem.substr(sl + 1);
        if (stem.size() > 4 && stem.compare(stem.size() - 4, 4, ".exe") == 0) stem.resize(stem.size() - 4);
        if (stem.find("ppu") != std::string::npos) { g_bundle = "build/emberwing_ppu.phxp"; g_save = "build/emberwing_ppu.sav"; }
    }
    kBundle = g_bundle.c_str();
    kSave = g_save.c_str();
    if (!bake_emberwing_assets(kBundle)) { std::printf("EMBERWING FAIL (bake)\n"); return 1; }
    std::remove(kSave);

    // --- Run 1: the opening playthrough --------------------------------------------------
    {
        constexpr int FRAMES = 900;
        static uint32_t script[FRAMES];
        for (auto& s : script) s = 0;
        script[1] = button_bit(Button::Start);                       // title -> level
        for (int f = 5; f < 860; ++f) script[f] |= button_bit(Button::Right);
        for (int f = 20; f < 840; f += 26)                           // held hops (full height)
            for (int k = 0; k < 14; ++k) script[f + k] |= button_bit(Button::A);
        script[870] = button_bit(Button::Start);                     // pause -> save
        script[880] = button_bit(Button::Start);                     // resume

        phx_null_set_step_ns(1000000000ull / 60);
        phx_null_set_max_frames(FRAMES);
        phx_null_set_button_script(script, FRAMES);

        App app(make_cfg());
        PlayGame game; game.bundle = kBundle; game.save_path = kSave;
        if (app.run(&game) != 0) { std::printf("EMBERWING FAIL (run1 rc)\n"); return 1; }
        std::printf("emberwing run1 done\n");
        if (g_fail) { std::printf("EMBERWING FAIL\n\n"); return 1; }
    }

    // --- Run 2: the goal ------------------------------------------------------------------
    {
        constexpr int FRAMES = 300;
        static uint32_t script[FRAMES];
        for (auto& s : script) s = 0;
        script[1] = button_bit(Button::Start);
        for (int f = 40; f < 280; ++f) script[f] |= button_bit(Button::Right);

        phx_null_set_max_frames(FRAMES);
        phx_null_set_button_script(script, FRAMES);

        App app(make_cfg());
        GoalGame game; game.bundle = kBundle; game.save_path = kSave;
        if (app.run(&game) != 0) { std::printf("EMBERWING FAIL (run2 rc)\n"); return 1; }
        std::printf("emberwing run2 done\n");
        if (g_fail) { std::printf("EMBERWING FAIL\n\n"); return 1; }
    }

    // --- Run 3: restore the save ------------------------------------------------------------
    {
        static uint32_t idle[8] = { 0 };
        phx_null_set_max_frames(8);
        phx_null_set_button_script(idle, 8);

        App app(make_cfg());
        LoadGame game; game.bundle = kBundle; game.save_path = kSave;
        if (app.run(&game) != 0) { std::printf("EMBERWING FAIL (run3 rc)\n"); return 1; }
        std::printf("emberwing run3 done\n");
        if (g_fail) { std::printf("EMBERWING FAIL\n\n"); return 1; }
    }

    std::printf("EMBERWING PASS\n\n");
    return 0;
}
