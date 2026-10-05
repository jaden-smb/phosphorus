// phx/core/config.h — immutable boot configuration. Zeros are filled from the active
// capability tier, so a game gets sane per-platform budgets without #ifdefs. Config is
// frozen after boot, keeping the engine footprint statically analyzable (vital on GBA).
#ifndef PHX_CORE_CONFIG_H
#define PHX_CORE_CONFIG_H

#include "phx/core/types.h"
#include "phx/core/caps.h"
#include "phx/core/log.h"

namespace phx {

struct Config {
    const char* title = "Phosphorus Engine";

    // budgets — 0 means "derive from the capability tier"
    uint32_t total_ram     = 0;
    uint32_t cache_bytes   = 0;
    uint32_t frame_scratch = 0;
    uint32_t max_entities  = 0;

    uint16_t sim_hz  = 60;
    uint16_t max_fps = 0;        // 0 => vsync-locked
    int32_t  width   = 480;
    int32_t  height  = 272;      // PSP native; PC windows can override
    bool     vsync   = true;

    LogLevel log_level = LogLevel::Info;

    static Config from_defaults() {
        Config c;
        const Caps k = caps();
        c.total_ram     = k.total_main_ram;
        c.max_entities  = k.max_entities;
        c.frame_scratch = k.total_main_ram / 32;          // ~3% per frame buffer (x2)
        c.cache_bytes   = k.total_main_ram / 2;
        return c;
    }

    Status validate() const {
        if (sim_hz == 0) return Status::BadArg;
        if (frame_scratch * 2 >= total_ram) return Status::OutOfMemory;
        return Status::Ok;
    }
};

} // namespace phx
#endif // PHX_CORE_CONFIG_H
