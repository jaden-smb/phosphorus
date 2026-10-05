// tests/smoke_app.cpp — end-to-end headless smoke: boot the engine, run the fixed-step
// loop for a deterministic number of frames on the null platform, and assert the
// heartbeat is exactly as expected. This is the first time the whole boot path runs.
#include "phx/runtime/app.h"
#include "phx/runtime/main.h"
#include "phx/ecs/reflect.h"
#include "phx/runtime/devtools.h"
#include "phx/runtime/budget.h"
#include "phx/physics/physics.h"
#include "phx/platform/desktop.h"
#include "phx/platform/gfx_soft.h"
#include "phx/core/log.h"

#include <cstdio>
#include <string>

// hooks exported by the null backend to make the loop deterministic
extern "C" void phx_null_set_step_ns(uint64_t ns);
extern "C" void phx_null_set_max_frames(uint64_t n);
extern "C" void phx_null_desktop_push(const phx_desktop_event* e);

using namespace phx;

// A reflected component: what `make game` exports for Phosphorus Studio (write_component_schema).
struct Speedy { int16_t top = 90; scalar accel = s_from_q16(3 * 32768); bool turbo = true; };
PHX_COMPONENT(Speedy, PHX_FIELD(Speedy, top), PHX_FIELD(Speedy, accel), PHX_FIELD(Speedy, turbo));

namespace {
struct Tag {};

struct CountGame : Game {
    int  fixed_updates = 0;
    int  renders       = 0;
    bool started = false, stopped = false;
    bool alpha_in_range = true;

    // Proves world.defer()/flush_deferred() wiring (docs/04-ecs.md): App::run must apply a
    // deferred despawn recorded inside on_fixed_update before the SAME frame's on_render
    // runs, with no manual flush_deferred() call from the game.
    ecs::Entity keep{}, doomed{};
    bool deferred_recorded  = false;
    bool auto_flush_checked = false;
    bool auto_flush_ok      = false;

    void on_start(App& app) override {
        started = true;
        keep   = app.world().spawn();
        doomed = app.world().spawn();
        app.world().add<Tag>(keep);
        app.world().add<Tag>(doomed);
    }
    void on_fixed_update(App& app, scalar) override {
        ++fixed_updates;
        if (!deferred_recorded) {
            // Record via each() (the intended defer() use case) rather than a bare despawn.
            app.world().each<Tag>([&](ecs::Entity e, Tag&) {
                if (e == doomed) app.world().defer().despawn(e);
            });
            deferred_recorded = true;
        }
    }
    void on_render(App& app, scalar alpha) override {
        ++renders;
        double a = s_to_double(alpha);             // tier-agnostic (float or fixed16)
        if (!(a >= 0.0 && a < 1.0)) alpha_in_range = false;
        if (deferred_recorded && !auto_flush_checked) {
            auto_flush_checked = true;
            auto_flush_ok = !app.world().is_alive(doomed) && app.world().is_alive(keep);
        }
    }
    void on_stop(App&) override { stopped = true; }
};

// A game that describes itself before boot (phx/runtime/main.h): the target profile policy.
struct ConfiguredGame : CountGame {
    uint32_t want_entities = 0;
    uint32_t seen_ram = 0;
    void on_configure(Config& cfg) override {
        seen_ram   = cfg.total_ram;              // what the target handed in
        cfg.title  = "configured";
        cfg.width  = 320;
        cfg.height = 200;
        if (want_entities) cfg.max_entities = want_entities;
    }
};

// A game that plays a sound (App::audio()): on the null platform there is no device, so the App
// mixes it headlessly and reports the peak.
struct SoundGame : CountGame {
    int16_t tone[512];
    bool    queued = false;
    void on_start(App& app) override {
        CountGame::on_start(app);
        for (int i = 0; i < 512; ++i) tone[i] = (i & 32) ? int16_t(8000) : int16_t(-8000);
        queued = app.audio().play(SoundView{ tone, 512, 44100 });
    }
};

// Under the developer tools: reads the finished frame at teardown, where the overlay drew.
struct DevGame : CountGame {
    ecs::Entity marked = ecs::kInvalid;
    uint32_t panel_px = 0, clear_px = 0;
    void on_start(App& app) override {
        CountGame::on_start(app);
        marked = app.world().spawn();
        app.world().add<Speedy>(marked, Speedy{});          // a reflected component for the inspector
        app.world().add<Transform>(marked, Transform{ vec2{ s_from_int(20), s_from_int(20) } });
    }
    void on_render(App& app, scalar a) override {
        CountGame::on_render(app, a);
        app.render().begin_frame(Camera2D{});
        app.render().end_frame();
    }
    void on_stop(App& app) override {
        CountGame::on_stop(app);
        const phx_soft_fb fb = phx_gfx_soft_lock(app.platform()->gfx());
        if (fb.pixels && fb.w > 150 && fb.h > 30) {
            panel_px = fb.pixels[size_t(3) * size_t(fb.w) + 196];    // inside the overlay panel (past the short first line)
            clear_px = fb.pixels[size_t(fb.h - 1) * size_t(fb.w) + size_t(fb.w - 1)];   // outside it
        }
    }
};

// The debugger's other tools: F9 halts on the engine's next warning; PgDn + '=' edit the selected
// entity (sent mid-run, once the overlay has picked it); PHX_TRACE writes the per-frame trace.
struct DebugGame : CountGame {
    ecs::Entity marked = ecs::kInvalid;
    scalar end_x{}, end_y{};
    void on_start(App& app) override {
        CountGame::on_start(app);
        marked = app.world().spawn();
        app.world().add<Transform>(marked, Transform{ vec2{ s_from_int(20), s_from_int(20) } });
    }
    void on_fixed_update(App& app, scalar dt) override {
        CountGame::on_fixed_update(app, dt);
        auto key = [](int k) { phx_desktop_event e{}; e.kind = PHX_DEV_KEY_DOWN; e.key = k; phx_null_desktop_push(&e); };
        if (fixed_updates == 2) { key(PHX_KEY_PAGE_DOWN); key('='); key('='); }   // cursor -> y, y += 2
        if (fixed_updates == 5) PHX_LOG_WARN("smoke: a warning the developer tools halt on");
    }
    void on_stop(App& app) override {
        CountGame::on_stop(app);
        if (const Transform* t = app.world().get<Transform>(marked)) { end_x = t->pos.x; end_y = t->pos.y; }
    }
};

bool game_config_policy_ok() {
    bool ok = true;
    ConfiguredGame g;
    const Config pc = game_config(g, kTargetDesktop);             // tier budgets, the game's size
    ok = ok && pc.total_ram == Config::from_defaults().total_ram && g.seen_ram == pc.total_ram &&
         pc.width == 320 && pc.height == 200 && std::string(pc.title) == "configured";
    const Config gba = game_config(g, kTargetGba);                // proven budgets, the LCD's size
    ok = ok && gba.total_ram == (160u << 10) && g.seen_ram == (160u << 10) && gba.frame_scratch == (4u << 10) &&
         gba.max_entities == 256 && gba.width == 240 && gba.height == 160 && gba.validate() == Status::Ok;
    g.want_entities = 100;                                        // a game may still change a budget
    ok = ok && game_config(g, kTargetGba).max_entities == 100;
    const Config psp = game_config(g, kTargetPsp);                // any size up to the screen
    ok = ok && psp.total_ram == (4u << 20) && psp.width == 320 && psp.height == 200 && psp.validate() == Status::Ok;
    return ok;
}
} // namespace

int main() {
    // one clock tick == one sim step  => exactly one fixed update per frame, deterministically
    phx_null_set_step_ns(1000000000ull / 60);
    phx_null_set_max_frames(100);

    Config cfg   = Config::from_defaults();
    cfg.title    = "smoke";
    cfg.sim_hz   = 60;
    cfg.total_ram = 8u << 20;          // 8 MB is plenty for a heartbeat (override the PC default)
    cfg.frame_scratch = 256u << 10;    // 256 KB x2

    App app(cfg);
    CountGame game;
    int rc = app.run(&game);

    std::printf("\nsmoke results: rc=%d started=%d stopped=%d fixed=%d render=%d frames=%llu "
                "alpha_ok=%d auto_flush_ok=%d\n",
                rc, game.started, game.stopped, game.fixed_updates, game.renders,
                (unsigned long long)app.frame(), game.alpha_in_range, game.auto_flush_ok);

    bool ok = rc == 0
           && game.started && game.stopped
           && game.fixed_updates == 100
           && game.renders == 100
           && app.frame() == 100
           && game.alpha_in_range
           && game.auto_flush_checked
           && game.auto_flush_ok;

    // Regression soak: the loop's profiler clock reads must not leak time into the fixed-step
    // accumulator. Uncompensated, the 1 µs-per-read ticks added +5 µs/frame, which crossed a
    // whole extra step every ~3,334 frames — one phantom double-sim-step frame per minute (the
    // occasional GBA stutter; same virtual-clock convention). Run past that boundary and
    // require EXACTLY one fixed update per frame.
    phx_null_set_max_frames(4000);
    App soak_app(cfg);
    CountGame soak;
    int soak_rc = soak_app.run(&soak);
    std::printf("soak results: rc=%d fixed=%d frames=%llu\n",
                soak_rc, soak.fixed_updates, (unsigned long long)soak_app.frame());
    ok = ok && soak_rc == 0 && soak.fixed_updates == 4000 && soak_app.frame() == 4000;

    // The engine-owned entry: each target's profile turns into the boot Config, then runs.
    const bool policy_ok = game_config_policy_ok();
    phx_null_set_max_frames(10);
    ConfiguredGame entry_game;
    // The console profile of this build's tier boots (GBA budgets only fit the GBA caps tier:
    // on the PC tier the renderer alone sizes for 16384 sprites).
    const int entry_rc = run_game(entry_game, caps().render_tier == 0 ? kTargetGba : kTargetPsp);
    std::printf("entry results: policy_ok=%d rc=%d fixed=%d\n", policy_ok, entry_rc, entry_game.fixed_updates);
    ok = ok && policy_ok && entry_rc == 0 && entry_game.fixed_updates == 10;

    // Engine-owned audio: lazy (a silent game never starts it), headless where there's no device.
    phx_null_set_max_frames(10);
    App quiet_app(cfg);
    CountGame quiet;
    const int quiet_rc = quiet_app.run(&quiet);
    App sound_app(cfg);
    SoundGame sound;
    const int sound_rc = sound_app.run(&sound);
    std::printf("audio results: quiet plays=%u peak=%d | sound queued=%d plays=%u peak=%d device=%d rate=%u\n",
                unsigned(quiet_app.audio().plays()), int(quiet_app.audio().peak()), sound.queued,
                unsigned(sound_app.audio().plays()), int(sound_app.audio().peak()),
                sound_app.audio().has_device(), unsigned(sound_app.audio().rate()));
    ok = ok && quiet_rc == 0 && quiet_app.audio().plays() == 0 && quiet_app.audio().peak() == 0 &&
         sound_rc == 0 && sound.queued && sound_app.audio().plays() == 1 && sound_app.audio().peak() > 0 &&
         !sound_app.audio().has_device() && sound_app.audio().rate() == GameAudio::kHeadlessRate;

    // Developer tools: F5 pauses, F6 steps once each, the overlay draws over the frame.
    {
        auto key = [](int k) { phx_desktop_event e{}; e.kind = PHX_DEV_KEY_DOWN; e.key = k; phx_null_desktop_push(&e); };
        key(PHX_KEY_F5); key(PHX_KEY_F6); key(PHX_KEY_F6);
        phx_null_set_max_frames(10);
        Config dc = cfg;
        dc.width = 240; dc.height = 64;
        App dev_app(dc);
        install_devtools(dev_app);
        DevGame dev;
        const int dev_rc = dev_app.run(&dev);
        const bool darker = (dev.panel_px & 0xFF) < (dev.clear_px & 0xFF);
        std::printf("devtools results: rc=%d fixed=%d renders=%d overlay_px=%08x clear_px=%08x\n", dev_rc,
                    dev.fixed_updates, dev.renders, unsigned(dev.panel_px), unsigned(dev.clear_px));
        ok = ok && dev_rc == 0 && dev.fixed_updates == 2 && dev.renders == 10 && darker;

        // F9 (halt on a warning) + a live edit + the trace
#if defined(_WIN32)
        _putenv("PHX_TRACE=build/smoke_trace.csv");
#else
        setenv("PHX_TRACE", "build/smoke_trace.csv", 1);
#endif
        key(PHX_KEY_F9);
        phx_null_set_max_frames(20);
        App dbg_app(dc);
        install_devtools(dbg_app);
        DebugGame dbg;
        const int dbg_rc = dbg_app.run(&dbg);
#if defined(_WIN32)
        _putenv("PHX_TRACE=");
#else
        unsetenv("PHX_TRACE");
#endif
        int trace_lines = 0;
        if (FILE* f = std::fopen("build/smoke_trace.csv", "rb")) {
            char b[256];
            while (std::fgets(b, sizeof(b), f)) ++trace_lines;
            std::fclose(f);
        }
        // F3: half speed (one fixed step every other frame)
        key(PHX_KEY_F3);
        phx_null_set_max_frames(20);
        App slow_app(dc);
        install_devtools(slow_app);
        CountGame slow;
        const int slow_rc = slow_app.run(&slow);
        std::printf("debugger results: rc=%d fixed=%d x=%.1f y=%.1f trace_lines=%d | slow rc=%d fixed=%d\n", dbg_rc,
                    dbg.fixed_updates, s_to_double(dbg.end_x), s_to_double(dbg.end_y), trace_lines, slow_rc, slow.fixed_updates);
        ok = ok && dbg_rc == 0 && dbg.fixed_updates == 5 && dbg.end_x == s_from_int(20) && dbg.end_y == s_from_int(22) &&
             trace_lines >= 20 && slow_rc == 0 && slow.fixed_updates == 10;
    }

    // The component schema tools read: names, field types, and the C++ defaults.
    std::string schema;
    if (write_component_schema("build/smoke_components.json"))
        if (FILE* f = std::fopen("build/smoke_components.json", "rb")) {
            char b[4096]; const size_t n = std::fread(b, 1, sizeof(b), f); schema.assign(b, n); std::fclose(f);
        }
    const bool schema_ok = schema.find("\"name\": \"Speedy\"") != std::string::npos &&
                           schema.find("{ \"name\": \"top\", \"type\": \"i16\", \"default\": 90 }") != std::string::npos &&
                           schema.find("{ \"name\": \"accel\", \"type\": \"scalar\", \"default\": 1.5 }") != std::string::npos &&
                           schema.find("{ \"name\": \"turbo\", \"type\": \"bool\", \"default\": 1 }") != std::string::npos;
    std::printf("schema results: ok=%d (%u bytes)\n", schema_ok, unsigned(schema.size()));
    ok = ok && schema_ok;

    // The budget report (phx/runtime/budget.h): the first run's high-water marks, written after
    // run() tore the App down (the peaks outlive it), with the target's ceilings and the host-only
    // bytes left out of the arena figures.
    const RuntimePeaks& pk = app.peaks();
    std::string rep;
    if (write_budget_report(app, kTargetGba, "build/smoke_budget.json", 1024u))
        if (FILE* f = std::fopen("build/smoke_budget.json", "rb")) {
            char b[4096]; const size_t n = std::fread(b, 1, sizeof(b), f); rep.assign(b, n); std::fclose(f);
        }
    const bool budget_ok = pk.entities >= 1 && pk.arena_used > 0 && pk.arena_capacity >= pk.arena_used &&
                           rep.find("\"target\": \"gba\"") != std::string::npos &&
                           rep.find("\"sprites\": { \"peak\": ") != std::string::npos &&
                           rep.find("\"max\": 128") != std::string::npos &&
                           rep.find("\"capacity\": " + std::to_string(pk.arena_capacity - 1024u) + " }") != std::string::npos &&
                           rep.find("\"frames\": 100") != std::string::npos;
    std::printf("budget results: ok=%d entities=%u arena=%llu/%llu\n", budget_ok, pk.entities,
                (unsigned long long)pk.arena_used, (unsigned long long)pk.arena_capacity);
    ok = ok && budget_ok;

    std::printf(ok ? "SMOKE PASS\n\n" : "SMOKE FAIL\n\n");
    return ok ? 0 : 1;
}
