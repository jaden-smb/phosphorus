// tests/suites/project_entry.cpp — main() for `make project-check`'s headless runs of a game
// project: the engine's desktop entry, except that PHX_PROFILE=gba|psp|desktop picks the target
// profile (budgets + fixed resolution, phx/runtime/main.h) so the host runs the game exactly as
// the console entry would configure it. Linked with the null platform; never a shipping entry.
//
// Input is scripted like a player's: idle, A (past a title screen, or a jump), walking right with
// another A (a jump: the template's jump sound), then walking left. With PHX_EXPECT_AUDIO=1 the run fails unless the game queued a sound
// and the App's headless mix of it was audible (App::audio(): no device on the null platform).
//
// Budget runs (`make project-budget`): PHX_BUDGET_REPORT=file writes the run's budget report
// (phx/runtime/budget.h), and PHX_BUDGET_FRAMES=n repeats the scripted play (A, then running right
// with a jump every 40 frames, then a little left) for n frames.
#include "phx/runtime/main.h"
#include "phx/runtime/budget.h"
#include "phx/runtime/flow.h"
#include "phx/platform/gfx_soft.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" void phx_null_set_button_script(const uint32_t* masks, uint32_t n);

namespace {
constexpr uint32_t kA = 1u << 4, kLeft = 1u << 2, kRight = 1u << 3;   // canonical phx_button bits
uint32_t g_script[4096];
} // namespace

// Exercise the generated project's actual baked flow/maps/prefabs/sprites at the target
// budget. No substitute fixtures: project-check passes its freshly generated bundle.
struct ReloadStress final : phx::Game {
    const char* bundle = nullptr;
    uint32_t cycles = 1000;
    bool passed = false;
    phx::GameFlow flow;

    void on_start(phx::App& app) override {
        using namespace phx;
        auto cache = ResourceCache::create(app.mem().persistent());
        if (!cache || cache.unwrap()->mount(app.platform(), bundle) != Status::Ok ||
            flow.start(app, *cache.unwrap()) != Status::Ok) { app.request_quit(); return; }
        NameHash level_name = 0, title_name = 0;
        for (uint32_t i = 0; i < flow.table().count(); ++i) {
            const NameHash kind = flow.table().get_hash(i, "kind"_hash);
            if (!level_name && kind == "level"_hash) level_name = flow.table().get_hash(i, "name"_hash);
            if (!title_name && kind == "title"_hash) title_name = flow.table().get_hash(i, "name"_hash);
        }
        if (!level_name || !title_name) { app.request_quit(); return; }
        uint32_t expected_hash = 0, entities = 0, sprites = 0, arena_bytes = 0, completed = 0;
        uint16_t live = 0;
        passed = true;
        for (uint32_t cycle = 0; cycle <= cycles; ++cycle) {
            if (!flow.go(app, title_name) || app.world().count() != 0 ||
                !flow.go(app, level_name) || !flow.level().loaded()) { passed = false; break; }
            bool valid = true;
            uint32_t sprite_count = 0;
            app.world().each<SpriteRenderer>([&](ecs::Entity, SpriteRenderer& sr) {
                valid = valid && sr.tex != kNoTexture; ++sprite_count;
            });
            if (!valid || !sprite_count) { passed = false; break; }
            Renderer& r = app.render();
            flow.render(app);
            const phx_soft_fb fb = phx_gfx_soft_lock(app.platform()->gfx());
            uint32_t hash = kFnvOffset;
            for (int y = 0; y < fb.h; ++y)
                for (int x = 0; x < fb.w; ++x) hash = (hash ^ fb.pixels[y * fb.w + x]) * kFnvPrime;
            const uint32_t used = app.mem().persistent().used();
            if (cycle == 0) {
                expected_hash = hash; entities = app.world().count(); sprites = sprite_count;
                live = r.live_textures(); arena_bytes = used;
            } else if (hash != expected_hash || entities != app.world().count() || sprites != sprite_count ||
                       r.live_textures() != live || used != arena_bytes) { passed = false; break; }
            if (r.stats().tiles_drawn == 0 || r.stats().sprites_submitted < sprite_count) { passed = false; break; }
            if (cycle) ++completed;
        }
        std::printf("[project] reload stress: %s, %u cycles, frame %08x, %u live textures, %u arena bytes\n",
                    passed ? "PASS" : "FAIL", unsigned(completed), unsigned(expected_hash), unsigned(live), unsigned(arena_bytes));
        flow.level().unload(app);
        app.request_quit();
    }
};

int main() {
    const char* p = std::getenv("PHX_PROFILE");
    phx::TargetProfile target = !p                     ? phx::kTargetDesktop
                              : !std::strcmp(p, "gba") ? phx::kTargetGba
                              : !std::strcmp(p, "psp") ? phx::kTargetPsp
                                                       : phx::kTargetDesktop;
    // Off hardware the GBA PPU backend composes each frame into a 240x160 RGBA buffer carved from
    // the arena; on the console that image is VRAM, which costs the arena nothing. Grant exactly
    // that, so everything else (ECS, bundle, tiles, audio, the game) runs at the real GBA budget.
    const uint32_t host_only = target.screen_w == 240 && target.screen_h == 160 ? 240u * 160u * 4u + 64u : 0u;
    target.total_ram += host_only;

    for (uint32_t f = 0; f < 120; ++f)
        g_script[f] = f < 30 ? 0u : f < 60 ? (kRight | ((f == 30 || f == 45) ? kA : 0u)) : f < 90 ? kLeft : 0u;
    uint32_t script_n = 120;
    const char* report = std::getenv("PHX_BUDGET_REPORT");
    if (const char* fr = std::getenv("PHX_BUDGET_FRAMES"); fr && std::atoi(fr) > 120) {
        script_n = uint32_t(std::atoi(fr));
        if (script_n > 4096) script_n = 4096;
        for (uint32_t f = 120; f < script_n; ++f) {
            const uint32_t t = (f - 120) % 300;        // A (a title / a jump), run right jumping, step back
            g_script[f] = t == 0 ? kA : t < 260 ? (kRight | (t % 40 == 0 ? kA : 0u)) : kLeft;
        }
    }
    phx_null_set_button_script(g_script, script_n);

    phx::Game& game = phx::game_instance();
    phx::App app(phx::game_config(game, target));
    if (const char* bundle = std::getenv("PHX_RELOAD_BUNDLE")) {
        ReloadStress stress;
        stress.bundle = bundle;
        const int rc = app.run(&stress);
        return rc ? rc : stress.passed ? 0 : 4;
    }
    const int rc = app.run(&game);
    if (report && *report && !phx::write_budget_report(app, target, report, host_only))
        std::printf("[project][ERROR] cannot write the budget report %s\n", report);
    std::printf("[project] audio: %u sound(s) queued, headless peak %d\n",
                unsigned(app.audio().plays()), int(app.audio().peak()));
    const char* want = std::getenv("PHX_EXPECT_AUDIO");
    if (rc == 0 && want && *want == '1' && (app.audio().plays() == 0 || app.audio().peak() == 0)) {
        std::printf("[project][ERROR] the game made no sound\n");
        return 3;
    }
    return rc;
}
