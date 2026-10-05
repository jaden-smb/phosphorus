// tools/phxstudio/budget.h — the model behind Phosphorus Studio's BUDGET view: for each target (PC, GBA,
// PSP), what the game USED when it ran there (the budget report `make project-budget` writes:
// phx/runtime/budget.h) next to what the target ALLOWS, plus what the baked bundle for that tier
// holds (its size, its biggest assets, textures the GBA can't store as tiles). Every limit becomes
// a line with a level (ok / close / over), so the view and the tests read the same verdicts.
// Host-only, headless (the editors suite covers it).
#ifndef PHX_TOOLS_PHXSTUDIO_BUDGET_H
#define PHX_TOOLS_PHXSTUDIO_BUDGET_H

#include "model.h"
#include "json.h"          // tools/phxpack

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace phxstudio {

struct BudgetReport {
    bool ok = false;
    std::string target, title;
    uint64_t frames = 0;
    uint64_t arena_used = 0, arena_cap = 0;
    uint64_t scratch_peak = 0, scratch_cap = 0;
    uint64_t ents_peak = 0, ents_max = 0;
    uint64_t sprites_peak = 0, sprites_max = 0, sprites_dropped = 0;
    uint64_t sounds = 0, channels = 0;
    uint64_t warnings = 0, errors = 0;

    static bool parse(const std::string& text, BudgetReport& out) {
        out = BudgetReport{};
        phxtool::JsonValue r;
        if (!phxtool::JsonParser::parse(text, r) || !r.is_obj() || !r.find("budget")) return false;
        auto num = [&](const char* obj, const char* key) -> uint64_t {
            const phxtool::JsonValue* o = r.find(obj);
            const phxtool::JsonValue* v = o ? o->find(key) : nullptr;
            return v && v->as_num(0) > 0 ? uint64_t(v->as_num(0)) : 0;
        };
        out.target = r.str_at("target");
        out.title = r.str_at("title");
        if (const phxtool::JsonValue* f = r.find("frames")) out.frames = uint64_t(std::max(0.0, f->as_num(0)));
        out.arena_used = num("arena", "used");          out.arena_cap = num("arena", "capacity");
        out.scratch_peak = num("frame_scratch", "peak"); out.scratch_cap = num("frame_scratch", "capacity");
        out.ents_peak = num("entities", "peak");        out.ents_max = num("entities", "max");
        out.sprites_peak = num("sprites", "peak");      out.sprites_max = num("sprites", "max");
        out.sprites_dropped = num("sprites", "dropped");
        out.sounds = num("audio", "sounds");            out.channels = num("audio", "channels");
        out.warnings = num("log", "warnings");          out.errors = num("log", "errors");
        out.ok = true;
        return true;
    }
};

// What a tier's bundle holds.
struct BundleFacts {
    bool ok = false;
    uint64_t file_size = 0;
    struct Big { std::string name; phx::AssetType type; uint64_t bytes; };
    std::vector<Big> biggest;                    // the 5 largest assets (decompressed)
    std::vector<std::string> not_tiles;          // tier 0: textures kept RGBA8 (not 4bpp tiles)
    uint64_t sound_bytes = 0, texture_bytes = 0;
};

inline BundleFacts bundle_facts(const BundleDoc& d, const NameBook* names = nullptr) {
    BundleFacts f;
    if (!d.ok) return f;
    f.ok = true;
    f.file_size = d.file_size;
    auto name = [&](const AssetEntry& a) {
        const std::string* n = names ? names->find(a.hash) : nullptr;
        char b[16];
        std::snprintf(b, sizeof(b), "%08x", unsigned(a.hash));
        return n ? *n : std::string(b);
    };
    for (const AssetEntry& a : d.assets) {
        f.biggest.push_back(BundleFacts::Big{ name(a), a.type, a.usize });
        if (a.type == phx::AssetType::Sound) f.sound_bytes += a.usize;
        if (a.type == phx::AssetType::Texture) {
            f.texture_bytes += a.usize;
            TexView v;
            if (d.hdr.target == 0 && view_texture(a, v) && v.fmt != phx::PixelFormat::PAL4_TILES) f.not_tiles.push_back(name(a));
        }
    }
    std::stable_sort(f.biggest.begin(), f.biggest.end(), [](const BundleFacts::Big& x, const BundleFacts::Big& y) { return x.bytes > y.bytes; });
    if (f.biggest.size() > 5) f.biggest.resize(5);
    return f;
}

enum class Level : uint8_t { Ok, Close, Over, Info };
struct BudgetLine { std::string what; uint64_t used = 0, limit = 0; bool bytes = false; Level level = Level::Info; std::string note; };

inline Level level_of(uint64_t used, uint64_t limit) {
    if (!limit) return Level::Info;
    if (used > limit) return Level::Over;
    return used * 100 >= limit * 90 ? Level::Close : Level::Ok;
}

// The GBA cartridge the size gate allows (Makefile GAME_GBA_ROM_MB), and roughly what the engine's
// code adds to the bundle in a ROM (the template's ROM minus its bundle; measured by size_gate).
constexpr uint64_t kGbaRomBytes  = 32ull << 20;
constexpr uint64_t kGbaCodeBytes = 200ull << 10;

// The lines one target's card shows. `rep` may be !ok (not measured yet): then only the bundle.
inline std::vector<BudgetLine> budget_lines(const std::string& target, const BudgetReport& rep, const BundleFacts& b) {
    std::vector<BudgetLine> out;
    if (rep.ok) {
        out.push_back(BudgetLine{ "memory (arena)", rep.arena_used, rep.arena_cap, true, level_of(rep.arena_used, rep.arena_cap),
                                  target == "gba" ? "measured on the host: 64-bit pointers make it a little high" : "" });
        out.push_back(BudgetLine{ "entities (peak)", rep.ents_peak, rep.ents_max, false, level_of(rep.ents_peak, rep.ents_max), "" });
        BudgetLine sp{ "sprites per frame", rep.sprites_peak, rep.sprites_max, false, level_of(rep.sprites_peak, rep.sprites_max), "" };
        if (rep.sprites_dropped) { sp.level = Level::Over; sp.note = std::to_string(rep.sprites_dropped) + " sprites dropped (past the limit)"; }
        out.push_back(sp);
        if (rep.scratch_cap)
            out.push_back(BudgetLine{ "frame scratch", rep.scratch_peak, rep.scratch_cap, true, level_of(rep.scratch_peak, rep.scratch_cap), "" });
        BudgetLine lg{ "engine warnings / errors", rep.warnings + rep.errors, 0, false, rep.errors ? Level::Over : rep.warnings ? Level::Close : Level::Ok, "" };
        lg.note = std::to_string(rep.warnings) + " warnings, " + std::to_string(rep.errors) + " errors (see the run's log)";
        out.push_back(lg);
    }
    if (b.ok) {
        if (target == "gba") {
            const uint64_t rom = b.file_size + kGbaCodeBytes;
            out.push_back(BudgetLine{ "ROM (bundle + ~200 KB code)", rom, kGbaRomBytes, true, level_of(rom, kGbaRomBytes), "" });
            if (!b.not_tiles.empty()) {
                std::string n;
                for (size_t i = 0; i < b.not_tiles.size() && i < 4; ++i) n += (i ? ", " : "") + b.not_tiles[i];
                out.push_back(BudgetLine{ "textures not in GBA tile format", b.not_tiles.size(), 0, false, Level::Close,
                                          n + (b.not_tiles.size() > 4 ? ", ..." : "") + ": > 15 colours in an 8x8 tile, or not 8-px aligned" });
            }
        } else {
            out.push_back(BudgetLine{ "bundle", b.file_size, 0, true, Level::Info, "" });
        }
    }
    return out;
}

// ---- the PROFILER: a frame-timing trace (PHX_TRACE=file while playing: engine/runtime/src/devtools.cpp) ----
struct FrameTrace {
    struct Row { uint32_t frame = 0, update = 0, render = 0, present = 0, total = 0, budget = 16667; int steps = 0;
                 uint32_t ents = 0, sprites = 0; };
    std::vector<Row> rows;

    // CSV: frame,update_us,render_us,present_us,frame_us,budget_us,steps,entities,sprites (a header line first).
    static bool parse(const std::string& csv, FrameTrace& out) {
        out = FrameTrace{};
        size_t p = 0;
        bool header = false;
        while (p < csv.size()) {
            size_t e = csv.find('\n', p);
            if (e == std::string::npos) e = csv.size();
            const std::string line = csv.substr(p, e - p);
            p = e + 1;
            if (line.empty()) continue;
            if (!header) { header = line.compare(0, 6, "frame,") == 0; if (!header) return false; continue; }
            Row r;
            unsigned long v[9] = {};
            long st = 0;
            if (std::sscanf(line.c_str(), "%lu,%lu,%lu,%lu,%lu,%lu,%ld,%lu,%lu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &st,
                            &v[7], &v[8]) != 9)
                continue;                                   // a line cut short by a crash: skip it
            r.frame = uint32_t(v[0]); r.update = uint32_t(v[1]); r.render = uint32_t(v[2]); r.present = uint32_t(v[3]);
            r.total = uint32_t(v[4]); r.budget = v[5] ? uint32_t(v[5]) : 16667u; r.steps = int(st);
            r.ents = uint32_t(v[7]); r.sprites = uint32_t(v[8]);
            if (r.frame == 0) continue;                     // the first frame has no previous one to time
            out.rows.push_back(r);
        }
        return header;
    }
    struct Stat { uint32_t avg = 0, p50 = 0, p95 = 0, max = 0; };
    Stat stat(uint32_t Row::*f) const {
        Stat s;
        if (rows.empty()) return s;
        std::vector<uint32_t> v;
        v.reserve(rows.size());
        uint64_t sum = 0;
        for (const Row& r : rows) { v.push_back(r.*f); sum += r.*f; }
        std::sort(v.begin(), v.end());
        s.avg = uint32_t(sum / v.size());
        s.p50 = v[v.size() / 2];
        s.p95 = v[std::min(v.size() - 1, v.size() * 95 / 100)];
        s.max = v.back();
        return s;
    }
    // Frames whose WORK (update + render) overran the step budget. `present` is left out: it holds
    // the vsync wait, so a vsynced frame always takes about one budget whatever the game does.
    static uint32_t work(const Row& r) { return r.update + r.render; }
    size_t over_budget() const {
        size_t n = 0;
        for (const Row& r : rows) n += work(r) > r.budget;
        return n;
    }
    // The indices of the `n` frames with the most work, most first.
    std::vector<size_t> worst(size_t n) const {
        std::vector<size_t> idx(rows.size());
        for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
        std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return work(rows[a]) > work(rows[b]); });
        if (idx.size() > n) idx.resize(n);
        return idx;
    }
};

} // namespace phxstudio
#endif // PHX_TOOLS_PHXSTUDIO_BUDGET_H
