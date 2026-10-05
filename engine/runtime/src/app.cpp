// phx/runtime/src/app.cpp — the fixed-timestep main loop (docs/00 §7). Decoupled from any
// platform: it talks only to the phx_platform C seam and the engine's own allocators.
#include "phx/runtime/app.h"
#include "phx/core/log.h"
#include "phx/platform/platform.h"

#include <cstdlib>   // getenv — optional bounded-run cap for headless/CI verification

namespace phx {
namespace {
// Optional frame cap: if PHX_MAX_FRAMES is set (>0), the loop quits after that many frames.
// Lets a windowed backend be smoke-run unattended (a bounded run that still shuts down cleanly,
// printing the frame count) without a synthetic quit event. 0/unset = run until the user quits.
uint64_t env_frame_cap() {
    const char* s = std::getenv("PHX_MAX_FRAMES");
    if (!s || !*s) return 0;
    unsigned long long v = std::strtoull(s, nullptr, 10);
    return uint64_t(v);
}
// Adapter so the engine's LogSink (LogLevel) can drive the platform's C log (phx_log_level)
// without an unsafe function-pointer cast.
const phx_platform* g_log_plat = nullptr;
void plat_log_adapter(LogLevel level, const char* msg) {
    if (g_log_plat && g_log_plat->log)
        g_log_plat->log(static_cast<phx_log_level>(level), msg);
}
} // namespace

int App::run(Game* game) {
    // 0. validate config (programmer error if bad — fail loud at boot, never mid-frame)
    if (Status st = cfg_.validate(); st != Status::Ok) {
        PHX_LOG_ERROR("invalid Config (status=%d)", int(st));
        return 1;
    }

    // 1. memory: the single boot allocation, carved into arenas + frame stacks
    auto mr = MemoryRoot::boot(cfg_.total_ram, cfg_.frame_scratch);
    if (!mr.ok()) { PHX_LOG_ERROR("MemoryRoot::boot failed (ram=%u)", cfg_.total_ram); return 1; }
    mem_ = mr.unwrap();

    // 2. platform: exactly one backend is linked; resolve and init it
    plat_ = phx_platform_get();
    phx_platform_desc desc{};
    desc.title           = cfg_.title;
    desc.width           = cfg_.width;
    desc.height          = cfg_.height;
    desc.vsync           = cfg_.vsync ? 1 : 0;
    desc.root_arena      = nullptr;
    desc.root_arena_size = 0;
    if (plat_->init(&desc) != 0) { PHX_LOG_ERROR("platform init failed (%s)", cfg_.title); return 1; }
    g_log_plat = plat_;
    log_set_sink(plat_->log ? plat_log_adapter : nullptr);
    log_set_level(cfg_.log_level);

    // 3. ECS world (sized from config/caps) — carved from the persistent arena
    uint32_t max_ents = cfg_.max_entities ? cfg_.max_entities : caps().max_entities;
    auto wr = ecs::World::create(mem_->persistent(), max_ents);
    if (!wr.ok()) { PHX_LOG_ERROR("World::create failed (max=%u)", max_ents); return 1; }
    world_ = wr.unwrap();

    // 4. renderer (software/GL/GU/PPU per tier) — needs the platform gfx device
    if (phx_gfx* gfx = plat_->gfx()) {
        auto rr = Renderer::create(gfx, mem_->persistent(), caps());
        if (!rr.ok()) { PHX_LOG_ERROR("Renderer::create failed (tier=%d)", int(caps().render_tier)); return 1; }
        render_ = rr.unwrap();
    } else {
        PHX_LOG_WARN("no gfx device for '%s'; running without a renderer", cfg_.title);
    }

    // 5. timing
    acc_.configure(cfg_.sim_hz);
    dt_ = fixed_dt(cfg_.sim_hz);
    prof_.budget_us = cfg_.sim_hz ? 1000000u / cfg_.sim_hz : 16667u;

    // 5b. sound: nothing starts until the game first plays something (phx/runtime/audio.h)
    audio_.attach(plat_->audio ? plat_->audio() : nullptr, &mem_->persistent(), caps(), cfg_.sim_hz);

    PHX_LOG_INFO("Phosphorus boot: '%s'  ram=%uKB  sim=%uHz  ents=%u", cfg_.title,
                 cfg_.total_ram / 1024u, cfg_.sim_hz, max_ents);

    // 6. game start hook
    game->on_start(*this);

    // 7. the loop: fixed-step sim, interpolated render, O(1) transient reclaim
    const uint64_t frame_cap = env_frame_cap();
    phx_input_raw raw{};
    uint64_t prev = plat_->clock_ns();
    while (!quit_) {
        if (frame_cap && frame_ >= frame_cap) { quit_ = true; break; }
        if (plat_->pump_events() == 0) { quit_ = true; break; }

        uint64_t now = plat_->clock_ns();
        int steps = acc_.advance(now - prev);
        prev = now;

        plat_->poll_input(&raw);
        input_.update(raw);        // raw -> semantic edges, once per frame
        if (dev_.frame) steps = dev_.frame(dev_.user, *this, steps);   // developer pause / step

        // Phase profiling: stamp each phase with the platform clock (µs into prof_). Four
        // clock reads per frame — cheap enough to keep on unconditionally, even on GBA.
        const uint64_t t_upd = plat_->clock_ns();
        for (int i = 0; i < steps; ++i) {
            game->on_fixed_update(*this, dt_);
            // Apply any despawns the game recorded via world().defer() during this step
            // (e.g. from inside an each() callback). Automatic and unconditional: with no
            // pending commands this is an empty loop, so games that never call defer() pay
            // nothing extra. This is what makes `world.defer().despawn(e)` actually "safe to
            // record during iteration, applied on flush" (world.h) without the game having
            // to remember a manual flush call itself.
            world_->flush_deferred();
        }

        const uint64_t t_ren = plat_->clock_ns();
        game->on_render(*this, acc_.alpha());
        if (dev_.overlay) dev_.overlay(dev_.user, *this);                // developer overlay
        if (audio_.pump_) audio_.pump_(audio_);   // no device: mix this frame's sound here

        // high-water marks (a budget report's numbers)
        if (world_->count() > peaks_.entities) peaks_.entities = world_->count();
        if (render_) {
            const RenderStats& rs = render_->stats();
            if (rs.sprites_submitted > peaks_.sprites) peaks_.sprites = rs.sprites_submitted;
            if (rs.tiles_drawn > peaks_.tiles) peaks_.tiles = rs.tiles_drawn;
            if (rs.batches > peaks_.batches) peaks_.batches = rs.batches;
            peaks_.sprites_dropped += rs.sprites_dropped;
        }
        if (mem_->frame_stack().used() > peaks_.frame_scratch) peaks_.frame_scratch = uint32_t(mem_->frame_stack().used());
        peaks_.arena_used = mem_->used();
        peaks_.arena_capacity = mem_->total_capacity();

        mem_->swap_frame();        // double-buffered transient reclaim, O(1)
        ++frame_;

        const uint64_t t_pre = plat_->clock_ns();
        plat_->present();          // swap / vblank (no-op headless)
        const uint64_t t_end = plat_->clock_ns();

        // ns -> µs via multiply-shift (4294967/2^32 ≈ 1e-3, -0.07ppm), rounded UP so any nonzero
        // phase stamps at least 1 µs (the null clock ticks 1 µs/read; a truncating convert
        // would report 0 for every phase there). Four u64 soft divisions per frame add up on
        // a 16 MHz ARM7 with no divider, and these only feed the profiler display.
        const auto ns_to_us = [](uint64_t ns) {
            return uint32_t((ns * 4294967ull + 0xFFFFFFFFull) >> 32);   // (4295 was 1e-6: ms, not µs)
        };
        prof_.update_us  = ns_to_us(t_ren - t_upd);
        prof_.render_us  = ns_to_us(t_pre - t_ren);
        prof_.present_us = ns_to_us(t_end - t_pre);
        prof_.frame_us   = ns_to_us(t_end - now);
    }

    // 6. teardown in reverse
    if (dev_.stop) dev_.stop(dev_.user, *this);
    game->on_stop(*this);
    if (audio_.stop_) audio_.stop_(audio_);   // silence the device before its state goes away
    audio_.detach();
    plat_->shutdown();
    MemoryRoot::shutdown(mem_);
    PHX_LOG_INFO("Phosphorus shutdown after %llu frames", (unsigned long long)frame_);
    return 0;
}

} // namespace phx
