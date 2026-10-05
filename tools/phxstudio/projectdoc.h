// tools/phxstudio/projectdoc.h — a Phosphorus GAME PROJECT and the rules for what Phosphorus Studio may
// touch while one is open. Headless and unit-tested in the editors suite.
//
// A project is a folder holding a `phxproject.json`:
//
//   { "name": "Emberwing",
//     "source":  ["src"],                         // code folders (hints for the Explorer/templates)
//     "assets":  ["assets"],                      // author-asset folders (what `make game-assets` bakes)
//     "bundles": ["build/emberwing.phxp"],        // baked bundles the Assets view shows (may be ../..)
//     "launches": [ { "label": "Play", "group": "play", "command": "make -C \"$PHX_ROOT\" play PROJECT=\"$PHX_PROJECT\"",
//                     "blurb": "...", "needs": ["sdl2-config"], "windowed": true } ] }
//
// Launch commands run from the PROJECT folder with $PHX_ROOT (the Phosphorus checkout) and
// $PHX_PROJECT (the project folder) exported. Paths in the file are relative to the project.
//
// The AccessPolicy is the "professional project" boundary: with a project open, the Studio may
// WRITE only inside the project folder, may READ (never edit) the engine's public API headers
// (engine/<module>/include/...) and its documentation, and may not open anything else — least of
// all the engine's own sources, tools or tests. Paths are canonicalised first, so `..` and
// symlinks cannot step outside. `--engine-dev` (engine maintenance) lifts the boundary.
// Host-only.
#ifndef PHX_TOOLS_PHXSTUDIO_PROJECTDOC_H
#define PHX_TOOLS_PHXSTUDIO_PROJECTDOC_H

#include "json.h"                  // tools/phxpack — the one JSON parser
#include "synth.h"                 // tools/phxpack — the template's .sfx sounds and .song music
#include "font.h"                  // tools/phxpack — the template's .font
#include "dialogue.h"              // tools/phxpack — the template's .dlg
#include "project.h"
#include "pixeldoc.h"
#include "../phxtmap/editor.h"     // TmapDoc (the template level)
#include "ascii_font.h"            // tools/common: the template's font sheet

#include <cstdio>
#include <string>
#include <system_error>
#include <vector>

namespace phxstudio {

// ---- paths ----
// Absolute, symlink-resolved (for the parts that exist), normalised, without a trailing '/'.
inline std::string canon_path(const std::string& p) {
    std::error_code ec;
    pfs::path a = pfs::absolute(p, ec);
    if (ec) return p;
    const pfs::path c = pfs::weakly_canonical(a, ec);
    std::string s = (ec ? a : c).lexically_normal().generic_string();
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}
// Is `p` the folder `dir` or inside it? (both canonical)
inline bool path_within(const std::string& p, const std::string& dir) {
    if (dir.empty()) return false;
    if (p == dir) return true;
    return p.size() > dir.size() && p.compare(0, dir.size(), dir) == 0 && p[dir.size()] == '/';
}

// ---- the access boundary ----
enum class Access : uint8_t { None, Read, Write };

struct AccessPolicy {
    bool engine_dev = false;     // engine maintenance: everything is editable
    std::string engine_root;     // canonical Phosphorus checkout
    std::string project_dir;     // canonical project folder ("" = no project open)

    // The engine's public API: engine/<module>/include/**.h(pp)
    bool is_api_header(const std::string& c) const {
        const std::string eng = engine_root + "/engine";
        if (!path_within(c, eng)) return false;
        const std::string rel = c.substr(eng.size() + 1);             // <module>/include/...
        const size_t sl = rel.find('/');
        if (sl == std::string::npos || rel.compare(sl + 1, 8, "include/") != 0) return false;
        const std::string e = lower_ext(c);
        return e == ".h" || e == ".hpp";
    }
    // Documentation (never code): docs/*.md, README.md, and each tool's instructions.md.
    bool is_engine_doc(const std::string& c) const {
        if (c == engine_root + "/README.md") return true;
        if (path_within(c, engine_root + "/docs") && lower_ext(c) == ".md") return true;
        return path_within(c, engine_root + "/tools") && base_name(c) == "instructions.md";
    }
    Access access(const std::string& path) const {
        if (engine_dev) return Access::Write;
        const std::string c = canon_path(path);
        if (!project_dir.empty() && path_within(c, project_dir)) return Access::Write;
        if (is_api_header(c) || is_engine_doc(c)) return Access::Read;
        return Access::None;
    }
    // Why a folder can't be a project (empty = it can): it must not contain the engine and must
    // not sit inside the engine's own source folders.
    std::string project_dir_problem(const std::string& dir) const {
        const std::string c = canon_path(dir);
        if (path_within(engine_root, c)) return "a project can't contain the Phosphorus engine itself";
        for (const char* sub : { "engine", "tools", "tests", "cmake", "docs", ".github" })
            if (path_within(c, engine_root + "/" + sub))
                return std::string("a project can't live inside the engine's ") + sub + "/ folder";
        return "";
    }
};

// ---- the project file ----
struct ProjectLaunch {
    std::string label, group = "play", command, blurb;
    std::vector<std::string> needs;
    bool windowed = false;
};

class ProjectDoc {
public:
    static constexpr const char* kFile = "phxproject.json";

    std::string dir;             // canonical project folder
    std::string name, description;
    std::vector<std::string> source{ "src" }, assets{ "assets" }, bundles;
    std::vector<ProjectLaunch> launches;

    std::string file() const { return dir + "/" + kFile; }
    static bool is_project_dir(const std::string& d) {
        std::error_code ec;
        return pfs::is_regular_file(pfs::path(d) / kFile, ec);
    }

    // Load from a project folder or its phxproject.json.
    static bool load(const std::string& dir_or_file, ProjectDoc& out, std::string* err = nullptr) {
        auto fail = [&](const std::string& why) { if (err) *err = why; return false; };
        std::string d = dir_or_file;
        if (base_name(d) == kFile) d = dir_name(d);
        if (d.empty()) d = ".";
        std::string text;
        {
            FILE* f = std::fopen((d + "/" + kFile).c_str(), "rb");
            if (!f) return fail("no " + std::string(kFile) + " in " + d);
            char buf[8192];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
            std::fclose(f);
        }
        return parse(text, canon_path(d), out, err);
    }
    static bool parse(const std::string& text, const std::string& dir, ProjectDoc& out, std::string* err = nullptr) {
        auto fail = [&](const std::string& why) { if (err) *err = std::string(kFile) + ": " + why; return false; };
        phxtool::JsonValue root;
        std::string jerr;
        if (!phxtool::JsonParser::parse(text, root, &jerr)) return fail("malformed JSON: " + jerr);
        if (!root.is_obj()) return fail("top level is not a JSON object");
        out = ProjectDoc{};
        out.dir = dir;
        out.name = root.str_at("name");
        if (out.name.empty()) out.name = base_name(dir);
        out.description = root.str_at("description");
        auto strings = [&](const char* key, std::vector<std::string>& v) {
            if (const phxtool::JsonValue* a = root.find(key); a && a->is_arr()) {
                v.clear();
                for (const phxtool::JsonValue& s : a->arr) if (!s.as_str().empty()) v.push_back(s.as_str());
            }
        };
        strings("source", out.source);
        strings("assets", out.assets);
        strings("bundles", out.bundles);
        if (const phxtool::JsonValue* ls = root.find("launches"); ls && ls->is_arr())
            for (const phxtool::JsonValue& l : ls->arr) {
                ProjectLaunch pl;
                pl.label = l.str_at("label");
                pl.command = l.str_at("command");
                if (pl.label.empty() || pl.command.empty()) return fail("every launch needs a \"label\" and a \"command\"");
                const std::string g = l.str_at("group");
                if (!g.empty()) pl.group = g;
                pl.blurb = l.str_at("blurb");
                if (const phxtool::JsonValue* nd = l.find("needs"); nd && nd->is_arr())
                    for (const phxtool::JsonValue& s : nd->arr) pl.needs.push_back(s.as_str());
                pl.windowed = l.find("windowed") && l.find("windowed")->boolean;
                out.launches.push_back(pl);
            }
        return true;
    }

    std::string to_json() const {
        auto q = [](const std::string& s) {
            std::string o = "\"";
            for (char c : s) { if (c == '"' || c == '\\') o += '\\'; o += c; }
            return o + "\"";
        };
        auto list = [&](const std::vector<std::string>& v) {
            std::string o = "[";
            for (size_t i = 0; i < v.size(); ++i) o += (i ? ", " : "") + q(v[i]);
            return o + "]";
        };
        std::string j = "{\n  \"name\": " + q(name) + ",\n";
        if (!description.empty()) j += "  \"description\": " + q(description) + ",\n";
        j += "  \"source\": " + list(source) + ",\n  \"assets\": " + list(assets) + ",\n  \"bundles\": " + list(bundles) + ",\n";
        j += "  \"launches\": [";
        for (size_t i = 0; i < launches.size(); ++i) {
            const ProjectLaunch& l = launches[i];
            j += i ? ",\n    { " : "\n    { ";
            j += "\"label\": " + q(l.label) + ", \"group\": " + q(l.group) + ",\n      \"command\": " + q(l.command);
            if (!l.blurb.empty()) j += ",\n      \"blurb\": " + q(l.blurb);
            if (!l.needs.empty()) j += ", \"needs\": " + list(l.needs);
            if (l.windowed) j += ", \"windowed\": true";
            j += " }";
        }
        j += launches.empty() ? "]\n}\n" : "\n  ]\n}\n";
        return j;
    }
    bool save(std::string* err = nullptr) const {
        const std::string t = to_json();
        FILE* f = std::fopen(file().c_str(), "wb");
        if (!f) { if (err) *err = "cannot write " + file(); return false; }
        const bool ok = std::fwrite(t.data(), 1, t.size(), f) == t.size();
        std::fclose(f);
        if (!ok && err) *err = "short write to " + file();
        return ok;
    }

    // The bundles to show (absolute): the listed ones, plus every .phxp in the project's folder
    // and its build/ folder.
    std::vector<std::string> bundle_paths() const {
        std::vector<std::string> out;
        for (const std::string& b : bundles) out.push_back(canon_path(is_abs_path(b) ? b : dir + "/" + b));
        std::error_code ec;
        for (const pfs::path& d : { pfs::path(dir), pfs::path(dir) / "build" })
            for (pfs::directory_iterator it(d, ec), end; !ec && it != end; it.increment(ec))
                if (it->is_regular_file(ec) && it->path().extension() == ".phxp") {
                    const std::string p = canon_path(it->path().string());
                    if (std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
                }
        return out;
    }

    // The standard launches of a project built by the generic rules (`make game|game-assets|play`,
    // `play-gba`, `play-psp`). A need may name an SDK variable ($DEVKITARM; see expand_need_vars).
    static std::vector<ProjectLaunch> standard_launches() {
        const std::string mk = "make -C \"$PHX_ROOT\" ";
        return {
            ProjectLaunch{ "Play", "play", mk + "-s play PROJECT=\"$PHX_PROJECT\"",
                           "Build the game, bake its assets and run it in a window", { "sdl2-config" }, true },
            ProjectLaunch{ "Build", "build", mk + "game PROJECT=\"$PHX_PROJECT\"",
                           "Compile src/*.cpp against the engine into build/<name>", { "sdl2-config" }, false },
            ProjectLaunch{ "Bake assets", "build", mk + "game-assets PROJECT=\"$PHX_PROJECT\"",
                           "Bake assets/ into build/<name>.phxp (sprites, maps, sounds, tables, images)", {}, false },
            ProjectLaunch{ "GBA ROM", "console", mk + "-s play-gba PROJECT=\"$PHX_PROJECT\"",
                           "devkitARM -> build/<name>.gba (native PPU, size-gated), opened in mGBA if installed",
                           { "$DEVKITARM/bin/arm-none-eabi-g++" }, true },
            ProjectLaunch{ "PSP EBOOT", "console", mk + "-s play-psp PROJECT=\"$PHX_PROJECT\"",
                           "pspsdk -> build/psp/EBOOT.PBP, opened in PPSSPP if installed", { "psp-g++" }, true },
            ProjectLaunch{ "Export for PC", "build", mk + "-s game-export PROJECT=\"$PHX_PROJECT\" EXPORT=pc",
                           "A release build + its assets (+ DLLs on Windows) -> dist/<name>-<os>/ and a .zip for players",
                           { "sdl2-config" }, false },
            ProjectLaunch{ "Export for GBA", "console", mk + "-s game-export PROJECT=\"$PHX_PROJECT\" EXPORT=gba",
                           "The size-gated ROM -> dist/<name>-gba/ and a .zip", { "$DEVKITARM/bin/arm-none-eabi-g++" }, false },
            ProjectLaunch{ "Export for PSP", "console", mk + "-s game-export PROJECT=\"$PHX_PROJECT\" EXPORT=psp",
                           "PSP/GAME/<NAME>/EBOOT.PBP -> dist/<name>-psp/ and a .zip", { "psp-g++" }, false },
            ProjectLaunch{ "Measure budgets", "test", mk + "-s project-budget PROJECT=\"$PHX_PROJECT\"",
                           "Run the game headlessly as PC, GBA and PSP: memory, entities, sprites -> the Budget view",
                           {}, false },
            ProjectLaunch{ "Profile", "test", "export PHX_TRACE=build/trace.csv; " + mk + "-s play PROJECT=\"$PHX_PROJECT\"",
                           "Play, recording every frame's timings to build/trace.csv -> the Budget view's Profiler",
                           { "sdl2-config" }, true },
            ProjectLaunch{ "Debug", "test", mk + "-s game-debug PROJECT=\"$PHX_PROJECT\"",
                           "Play under gdb: a crash prints every thread's backtrace (file:line links) in the log",
                           { "sdl2-config", "gdb" }, true },
        };
    }
};

// The shell command a project launch runs: from the project folder, with $PHX_ROOT (the engine
// checkout) and $PHX_PROJECT (the project) exported. The job runner itself starts in the engine
// root, so the `cd` is what makes relative paths in the command mean the project.
inline std::string launch_shell_command(const ProjectLaunch& l, const std::string& engine_root, const std::string& dir) {
    auto q = [](const std::string& s) {
        std::string o = "'";
        for (char c : s) { if (c == '\'') o += "'\\''"; else o += c; }
        return o + "'";
    };
    return "export PHX_ROOT=" + q(engine_root) + " PHX_PROJECT=" + q(dir) + "; cd " + q(dir) + " && " + l.command;
}

// Project folders under <engine>/examples (one level) — the examples that ship as projects.
inline std::vector<std::string> discover_projects(const std::string& engine_root) {
    std::vector<std::string> out;
    std::error_code ec;
    for (pfs::directory_iterator it(pfs::path(engine_root) / "examples", ec), end; !ec && it != end; it.increment(ec))
        if (it->is_directory(ec) && ProjectDoc::is_project_dir(it->path().string())) out.push_back(canon_path(it->path().string()));
    std::sort(out.begin(), out.end());
    return out;
}

// A project name -> a folder / binary / bundle name (letters, digits, '_' and '-').
inline std::string project_slug(const std::string& name) {
    std::string o;
    for (char c : name) {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-') o += c;
        else if (c >= 'A' && c <= 'Z') o += char(c - 'A' + 'a');
        else if (c == ' ') o += '_';
    }
    return o;
}

// ---- New project: a small, complete, buildable game ----
namespace tmpl {

inline const char* main_cpp() {
    return R"CPP(// src/main.cpp — @NAME@: a Phosphorus game (made from the Phosphorus Studio project template).
//
// One source for every target: gameplay code talks to the engine's public API only (phx/...),
// never to a platform header, and the engine supplies main() for each target (PHX_GAME, at the
// bottom). Build + run from Phosphorus Studio (Run > Play, GBA ROM, PSP EBOOT) or from the engine
// checkout:  make play | play-gba | play-psp PROJECT=path/to/this/project
//
// The GAME is data you edit in the Studio, not code:
//   assets/flow.json     the screens and their order: the title, the levels (each a map, with
//                        its lives and HUD), game over, the ending (phx/runtime/flow.h)
//   assets/level.tmj     a level: tiles, per-tile collision (spikes are hazard tiles) and the
//                        spawns you place with the map editor's T tool
//   assets/prefabs.json  what each spawn type is: sprite, collider, and its components: the
//                        engine's stock behaviours (PlatformerController, Patrol, Pickup, Hazard,
//                        Checkpoint, Exit, CameraFollow) tuned by columns, or your own
// This file just runs the flow. Add your own rules in on_fixed_update, around flow.update():
// flow.behaviours() has this step's contacts, flow.total("coins"_hash) the tallies.
#include "phx/runtime/main.h"
#include "phx/runtime/flow.h"
#include "phx/resource/cache.h"
#include "phx/core/log.h"

using namespace phx;

namespace {

struct @TYPE@ final : Game {
    ResourceCache* res = nullptr;
    GameFlow       flow;

    // Before boot. The budgets arrive sized for the target (GBA, PSP, PC); 240x160 is the GBA's
    // screen, which fits every target.
    void on_configure(Config& cfg) override {
        cfg.title  = "@NAME@";
        cfg.width  = 240;
        cfg.height = 160;
        cfg.sim_hz = 60;
    }

    void on_start(App& app) override {
        res = ResourceCache::create(app.mem().persistent()).unwrap();
        if (res->mount(app.platform(), "build/@SLUG@.phxp") != Status::Ok) {
            PHX_LOG_ERROR("@SLUG@: no bundle - bake the assets first (Run > Bake assets)");
            return;
        }
        flow.start(app, *res);
    }

    void on_fixed_update(App& app, scalar dt) override { flow.update(app, dt); }
    void on_render(App& app, scalar) override { flow.render(app); }
};

} // namespace

// The game. The engine's main() for each target (PC, GBA, PSP) boots it: no main() here.
PHX_GAME(@TYPE@);
)CPP";
}

inline std::string replace_all(std::string s, const std::string& a, const std::string& b) {
    for (size_t p = s.find(a); p != std::string::npos; p = s.find(a, p + b.size())) s.replace(p, a.size(), b);
    return s;
}

// The tileset: 8 tiles of 8x8 (grass, dirt, brick, plank, spikes, cloud, bush, flower).
inline PixelDoc tileset() {
    PixelDoc t = PixelDoc::blank(64, 8, 0);
    const uint32_t grass = px_rgba(88, 176, 72), grass_d = px_rgba(52, 120, 52), dirt = px_rgba(140, 92, 56),
                   dirt_d = px_rgba(104, 66, 40), brick = px_rgba(150, 150, 170), mortar = px_rgba(96, 96, 116),
                   wood = px_rgba(190, 136, 76), wood_d = px_rgba(130, 86, 44), spike = px_rgba(210, 210, 224),
                   cloud = px_rgba(236, 240, 255), leaf = px_rgba(64, 150, 70), petal = px_rgba(240, 110, 150);
    t.rect(0, 0, 7, 7, dirt, true);  t.rect(0, 0, 7, 2, grass, true);  t.set(2, 3, grass_d); t.set(6, 3, grass_d);
    t.set(3, 5, dirt_d); t.set(6, 6, dirt_d);
    t.rect(8, 0, 15, 7, dirt, true); t.set(9, 2, dirt_d); t.set(13, 4, dirt_d); t.set(11, 6, dirt_d);
    t.rect(16, 0, 23, 7, brick, true); t.rect(16, 3, 23, 3, mortar, true); t.rect(16, 7, 23, 7, mortar, true);
    t.set(19, 0, mortar); t.set(19, 1, mortar); t.set(19, 2, mortar); t.set(21, 4, mortar); t.set(21, 5, mortar); t.set(21, 6, mortar);
    t.rect(24, 0, 31, 2, wood, true); t.rect(24, 2, 31, 2, wood_d, true); t.set(25, 3, wood_d); t.set(30, 3, wood_d);
    for (int k = 0; k < 4; ++k) { t.line(32 + k * 2, 7, 32 + k * 2 + 1, 3, spike); t.set(32 + k * 2 + 1, 7, spike); }
    t.ellipse(40, 2, 47, 7, cloud, true); t.ellipse(42, 0, 46, 5, cloud, true);
    t.ellipse(48, 2, 55, 7, leaf, true); t.set(50, 4, grass_d); t.set(53, 5, grass_d);
    t.line(59, 3, 59, 7, leaf); t.rect(58, 1, 60, 3, petal, true); t.set(59, 2, px_rgba(250, 220, 90));
    return t;
}

// The hero: 6 frames of 16x16 — a walk cycle (frame 0 doubles as the idle pose), a jump pose
// (stretched, feet tucked) and a landing squash.
inline PixelDoc hero() {
    PixelDoc h = PixelDoc::blank(96, 16, 0);
    const uint32_t body = px_rgba(255, 138, 48), dark = px_rgba(170, 70, 30), eye = px_rgba(250, 250, 250),
                   pupil = px_rgba(30, 20, 30), foot = px_rgba(120, 50, 30);
    for (int f = 0; f < 4; ++f) {
        const int x = f * 16, bob = (f % 2) ? 1 : 0;
        h.ellipse(x + 3, 2 + bob, x + 12, 12 + bob, body, true);
        h.ellipse(x + 3, 2 + bob, x + 12, 12 + bob, dark, false);
        h.rect(x + 8, 5 + bob, x + 10, 7 + bob, eye, true);
        h.set(x + 10, 6 + bob, pupil);
        h.set(x + 5, 1 + bob, body); h.set(x + 6, 0 + bob, body);          // a little crest
        const int step = (f == 1) ? 2 : (f == 3) ? -2 : 0;
        h.rect(x + 5 + step, 13, x + 6 + step, 15, foot, true);
        h.rect(x + 9 - step, 13, x + 10 - step, 15, foot, true);
    }
    {                                                       // frame 4: jump
        const int x = 64;
        h.ellipse(x + 4, 0, x + 11, 11, body, true);
        h.ellipse(x + 4, 0, x + 11, 11, dark, false);
        h.rect(x + 8, 3, x + 10, 5, eye, true);
        h.set(x + 10, 4, pupil);
        h.rect(x + 4, 12, x + 5, 13, foot, true);
        h.rect(x + 10, 12, x + 11, 13, foot, true);
    }
    {                                                       // frame 5: land
        const int x = 80;
        h.ellipse(x + 1, 6, x + 14, 14, body, true);
        h.ellipse(x + 1, 6, x + 14, 14, dark, false);
        h.rect(x + 9, 8, x + 11, 10, eye, true);
        h.set(x + 11, 9, pupil);
        h.rect(x + 3, 14, x + 5, 15, foot, true);
        h.rect(x + 10, 14, x + 12, 15, foot, true);
    }
    return h;
}

// The coin: 2 frames of 8x8 (a spin).
inline PixelDoc coin() {
    PixelDoc c = PixelDoc::blank(16, 8, 0);
    const uint32_t gold = px_rgba(250, 206, 64), dark = px_rgba(196, 128, 32), shine = px_rgba(255, 246, 200);
    c.ellipse(1, 1, 6, 6, gold, true);  c.ellipse(1, 1, 6, 6, dark, false);  c.set(3, 2, shine); c.set(3, 3, shine);
    c.ellipse(10, 1, 13, 6, gold, true); c.ellipse(10, 1, 13, 6, dark, false); c.set(11, 3, shine);
    return c;
}

// The slime: 2 frames of 16x16 (a squish).
inline PixelDoc slime() {
    PixelDoc s = PixelDoc::blank(32, 16, 0);
    const uint32_t body = px_rgba(96, 200, 110), dark = px_rgba(40, 120, 60), eye = px_rgba(250, 250, 250),
                   pupil = px_rgba(20, 30, 20);
    for (int f = 0; f < 2; ++f) {
        const int x = f * 16, top = f ? 7 : 5;               // frame 1 squashes down
        s.ellipse(x + 1, top, x + 14, 15, body, true);
        s.ellipse(x + 1, top, x + 14, 15, dark, false);
        s.rect(x + 4, top + 3, x + 5, top + 4, eye, true);  s.set(x + 5, top + 4, pupil);
        s.rect(x + 9, top + 3, x + 10, top + 4, eye, true); s.set(x + 10, top + 4, pupil);
    }
    return s;
}

// The door: one 16x16 frame (the level's exit).
inline PixelDoc door() {
    PixelDoc d = PixelDoc::blank(16, 16, 0);
    const uint32_t wood = px_rgba(150, 96, 56), dark = px_rgba(96, 58, 32), knob = px_rgba(250, 210, 90);
    d.rect(3, 1, 12, 15, wood, true);
    d.rect(3, 1, 12, 15, dark, false);
    d.rect(5, 3, 10, 7, dark, false);
    d.rect(5, 9, 10, 13, dark, false);
    d.set(10, 8, knob);
    return d;
}

// The sign: one 16x16 frame (a Talk: walk up to it and press Up).
inline PixelDoc sign() {
    PixelDoc d = PixelDoc::blank(16, 16, 0);
    const uint32_t wood = px_rgba(186, 132, 78), dark = px_rgba(110, 70, 38), ink = px_rgba(70, 44, 24);
    d.rect(7, 9, 8, 15, dark, true);                       // the post
    d.rect(1, 2, 14, 9, wood, true);                       // the board
    d.rect(1, 2, 14, 9, dark, false);
    d.line(3, 4, 11, 4, ink); d.line(3, 6, 9, 6, ink);     // some writing
    return d;
}

// The font sheet the flow's screens and HUD write with: printable ASCII in 8x8 cells, 16 per row.
inline PixelDoc font() {
    PixelDoc f = PixelDoc::blank(phxtool::kAsciiFontW, phxtool::kAsciiFontH, 0);
    phxtool::build_ascii_font(f.px.data());
    return f;
}

// The font the flow's screens and HUD use: assets/font.png (a 5x7 ASCII sheet in 8x8 cells) as a
// PROPORTIONAL font (each glyph advances by its width + 1; the Studio's font editor measures it).
inline phxtool::FontDef font_def() {
    phxtool::FontDef d;
    d.image = "font.png";
    d.cell_w = 8; d.cell_h = 8; d.first = 32;
    d.proportional = true; d.spacing = 1; d.space = 3; d.line_h = 9;
    return d;
}

// The game flow (a phxbin table, phx/runtime/flow.h): the screens in order. `name` is the game's
// title line. Add a level: a row of kind "level" with its own `map` (a new .tmj). The title screen
// starts assets/theme.song (the Studio's song editor); screens with no `music` keep it playing.
inline std::string flow_json(const std::string& name) {
    std::string title;
    for (char c : name) if (c != '"' && c != '\\' && c != '|') title += c;
    if (title.size() > 40) title.resize(40);
    return "{ \"struct\":\"Screen\",\n"
           "  \"fields\":[{\"name\":\"name\",\"type\":\"str16\"}, {\"name\":\"kind\",\"type\":\"str16\"},"
           " {\"name\":\"map\",\"type\":\"str16\"}, {\"name\":\"text\",\"type\":\"str64\"}, {\"name\":\"next\",\"type\":\"str16\"},"
           " {\"name\":\"lives\",\"type\":\"u8\"}, {\"name\":\"counter\",\"type\":\"str16\"}, {\"name\":\"label\",\"type\":\"str16\"},"
           " {\"name\":\"music\",\"type\":\"str16\"}],\n"
           "  \"records\":[\n"
           "    {\"name\":\"title\", \"kind\":\"title\", \"text\":\"" + title + "|a Phosphorus game\", \"music\":\"theme\"},\n"
           "    {\"name\":\"level1\", \"kind\":\"level\", \"map\":\"level\", \"text\":\"LEVEL 1\", \"lives\":3,"
           " \"counter\":\"coins\", \"label\":\"COINS\"},\n"
           "    {\"name\":\"end\", \"kind\":\"end\", \"text\":\"YOU WIN!|thanks for playing\"},\n"
           "    {\"name\":\"gameover\", \"kind\":\"title\", \"text\":\"GAME OVER\", \"next\":\"level1\"}\n"
           "  ] }\n";
}

// The prefab table (a phxbin table, edited in the Studio's table editor): what each spawn type in
// the level is made of. The engine's level loader reads these columns by name (phx/runtime/level.h),
// and `components` attaches behaviours (phx/runtime/behaviours.h, or your own PHX_COMPONENTs) tuned
// by `Component_field` columns. Collision: the player (layer 1) reports coins (2), slimes (4), the
// door (8) and the sign (16).
inline const char* prefabs_json() {
    return "{ \"struct\":\"Prefab\",\n"
           "  \"fields\":[{\"name\":\"type\",\"type\":\"str16\"}, {\"name\":\"sprite\",\"type\":\"str16\"},"
           " {\"name\":\"w\",\"type\":\"u8\"}, {\"name\":\"h\",\"type\":\"u8\"}, {\"name\":\"body\",\"type\":\"u8\"},"
           " {\"name\":\"layer\",\"type\":\"u16\"}, {\"name\":\"mask\",\"type\":\"u16\"},"
           " {\"name\":\"components\",\"type\":\"str64\"}, {\"name\":\"Pickup_value\",\"type\":\"i16\"},"
           " {\"name\":\"Pickup_sound\",\"type\":\"str16\"}, {\"name\":\"Patrol_range\",\"type\":\"i16\"},"
           " {\"name\":\"Talk_conversation\",\"type\":\"str16\"}],\n"
           "  \"records\":[\n"
           "    {\"type\":\"player\", \"sprite\":\"hero\", \"w\":10, \"h\":16, \"body\":1, \"layer\":1, \"mask\":30,"
           " \"components\":\"PlatformerController CameraFollow\"},\n"
           "    {\"type\":\"coin\", \"sprite\":\"coin\", \"w\":8, \"h\":8, \"body\":0, \"layer\":2, \"mask\":1,"
           " \"components\":\"Pickup\", \"Pickup_value\":1, \"Pickup_sound\":\"coin\"},\n"
           "    {\"type\":\"slime\", \"sprite\":\"slime\", \"w\":12, \"h\":16, \"body\":1, \"layer\":4, \"mask\":1,"
           " \"components\":\"Patrol Hazard\", \"Patrol_range\":24},\n"
           "    {\"type\":\"door\", \"sprite\":\"door\", \"w\":12, \"h\":16, \"body\":0, \"layer\":8, \"mask\":1,"
           " \"components\":\"Exit\"},\n"
           "    {\"type\":\"sign\", \"sprite\":\"sign\", \"w\":12, \"h\":16, \"body\":0, \"layer\":16, \"mask\":1,"
           " \"components\":\"Talk\", \"Talk_conversation\":\"sign\"}\n"
           "  ] }\n";
}

// The template's sound effects, as .sfx documents (tools/phxpack/synth.h; the Studio's SFX editor):
// the bake renders them exactly as the editor plays them. A rising jump and a coin's two notes.
inline phxtool::SfxParams jump_sfx() {
    phxtool::SfxParams p;
    p.wave = phxtool::Wave::Square; p.duty = 0.4; p.freq = 300; p.slide = 5;
    p.sustain = 0.08; p.decay = 0.1; p.volume = 0.45;
    return p;
}
inline phxtool::SfxParams coin_sfx() {
    phxtool::SfxParams p;
    p.wave = phxtool::Wave::Square; p.duty = 0.5; p.freq = 988; p.arp_mult = 1.335; p.arp_time = 0.07;
    p.sustain = 0.06; p.punch = 0.3; p.decay = 0.12; p.volume = 0.45;
    return p;
}

// The level: 30x20 tiles of 8x8 (one 240x160 screen), a backdrop layer + the gameplay layer.
inline phxtool::TmapDoc level() {
    phxtool::TmapDoc d = phxtool::TmapDoc::blank(30, 20, 8, 8, "tiles");
    d.tileset_image = "tiles.png";
    d.tileset_cols = 8; d.tileset_count = 8; d.tileset_img_w = 64; d.tileset_img_h = 8;
    d.layer_names[0] = "back";
    d.layer_parallax[0] = { 0.5, 1.0 };
    d.add_layer("main");
    // clouds + bushes behind
    for (int x : { 3, 11, 20, 26 }) { d.set_tile(0, x, 3 + (x % 3), 6); d.set_tile(0, x + 1, 3 + (x % 3), 6); }
    for (int x : { 6, 14, 23 }) d.set_tile(0, x, 16, 7);
    // ground, a raised block, a plank platform, spikes, flowers
    for (int x = 0; x < 30; ++x) { d.set_tile(1, x, 17, 1); d.set_tile(1, x, 18, 2); d.set_tile(1, x, 19, 2); }
    for (int x = 18; x < 22; ++x) { d.set_tile(1, x, 15, 3); d.set_tile(1, x, 16, 3); }
    for (int x = 8; x < 13; ++x) d.set_tile(1, x, 12, 4);
    d.set_tile(1, 25, 16, 5); d.set_tile(1, 26, 16, 5);
    d.set_tile(1, 3, 16, 8); d.set_tile(1, 15, 16, 8);
    d.set_tile_flag(1, phx::kTileFlagSolid); d.set_tile_flag(2, phx::kTileFlagSolid); d.set_tile_flag(3, phx::kTileFlagSolid);
    d.set_tile_flag(4, phx::kTileFlagOneWay); d.set_tile_flag(5, phx::kTileFlagHazard);
    d.add_spawn("player", 40, 120);
    d.spawns.back().name = "player";
    // coins: on the plank, on the raised block, and floating over the ground
    d.add_spawn("coin", 80, 84);
    d.add_spawn("coin", 160, 108);
    d.add_spawn("coin", 120, 116);
    d.set_prop(2, "Pickup_value", "int", "5");  // the coin on the block is worth 5 (a spawn property)
    d.add_spawn("slime", 100, 126);             // patrols the ground under the plank
    d.spawns.back().name = "slime";
    d.add_spawn("door", 228, 128);              // the exit: the level ends here (Exit)
    d.spawns.back().name = "door";
    d.add_spawn("sign", 64, 128);               // talks (Talk: Up plays assets/dialogue.dlg's "sign")
    d.spawns.back().name = "sign";
    return d;
}

} // namespace tmpl

// Create a new project folder `dir` named `name` from the template: phxproject.json, src/main.cpp,
// assets/ (hero + coin sprites, tileset, level map, prefab table, sounds), README.md. Refuses a
// non-empty folder.
inline bool create_project(const std::string& dir, const std::string& name, std::string* err = nullptr) {
    auto fail = [&](const std::string& why) { if (err) *err = why; return false; };
    const std::string slug = project_slug(name.empty() ? base_name(dir) : name);
    if (slug.empty()) return fail("the project needs a name made of letters or digits");
    std::error_code ec;
    if (pfs::exists(dir, ec) && !pfs::is_empty(dir, ec)) return fail(dir + " already exists and is not empty");
    pfs::create_directories(pfs::path(dir) / "src", ec);
    pfs::create_directories(pfs::path(dir) / "assets", ec);
    if (ec) return fail("cannot create " + dir + ": " + ec.message());
    auto write = [&](const std::string& rel, const std::string& text) {
        FILE* f = std::fopen((dir + "/" + rel).c_str(), "wb");
        if (!f) return false;
        const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
        std::fclose(f);
        return ok;
    };
    std::string type;                            // a C++ type name for the game: MyGameGame
    bool up = true;
    for (char c : slug) {
        if (c == '_' || c == '-') { up = true; continue; }
        type += up && c >= 'a' && c <= 'z' ? char(c - 'a' + 'A') : c;
        up = false;
    }
    if (type.empty() || (type[0] >= '0' && type[0] <= '9')) type = "G" + type;
    type += "Game";
    std::string main_cpp = tmpl::replace_all(tmpl::main_cpp(), "@NAME@", name.empty() ? slug : name);
    main_cpp = tmpl::replace_all(main_cpp, "@SLUG@", slug);
    main_cpp = tmpl::replace_all(main_cpp, "@TYPE@", type);
    if (!write("src/main.cpp", main_cpp)) return fail("cannot write src/main.cpp");

    std::string e;
    PixelDoc ts = tmpl::tileset();
    if (!ts.save_png(dir + "/assets/tiles.png", &e)) return fail(e);
    PixelDoc hero = tmpl::hero();
    if (!hero.save_png(dir + "/assets/hero.png", &e)) return fail(e);
    SprDoc spr;
    spr.sheet = "hero.png"; spr.frame_w = 16; spr.frame_h = 16;
    spr.clips = { SprClip{ "idle", 0, 1, 1, true }, SprClip{ "walk", 0, 4, 8, true },
                  SprClip{ "jump", 4, 1, 0, false }, SprClip{ "land", 5, 1, 12, false } };
    // its state machine: PlatformerController sends move/stop/jump/fall/land; "done" ends the landing
    spr.trans = { SprEdge{ "idle", "walk", "move" }, SprEdge{ "walk", "idle", "stop" },
                  SprEdge{ "*", "jump", "jump" },    SprEdge{ "*", "jump", "fall" },
                  SprEdge{ "jump", "land", "land" }, SprEdge{ "land", "idle", "done" } };
    if (!spr.save(dir + "/assets/hero.sprdef", &e)) return fail(e);
    phxtool::TmapDoc lvl = tmpl::level();
    if (!lvl.save_file(dir + "/assets/level.tmj")) return fail("cannot write assets/level.tmj");
    if (!write("assets/jump.sfx", phxtool::sfx_to_json(tmpl::jump_sfx()))) return fail("cannot write assets/jump.sfx");
    if (!write("assets/coin.sfx", phxtool::sfx_to_json(tmpl::coin_sfx()))) return fail("cannot write assets/coin.sfx");
    if (!write("assets/theme.song", phxtool::song_to_json(phxtool::song_starter()))) return fail("cannot write assets/theme.song");
    PixelDoc coin = tmpl::coin();
    if (!coin.save_png(dir + "/assets/coin.png", &e)) return fail(e);
    SprDoc coin_spr;
    coin_spr.sheet = "coin.png"; coin_spr.frame_w = 8; coin_spr.frame_h = 8;
    coin_spr.clips = { SprClip{ "spin", 0, 2, 6, true } };
    if (!coin_spr.save(dir + "/assets/coin.sprdef", &e)) return fail(e);
    PixelDoc slime = tmpl::slime();
    if (!slime.save_png(dir + "/assets/slime.png", &e)) return fail(e);
    SprDoc slime_spr;
    slime_spr.sheet = "slime.png"; slime_spr.frame_w = 16; slime_spr.frame_h = 16;
    slime_spr.clips = { SprClip{ "idle", 0, 2, 3, true } };
    if (!slime_spr.save(dir + "/assets/slime.sprdef", &e)) return fail(e);
    PixelDoc door = tmpl::door();
    if (!door.save_png(dir + "/assets/door.png", &e)) return fail(e);
    PixelDoc sign = tmpl::sign();
    if (!sign.save_png(dir + "/assets/sign.png", &e)) return fail(e);
    if (!write("assets/dialogue.dlg", phxtool::dlg_to_json(phxtool::dlg_starter()))) return fail("cannot write assets/dialogue.dlg");
    PixelDoc font = tmpl::font();
    if (!font.save_png(dir + "/assets/font.png", &e)) return fail(e);
    if (!write("assets/font.font", phxtool::fontdef_to_json(tmpl::font_def()))) return fail("cannot write assets/font.font");
    if (!write("assets/flow.json", tmpl::flow_json(name.empty() ? slug : name))) return fail("cannot write assets/flow.json");
    if (!write("assets/prefabs.json", tmpl::prefabs_json())) return fail("cannot write assets/prefabs.json");

    ProjectDoc p;
    p.dir = canon_path(dir);
    p.name = name.empty() ? slug : name;
    p.description = "A Phosphorus game";
    p.bundles = { "build/" + slug + ".phxp" };
    p.launches = ProjectDoc::standard_launches();
    if (!p.save(&e)) return fail(e);
    const std::string readme =
        "# " + p.name + "\n\n"
        "A Phosphorus game project (open it in Phosphorus Studio: `phxstudio --project " + dir + "`).\n\n"
        "| Folder | What lives there |\n|---|---|\n"
        "| `src/` | the game's C++ (it uses the engine's public API, `phx/...`, only) |\n"
        "| `assets/` | author files: sprites (`.png` + `.sprdef`), maps (`.tmj`), sound effects (`.sfx`), music (`.song`), sounds (`.wav`), fonts (`.font`), dialogue (`.dlg`), data tables (`.json`) |\n"
        "| `build/` | what the build makes: the game (`build/" + slug + "`) and its bundle (`build/" + slug + ".phxp`) |\n\n"
        "From the Phosphorus checkout: `make play PROJECT=" + dir + "` builds, bakes and runs it;\n"
        "`make game` / `make game-assets` do one step each.\n";
    if (!write("README.md", readme)) return fail("cannot write README.md");
    return true;
}

} // namespace phxstudio
#endif // PHX_TOOLS_PHXSTUDIO_PROJECTDOC_H
