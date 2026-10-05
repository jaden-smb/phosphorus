// tools/phxpack/builders.h — the shared bake logic (host-only, STL allowed). Turns an
// author-friendly source file (.png/.ppm/.tmcsv/.tmj/.wav/.sprdef/.json) into the matching
// engine asset(s) inside a BundleWriter. Both the `phxpack` assembler and the per-format
// converter front-ends (phxsprite/phxtile/phxsnd/phxbin) call these, so there is exactly ONE
// bake path (docs/08 §9 — "keeps the bake path single and testable").
#ifndef PHX_TOOLS_BUILDERS_H
#define PHX_TOOLS_BUILDERS_H

#include "bundle_writer.h"
#include "png.h"
#include "tiled.h"
#include "wav.h"
#include "json.h"
#include "synth.h"
#include "font.h"
#include "dialogue.h"      // .dlg -> a Dialogue asset (also Phosphorus Studio's dialogue editor)          // .font / .fnt -> a glyph table (also Phosphorus Studio's font editor)         // .sfx / .song -> PCM (the SFX generator and the tracker)
#include "analyze.h"       // tools/phxviz — offline visualization-track analysis (build_viz)

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace phxtool {

// ---- path helpers ---------------------------------------------------------
inline std::string stem(const std::string& path) {
    size_t s = path.find_last_of("/\\");
    size_t d = path.find_last_of('.');
    size_t b = (s == std::string::npos) ? 0 : s + 1;
    size_t e = (d == std::string::npos || d < b) ? path.size() : d;
    return path.substr(b, e - b);
}
inline std::string dir_of(const std::string& path) {
    size_t s = path.find_last_of("/\\");
    return s == std::string::npos ? std::string() : path.substr(0, s + 1);
}
inline bool ends_with(const std::string& s, const char* suf) {
    size_t n = std::strlen(suf);
    return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}

inline bool read_file(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz < 0) { std::fclose(f); return false; }
    out.resize(size_t(sz));
    size_t got = std::fread(out.data(), 1, size_t(sz), f);
    std::fclose(f);
    return got == size_t(sz);
}

// ---- textures: PNG / PPM --------------------------------------------------
inline bool build_png(BundleWriter& w, const std::string& in, const std::string& name = "") {
    std::vector<uint8_t> bytes;
    if (!read_file(in, bytes)) { std::fprintf(stderr, "phx: cannot read '%s'\n", in.c_str()); return false; }
    std::vector<uint32_t> px; uint16_t iw, ih;
    if (!png_decode(bytes.data(), bytes.size(), px, iw, ih)) {
        std::fprintf(stderr, "phx: bad/unsupported PNG '%s'\n", in.c_str()); return false; }
    const std::string nm = name.empty() ? stem(in) : name;
    w.add_texture(nm, px.data(), iw, ih);
    std::printf("  + texture %-12s %dx%d  (%s, PNG)\n", nm.c_str(), iw, ih, in.c_str());
    return true;
}

inline bool load_ppm(const std::string& path, std::vector<uint32_t>& out, uint16_t& w, uint16_t& h) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char magic[3] = {0};
    if (std::fscanf(f, "%2s", magic) != 1 || std::strcmp(magic, "P6") != 0) { std::fclose(f); return false; }
    int iw = 0, ih = 0, maxv = 0;
    if (std::fscanf(f, "%d %d %d", &iw, &ih, &maxv) != 3 || iw <= 0 || ih <= 0) { std::fclose(f); return false; }
    std::fgetc(f);                       // single whitespace after maxval
    w = uint16_t(iw); h = uint16_t(ih);
    out.resize(size_t(iw) * ih);
    for (size_t i = 0; i < out.size(); ++i) {
        int r = std::fgetc(f), g = std::fgetc(f), b = std::fgetc(f);
        if (b == EOF) { std::fclose(f); return false; }
        out[i] = uint32_t(r) | (uint32_t(g) << 8) | (uint32_t(b) << 16) | (uint32_t(255) << 24);
    }
    std::fclose(f);
    return true;
}
inline bool build_ppm(BundleWriter& w, const std::string& in, const std::string& name = "") {
    std::vector<uint32_t> px; uint16_t iw, ih;
    if (!load_ppm(in, px, iw, ih)) { std::fprintf(stderr, "phx: bad PPM '%s'\n", in.c_str()); return false; }
    const std::string nm = name.empty() ? stem(in) : name;
    w.add_texture(nm, px.data(), iw, ih);
    std::printf("  + texture %-12s %dx%d  (%s)\n", nm.c_str(), iw, ih, in.c_str());
    return true;
}

// ---- tilemaps: .tmcsv (stand-in) / Tiled .tmj -----------------------------
inline bool load_tmcsv(const std::string& path, std::vector<uint16_t>& idx,
                       uint16_t& w, uint16_t& h, std::string& tileset, int& tw, int& th) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char line[1024];
    bool have_header = false;
    std::vector<std::vector<uint16_t>> rows;
    while (std::fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        if (!have_header) {
            char name[256];
            if (std::sscanf(line, "%255s %d %d", name, &tw, &th) != 3) { std::fclose(f); return false; }
            tileset = name; have_header = true; continue;
        }
        std::vector<uint16_t> row;
        for (char* p = line; *p; ) {
            while (*p && !std::isdigit((unsigned char)*p)) ++p;
            if (!*p) break;
            row.push_back(uint16_t(std::strtol(p, &p, 10)));
        }
        if (!row.empty()) rows.push_back(std::move(row));
    }
    std::fclose(f);
    if (!have_header || rows.empty()) return false;
    h = uint16_t(rows.size());
    w = uint16_t(rows[0].size());
    idx.clear(); idx.reserve(size_t(w) * h);
    for (auto& row : rows)
        for (uint16_t x = 0; x < w; ++x) idx.push_back(x < row.size() ? row[x] : 0);
    return true;
}
inline bool build_tmcsv(BundleWriter& w, const std::string& in, const std::string& name = "") {
    std::vector<uint16_t> idx; uint16_t iw, ih; std::string ts; int tw, th;
    if (!load_tmcsv(in, idx, iw, ih, ts, tw, th)) { std::fprintf(stderr, "phx: bad tmcsv '%s'\n", in.c_str()); return false; }
    const std::string nm = name.empty() ? stem(in) : name;
    w.add_tilemap(nm, idx.data(), iw, ih, 1, uint8_t(tw), uint8_t(th), ts);
    std::printf("  + tilemap %-12s %dx%d tiles, tileset '%s'  (%s)\n", nm.c_str(), iw, ih, ts.c_str(), in.c_str());
    return true;
}

// The spawn extension (phx/resource/bundle.h) for a map's spawns: every spawn's name, then its
// properties typed as Tiled typed them (int, float, bool; anything else is a string). Empty when
// no spawn has a name or a property, so old-style maps bake byte-identically.
inline std::vector<uint8_t> spawn_ext_bytes(const std::vector<TiledSpawn>& spawns) {
    bool any = false;
    for (const TiledSpawn& s : spawns) any = any || !s.name.empty() || !s.props.empty();
    if (!any) return {};
    std::vector<phx::SpawnPropDef> props;
    std::string strings;
    for (size_t i = 0; i < spawns.size(); ++i)
        for (const TiledProp& p : spawns[i].props) {
            phx::SpawnPropDef d{};
            d.key = phx::fnv1a(p.name.c_str());
            d.spawn = uint16_t(i);
            if (p.type == "int") {
                d.type = phx::kPropInt;
                const long long v = std::strtoll(p.value.c_str(), nullptr, 10);
                d.value = int32_t(v < INT32_MIN ? INT32_MIN : v > INT32_MAX ? INT32_MAX : v);
            } else if (p.type == "float") {
                d.type = phx::kPropFloat;
                const float f = float(std::strtod(p.value.c_str(), nullptr));
                std::memcpy(&d.value, &f, 4);
            } else if (p.type == "bool") {
                d.type = phx::kPropBool;
                d.value = (p.value == "true" || p.value == "1") ? 1 : 0;
            } else {
                d.type = phx::kPropStr;
                d.value = int32_t(strings.size());
                strings += p.value;
                strings += '\0';
            }
            props.push_back(d);
        }
    std::vector<uint8_t> out;
    auto put = [&](const void* data, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(data);
        out.insert(out.end(), b, b + n);
    };
    const uint32_t hdr[3] = { phx::kSpawnExtMagic, uint32_t(props.size()), uint32_t(strings.size()) };
    put(hdr, sizeof(hdr));
    for (const TiledSpawn& s : spawns) {
        const phx::NameHash h = s.name.empty() ? 0 : phx::fnv1a(s.name.c_str());
        put(&h, 4);
    }
    if (!props.empty()) put(props.data(), props.size() * sizeof(phx::SpawnPropDef));
    put(strings.data(), strings.size());
    return out;
}

// Tiled .tmj -> Tilemap (+ Spawns). Both assets share `name` (matches the runtime lookup).
inline bool build_tmj(BundleWriter& w, const std::string& in, const std::string& name = "") {
    std::vector<uint8_t> bytes;
    if (!read_file(in, bytes)) { std::fprintf(stderr, "phx: cannot read '%s'\n", in.c_str()); return false; }
    TiledMap tm;
    std::string terr;
    if (!tiled_load(std::string(bytes.begin(), bytes.end()), tm, &terr)) {
        std::fprintf(stderr, "phx: bad Tiled map '%s': %s\n", in.c_str(), terr.c_str()); return false; }
    const std::string nm = name.empty() ? stem(in) : name;
    std::vector<uint16_t> flat;
    flat.reserve(tm.layers.size() * size_t(tm.width) * size_t(tm.height));
    for (const auto& L : tm.layers) flat.insert(flat.end(), L.begin(), L.end());
    w.add_tilemap(nm, flat.data(), uint16_t(tm.width), uint16_t(tm.height),
                  uint8_t(tm.layers.size()), uint8_t(tm.tile_w), uint8_t(tm.tile_h), tm.tileset,
                  tm.has_parallax() ? &tm.layer_parallax : nullptr,
                  tm.has_tile_flags() ? &tm.tile_flags : nullptr);
    std::printf("  + tilemap %-12s %dx%d tiles x%u layers%s%s, tileset '%s'  (%s, Tiled)\n",
                nm.c_str(), tm.width, tm.height, unsigned(tm.layers.size()),
                tm.has_parallax() ? " (parallax)" : "",
                tm.has_tile_flags() ? " (tile collision flags)" : "", tm.tileset.c_str(), in.c_str());
    if (!tm.spawns.empty()) {
        std::vector<phx::SpawnDef> sd;
        for (const auto& s : tm.spawns)
            sd.push_back(phx::SpawnDef{ phx::fnv1a((s.type.empty() ? s.name : s.type).c_str()),
                                        int16_t(s.x), int16_t(s.y), uint16_t(s.w), uint16_t(s.h) });
        size_t nprops = 0;
        for (const auto& s : tm.spawns) nprops += s.props.size();
        w.add_spawns(nm, sd, spawn_ext_bytes(tm.spawns));
        std::printf("  + spawns  %-12s %u objects%s  (%s)\n", nm.c_str(), unsigned(sd.size()),
                    nprops ? (", " + std::to_string(nprops) + " properties").c_str() : "", in.c_str());
    }
    return true;
}

// ---- sound: WAV -> mono16 -------------------------------------------------
inline bool build_wav(BundleWriter& w, const std::string& in, const std::string& name = "") {
    std::vector<uint8_t> bytes;
    if (!read_file(in, bytes)) { std::fprintf(stderr, "phx: cannot read '%s'\n", in.c_str()); return false; }
    std::vector<int16_t> mono; uint32_t rate = 0;
    if (!wav_decode(bytes.data(), bytes.size(), mono, rate)) {
        std::fprintf(stderr, "phx: bad/unsupported WAV '%s'\n", in.c_str()); return false; }
    const std::string nm = name.empty() ? stem(in) : name;
    w.add_sound(nm, mono.data(), uint32_t(mono.size()), rate);
    std::printf("  + sound   %-12s %u frames @ %u Hz  (%s, WAV)\n",
                nm.c_str(), unsigned(mono.size()), rate, in.c_str());
    return true;
}

// ---- synthesized audio: .sfx (sound-effect parameters) / .song (tracker) -> a Sound asset ----
// Rendered at kSynthRate by synth.h (the same code Phosphorus Studio auditions), then baked exactly like
// a WAV: add_sound() resamples it for tier 0.
inline bool build_sfx(BundleWriter& w, const std::string& in, const std::string& name = "") {
    std::vector<uint8_t> bytes;
    if (!read_file(in, bytes)) { std::fprintf(stderr, "phx: cannot read '%s'\n", in.c_str()); return false; }
    SfxParams p;
    std::string err;
    if (!sfx_from_json(std::string(bytes.begin(), bytes.end()), p, &err)) {
        std::fprintf(stderr, "phx: bad sound effect '%s': %s\n", in.c_str(), err.c_str()); return false; }
    const std::vector<int16_t> pcm = render_sfx(p);
    if (pcm.empty()) { std::fprintf(stderr, "phx: sound effect '%s' is silent (zero length)\n", in.c_str()); return false; }
    const std::string nm = name.empty() ? stem(in) : name;
    w.add_sound(nm, pcm.data(), uint32_t(pcm.size()), kSynthRate);
    std::printf("  + sound   %-12s %u frames @ %u Hz  (%s, sfx %s)\n",
                nm.c_str(), unsigned(pcm.size()), kSynthRate, in.c_str(), wave_name(p.wave));
    return true;
}

inline bool build_song(BundleWriter& w, const std::string& in, const std::string& name = "") {
    std::vector<uint8_t> bytes;
    if (!read_file(in, bytes)) { std::fprintf(stderr, "phx: cannot read '%s'\n", in.c_str()); return false; }
    Song song;
    std::string err;
    if (!song_from_json(std::string(bytes.begin(), bytes.end()), song, &err)) {
        std::fprintf(stderr, "phx: bad song '%s': %s\n", in.c_str(), err.c_str()); return false; }
    const std::vector<int16_t> pcm = render_song(song);
    if (pcm.empty()) { std::fprintf(stderr, "phx: song '%s' renders nothing (empty order list?)\n", in.c_str()); return false; }
    const std::string nm = name.empty() ? stem(in) : name;
    w.add_sound(nm, pcm.data(), uint32_t(pcm.size()), kSynthRate);
    std::printf("  + sound   %-12s %u frames @ %u Hz  (%s, song: %d rows, %.1f s)\n",
                nm.c_str(), unsigned(pcm.size()), kSynthRate, in.c_str(), song.total_rows(), song.seconds());
    return true;
}

// ---- visualization track: WAV -> per-video-frame .phxviz analysis blob ----
// Analyses the song offline (FFT bands / RMS / onset) at the target's audio device rate and
// stores the result as a generic Blob asset the ROM reads zero-copy (examples/miracle-player).
// device_rate defaults to the GBA vblank-locked rate so the baked stream lines up with the
// tier-0 resampled audio; pass the host mixer rate for a PC bundle.
inline bool build_viz(BundleWriter& w, const std::string& in, const std::string& name = "",
                      uint32_t device_rate = 18157) {
    std::vector<uint8_t> bytes;
    if (!read_file(in, bytes)) { std::fprintf(stderr, "phx: cannot read '%s'\n", in.c_str()); return false; }
    std::vector<int16_t> mono; uint32_t rate = 0;
    if (!wav_decode(bytes.data(), bytes.size(), mono, rate)) {
        std::fprintf(stderr, "phx: bad/unsupported WAV '%s'\n", in.c_str()); return false; }
    const std::string nm = name.empty() ? stem(in) : name;
    std::vector<uint8_t> blob = phxviz::analyze_pcm(mono.data(), uint32_t(mono.size()), rate, device_rate);
    const auto* h = reinterpret_cast<const miracle::VizHeader*>(blob.data());
    w.add_blob(nm, blob.data(), uint32_t(blob.size()));
    std::printf("  + viz     %-12s %u frames @ %u Hz  (%s)\n",
                nm.c_str(), unsigned(h->frame_count), device_rate, in.c_str());
    return true;
}

// ---- sprite sheets: .sprdef text OR a JSON sidecar ------------------------
// A transition as authored: clip NAMES ("*" = from any clip) and the trigger's name. Resolved to
// clip indices by sprite_transitions() at bake time, so a typo is a bake error, not a dead edge.
struct SprTrans { std::string from, to, trigger; };

struct SprDef {
    std::string sheet;                 // PNG path (resolved relative to the def's dir)
    int fw = 0, fh = 0;
    std::vector<phx::SpriteClipDef> clips;
    std::vector<std::string> clip_names;   // parallel to clips
    std::vector<SprTrans> trans;
};

// The baked transitions of `sd`, or false (with `err`) when an edge names a clip the sprite lacks.
inline bool sprite_transitions(const SprDef& sd, std::vector<phx::SpriteTransDef>& out, std::string* err = nullptr) {
    auto fail = [&](const std::string& why) { if (err) *err = why; return false; };
    auto clip_index = [&](const std::string& n) -> int {
        for (size_t i = 0; i < sd.clip_names.size(); ++i) if (sd.clip_names[i] == n) return int(i);
        return -1;
    };
    out.clear();
    for (const SprTrans& t : sd.trans) {
        if (t.trigger.empty()) return fail("transition " + t.from + " -> " + t.to + " has no trigger");
        const int to = clip_index(t.to);
        const int from = t.from == "*" ? int(phx::kSpriteTransAny) : clip_index(t.from);
        if (from < 0) return fail("transition from unknown clip '" + t.from + "'");
        if (to < 0) return fail("transition to unknown clip '" + t.to + "'");
        if (to >= int(phx::kSpriteTransAny) || (from != int(phx::kSpriteTransAny) && from >= int(phx::kSpriteTransAny)))
            return fail("transitions address at most 255 clips");
        phx::SpriteTransDef d{};
        d.trigger = phx::fnv1a(t.trigger.c_str());
        d.from = uint8_t(from);
        d.to   = uint8_t(to);
        out.push_back(d);
    }
    return true;
}

inline bool load_sprdef(const std::string& path, SprDef& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char line[1024];
    while (std::fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        char a[256], b[256], c3[256]; int v1, v2, v3, v4;
        if (std::sscanf(line, "sheet %255s %d %d", a, &v1, &v2) == 3) {
            out.sheet = a; out.fw = v1; out.fh = v2;
        } else if (std::sscanf(line, "clip %255s %d %d %d %d", b, &v1, &v2, &v3, &v4) == 5) {
            phx::SpriteClipDef c{};
            c.name  = phx::fnv1a(b);
            c.first = uint16_t(v1); c.count = uint16_t(v2);
            c.fps   = uint8_t(v3);  c.loop  = uint8_t(v4 ? 1 : 0);
            out.clips.push_back(c);
            out.clip_names.push_back(b);
        } else if (std::sscanf(line, "trans %255s %255s %255s", a, b, c3) == 3) {
            out.trans.push_back(SprTrans{ a, b, c3 });
        }
    }
    std::fclose(f);
    if (!out.sheet.empty() && out.sheet[0] != '/' && out.sheet.find('/') == std::string::npos)
        out.sheet = dir_of(path) + out.sheet;
    return !out.sheet.empty() && out.fw > 0 && out.fh > 0;
}

// JSON sidecar (docs/08 §3): { "image", "tile" | ["fw","fh"], "animations": { name: {frames,fps,loop} } }.
// `frames` is a list of frame indices; we bake it as a [first,count] run (contiguous), matching
// the SpriteClipDef model. Non-contiguous frame lists are rejected (caught offline).
inline bool load_sprjson(const std::string& path, SprDef& out, std::string* err = nullptr) {
    auto fail = [&](const std::string& why) { if (err) *err = why; return false; };
    std::vector<uint8_t> bytes;
    if (!read_file(path, bytes)) return fail("cannot read file");
    JsonValue root;
    std::string jerr;
    if (!JsonParser::parse(std::string(bytes.begin(), bytes.end()), root, &jerr))
        return fail("malformed JSON: " + jerr);
    if (!root.is_obj()) return fail("top level is not a JSON object");
    out.sheet = root.str_at("image");
    if (out.sheet.empty()) return fail("missing \"image\" (the sheet PNG path)");
    if (const JsonValue* t = root.find("tile")) { out.fw = out.fh = t->as_int(); }
    out.fw = root.int_at("frame_w", out.fw);
    out.fh = root.int_at("frame_h", out.fh);
    const JsonValue* anims = root.find("animations");
    if (anims && anims->is_obj()) {
        for (const auto& kv : anims->members) {
            const JsonValue& a = kv.second;
            const JsonValue* frames = a.find("frames");
            if (!frames || !frames->is_arr() || frames->arr.empty())
                return fail("animation '" + kv.first + "' needs a non-empty \"frames\" array");
            int first = frames->arr.front().as_int();
            for (size_t k = 0; k < frames->arr.size(); ++k)        // require a contiguous run
                if (frames->arr[k].as_int() != first + int(k))
                    return fail("animation '" + kv.first + "': \"frames\" must be a contiguous run "
                                "(e.g. [4,5,6]) — the baked clip model is [first,count]");
            phx::SpriteClipDef c{};
            c.name  = phx::fnv1a(kv.first.c_str());
            c.first = uint16_t(first);
            c.count = uint16_t(frames->arr.size());
            c.fps   = uint8_t(a.int_at("fps", 0));
            c.loop  = uint8_t(a.find("loop") && a.find("loop")->boolean ? 1 : 0);
            out.clips.push_back(c);
            out.clip_names.push_back(kv.first);
        }
    }
    // "transitions": [ { "from": "idle" | "*", "to": "walk", "on": "move" } ]
    if (const JsonValue* tr = root.find("transitions"); tr && tr->is_arr())
        for (const JsonValue& t : tr->arr) {
            const std::string from = t.str_at("from").empty() ? std::string("*") : t.str_at("from");
            out.trans.push_back(SprTrans{ from, t.str_at("to"), t.str_at("on") });
        }
    if (!out.sheet.empty() && out.sheet[0] != '/' && out.sheet.find('/') == std::string::npos)
        out.sheet = dir_of(path) + out.sheet;
    if (out.fw <= 0 || out.fh <= 0)
        return fail("missing/invalid frame size (\"tile\" or \"frame_w\"/\"frame_h\" must be > 0)");
    return true;
}

// Bakes the sheet texture + the Sprite metadata. The sprite asset is named `name` (default: the
// def's stem); the texture is named after the sheet PNG's stem (so several sprites can share it).
inline bool build_sprite(BundleWriter& w, const std::string& in, const std::string& name = "") {
    SprDef sd;
    std::string serr;
    const bool ok = ends_with(in, ".json") ? load_sprjson(in, sd, &serr) : load_sprdef(in, sd);
    if (!ok) {
        std::fprintf(stderr, "phx: bad sprite def '%s'%s%s\n", in.c_str(),
                     serr.empty() ? "" : ": ", serr.c_str());
        return false;
    }
    std::vector<uint8_t> bytes;
    if (!read_file(sd.sheet, bytes)) { std::fprintf(stderr, "phx: sprite def '%s' cannot read sheet '%s'\n", in.c_str(), sd.sheet.c_str()); return false; }
    std::vector<uint32_t> px; uint16_t iw, ih;
    if (!png_decode(bytes.data(), bytes.size(), px, iw, ih)) {
        std::fprintf(stderr, "phx: sprite def '%s' bad sheet PNG '%s'\n", in.c_str(), sd.sheet.c_str()); return false; }
    const std::string texname = stem(sd.sheet);
    const std::string nm = name.empty() ? stem(in) : name;
    w.add_texture(texname, px.data(), iw, ih);
    std::vector<phx::SpriteTransDef> trans;
    if (!sprite_transitions(sd, trans, &serr)) {
        std::fprintf(stderr, "phx: sprite def '%s': %s\n", in.c_str(), serr.c_str());
        return false;
    }
    const uint16_t cols = uint16_t(iw / uint16_t(sd.fw));
    w.add_sprite(nm, texname, uint16_t(sd.fw), uint16_t(sd.fh), cols, sd.clips, trans);
    std::printf("  + sprite  %-12s sheet '%s' %dx%d, %u clips", nm.c_str(), texname.c_str(), sd.fw, sd.fh,
                unsigned(sd.clips.size()));
    if (!trans.empty()) std::printf(", %u transitions", unsigned(trans.size()));
    std::printf("  (%s)\n", in.c_str());
    return true;
}

// Bakes the font's sheet (a Texture named after the PNG) + the Font asset (named `name`, default
// the def's stem): res->font("font"_hash) / phx::load_font.
inline bool build_font(BundleWriter& w, const std::string& in, const std::string& name = "") {
    std::vector<uint8_t> bytes;
    if (!read_file(in, bytes)) { std::fprintf(stderr, "phx: cannot read '%s'\n", in.c_str()); return false; }
    const std::string text(bytes.begin(), bytes.end());
    std::string err, image;
    FontDef fd;
    BmFont bm;
    const bool is_fnt = ends_with(in, ".fnt");
    if (is_fnt ? !load_bmfont(text, in, bm, &err) : !load_fontdef(text, in, fd, &err)) {
        std::fprintf(stderr, "phx: bad font '%s': %s\n", in.c_str(), err.c_str());
        return false;
    }
    image = font_image_path(in, is_fnt ? bm.page : fd.image);
    std::vector<uint8_t> png;
    if (!read_file(image, png)) { std::fprintf(stderr, "phx: font '%s' cannot read its sheet '%s'\n", in.c_str(), image.c_str()); return false; }
    std::vector<uint32_t> px; uint16_t iw, ih;
    if (!png_decode(png.data(), png.size(), px, iw, ih)) {
        std::fprintf(stderr, "phx: font '%s' bad sheet PNG '%s'\n", in.c_str(), image.c_str()); return false; }
    phx::FontBlobHeader hdr{};
    std::vector<phx::FontGlyphDef> glyphs;
    if (is_fnt ? !font_glyphs_from_bmfont(bm, hdr, glyphs, &err) : !font_glyphs_from_grid(fd, px, iw, ih, hdr, glyphs, &err)) {
        std::fprintf(stderr, "phx: font '%s': %s\n", in.c_str(), err.c_str());
        return false;
    }
    for (const phx::FontGlyphDef& g : glyphs)
        if (g.w && (g.sx + g.w > iw || g.sy + g.h > ih)) {
            std::fprintf(stderr, "phx: font '%s': a glyph lies outside the %ux%u sheet\n", in.c_str(), unsigned(iw), unsigned(ih));
            return false;
        }
    const std::string texname = stem(image);
    const std::string nm = name.empty() ? stem(in) : name;
    w.add_texture(texname, px.data(), iw, ih);
    hdr.texture = phx::fnv1a(texname.c_str());
    w.add_font(nm, hdr, glyphs);
    std::printf("  + font    %-12s sheet '%s', %u glyphs from '%c', %s, line %u  (%s)\n", nm.c_str(), texname.c_str(),
                unsigned(glyphs.size()), char(hdr.first_char), (hdr.flags & phx::kFontProportional) ? "proportional" : "fixed",
                unsigned(hdr.line_h), in.c_str());
    return true;
}

// ---- dialogue: a .dlg -> a Dialogue asset (conversations, choices, conditions) ----
inline bool build_dialogue(BundleWriter& w, const std::string& in, const std::string& name = "") {
    std::vector<uint8_t> bytes;
    if (!read_file(in, bytes)) { std::fprintf(stderr, "phx: cannot read '%s'\n", in.c_str()); return false; }
    DlgDoc doc;
    DlgCompiled c;
    std::string err;
    if (!dlg_from_json(std::string(bytes.begin(), bytes.end()), doc, &err) || !dlg_compile(doc, c, &err)) {
        std::fprintf(stderr, "phx: bad dialogue '%s': %s\n", in.c_str(), err.c_str());
        return false;
    }
    for (const DlgProblem& p : dlg_validate(doc))
        if (!p.error) std::fprintf(stderr, "phx: dialogue '%s': warning: %s\n", in.c_str(), p.what.c_str());
    const std::string nm = name.empty() ? stem(in) : name;
    w.add_dialogue(nm, c.blob());
    std::printf("  + dialog  %-12s %u conversations, %u lines, %u choices  (%s)\n", nm.c_str(), unsigned(c.convs.size()),
                unsigned(c.nodes.size()), unsigned(c.choices.size()), in.c_str());
    return true;
}

// ---- data tables: JSON -> flat binary + generated accessor header (phxbin) ----
// Schema: { "struct": "<Name>", "fields": [ {"name","type"} ... ], "records": [ {field: value} ] }.
// Field types: u8/i8/u16/i16/u32/i32/f32, plus str8/str16/str32/str64 — an inline NUL-terminated
// char[N] (values truncate to N-1). A string field names a record (a prefab's spawn type, an
// item's id string): the game hashes it (fnv1a) to match spawn types, and `phxtmap --prefabs`
// reads the same table as its placeable-entity vocabulary. Records are packed at natural C
// alignment so the generated POD struct matches the blob by construction (a static_assert in
// the header guards it).
struct BinField { std::string name, type; uint32_t size = 0, align = 0, offset = 0; };

// str8/str16/str32/str64 -> 8/16/32/64; 0 for every other type. (str64 holds a prefab's
// `components` list: "PlatformerController CameraFollow" is already 33 characters.)
inline uint32_t bin_str_size(const std::string& t) {
    if (t == "str8")  return 8;
    if (t == "str16") return 16;
    if (t == "str32") return 32;
    if (t == "str64") return 64;
    return 0;
}
inline bool bin_type_info(const std::string& t, uint32_t& size, uint32_t& align) {
    if (t == "u8"  || t == "i8")  { size = 1; align = 1; return true; }
    if (t == "u16" || t == "i16") { size = 2; align = 2; return true; }
    if (t == "u32" || t == "i32" || t == "f32") { size = 4; align = 4; return true; }
    if (const uint32_t n = bin_str_size(t)) { size = n; align = 1; return true; }
    return false;
}
inline const char* bin_ctype(const std::string& t) {
    if (t == "u8")  return "uint8_t";
    if (t == "i8")  return "int8_t";
    if (t == "u16") return "uint16_t";
    if (t == "i16") return "int16_t";
    if (t == "u32") return "uint32_t";
    if (t == "i32") return "int32_t";
    if (t == "f32") return "float";
    return "uint8_t";
}
inline void bin_write_field(std::vector<uint8_t>& rec, uint32_t off, const BinField& f, const JsonValue& v) {
    if (f.type == "f32") { float x = float(v.as_num()); std::memcpy(rec.data() + off, &x, 4); return; }
    if (bin_str_size(f.type)) {                       // char[N], always NUL-terminated
        const std::string& s = v.as_str();
        const uint32_t n = uint32_t(s.size()) < f.size - 1 ? uint32_t(s.size()) : f.size - 1;
        std::memcpy(rec.data() + off, s.data(), n);   // rec is zero-filled: rest stays NUL
        return;
    }
    int64_t n = int64_t(v.as_num());
    for (uint32_t b = 0; b < f.size; ++b) rec[off + b] = uint8_t((n >> (8 * b)) & 0xFF);  // little-endian
}

// Builds the table blob (named `name`) and, if `header_out` is non-empty, writes the .gen.h.
inline bool build_bin(BundleWriter& w, const std::string& in, const std::string& name = "",
                      const std::string& header_out = "") {
    std::vector<uint8_t> bytes;
    if (!read_file(in, bytes)) { std::fprintf(stderr, "phx: cannot read '%s'\n", in.c_str()); return false; }
    JsonValue root;
    std::string jerr;
    if (!JsonParser::parse(std::string(bytes.begin(), bytes.end()), root, &jerr)) {
        std::fprintf(stderr, "phx: malformed JSON '%s': %s\n", in.c_str(), jerr.c_str()); return false; }
    if (!root.is_obj()) {
        std::fprintf(stderr, "phx: '%s': top level is not a JSON object\n", in.c_str()); return false; }
    const std::string sname = root.str_at("struct");
    const JsonValue* fields = root.find("fields");
    const JsonValue* recs   = root.find("records");
    if (sname.empty() || !fields || !fields->is_arr() || fields->arr.empty()) {
        std::fprintf(stderr, "phx: '%s' needs \"struct\" + non-empty \"fields\"\n", in.c_str()); return false; }

    std::vector<BinField> fs;
    uint32_t off = 0, maxa = 1;
    for (const JsonValue& fv : fields->arr) {
        BinField f; f.name = fv.str_at("name"); f.type = fv.str_at("type");
        if (f.name.empty() || !bin_type_info(f.type, f.size, f.align)) {
            std::fprintf(stderr, "phx: '%s' bad field (name/type)\n", in.c_str()); return false; }
        off = (off + f.align - 1) & ~(f.align - 1);   // natural alignment
        f.offset = off; off += f.size;
        if (f.align > maxa) maxa = f.align;
        fs.push_back(f);
    }
    const uint32_t stride = (off + maxa - 1) & ~(maxa - 1);   // pad struct to its alignment

    const uint32_t count = (recs && recs->is_arr()) ? uint32_t(recs->arr.size()) : 0;
    std::vector<uint8_t> blob(8 + size_t(count) * stride, 0);
    std::memcpy(blob.data() + 0, &count, 4);
    std::memcpy(blob.data() + 4, &stride, 4);
    for (uint32_t r = 0; r < count; ++r) {
        std::vector<uint8_t> rec(stride, 0);
        const JsonValue& rv = recs->arr[r];
        for (const BinField& f : fs) if (const JsonValue* v = rv.find(f.name.c_str())) bin_write_field(rec, f.offset, f, *v);
        std::memcpy(blob.data() + 8 + size_t(r) * stride, rec.data(), stride);
    }
    // The schema trailer (phx/resource/bundle.h): padded to 4 bytes, then magic, field count and
    // one TableFieldDef per column, so the engine can read columns by name (TableView).
    blob.resize((blob.size() + 3) & ~size_t(3), 0);
    auto put32 = [&](uint32_t v) { const size_t at = blob.size(); blob.resize(at + 4); std::memcpy(blob.data() + at, &v, 4); };
    put32(phx::kTableSchemaMagic);
    put32(uint32_t(fs.size()));
    for (const BinField& f : fs) {
        phx::TableFieldDef d{};
        d.name   = phx::fnv1a(f.name.c_str());
        d.offset = uint16_t(f.offset);
        d.size   = uint8_t(f.size);
        d.type   = f.type == "u8"  ? phx::kFieldU8  : f.type == "i8"  ? phx::kFieldI8  :
                   f.type == "u16" ? phx::kFieldU16 : f.type == "i16" ? phx::kFieldI16 :
                   f.type == "u32" ? phx::kFieldU32 : f.type == "i32" ? phx::kFieldI32 :
                   f.type == "f32" ? phx::kFieldF32 : phx::kFieldStr;
        const size_t at = blob.size();
        blob.resize(at + sizeof(d));
        std::memcpy(blob.data() + at, &d, sizeof(d));
    }
    const std::string nm = name.empty() ? stem(in) : name;
    w.add_blob(nm, blob.data(), uint32_t(blob.size()));
    std::printf("  + table   %-12s %u x %s (%u-byte stride)  (%s)\n",
                nm.c_str(), count, sname.c_str(), stride, in.c_str());

    if (!header_out.empty()) {
        FILE* h = std::fopen(header_out.c_str(), "wb");
        if (!h) { std::fprintf(stderr, "phx: cannot write header '%s'\n", header_out.c_str()); return false; }
        std::fprintf(h, "// generated by phxbin from %s — do not edit.\n#pragma once\n#include <cstdint>\n\n", in.c_str());
        std::fprintf(h, "namespace phxbin {\n\nstruct %s {\n", sname.c_str());
        for (const BinField& f : fs) {
            if (const uint32_t n = bin_str_size(f.type))
                std::fprintf(h, "    %-9s %s[%u];   // NUL-terminated\n", "char", f.name.c_str(), n);
            else
                std::fprintf(h, "    %-9s %s;\n", bin_ctype(f.type), f.name.c_str());
        }
        std::fprintf(h, "};\nstatic_assert(sizeof(%s) == %u, \"%s stride drifted from the baked blob\");\n\n",
                     sname.c_str(), stride, sname.c_str());
        std::fprintf(h, "// Blob layout: [uint32 count][uint32 stride][%s records[count]].\n", sname.c_str());
        std::fprintf(h, "struct %sTable {\n    uint32_t count;\n    uint32_t stride;\n    %s records[];\n};\n\n",
                     sname.c_str(), sname.c_str());
        std::fprintf(h, "} // namespace phxbin\n");
        std::fclose(h);
        std::printf("  + header  %s\n", header_out.c_str());
    }
    return true;
}

// ---- dispatch a source file by extension (used by the phxpack assembler) ---
inline bool build_from_source(BundleWriter& w, const std::string& in) {
    if (ends_with(in, ".png"))    return build_png(w, in);
    if (ends_with(in, ".ppm"))    return build_ppm(w, in);
    if (ends_with(in, ".tmcsv"))  return build_tmcsv(w, in);
    if (ends_with(in, ".tmj"))    return build_tmj(w, in);
    if (ends_with(in, ".wav"))    return build_wav(w, in);
    if (ends_with(in, ".sfx"))    return build_sfx(w, in);
    if (ends_with(in, ".song"))   return build_song(w, in);
    if (ends_with(in, ".sprdef")) return build_sprite(w, in);
    if (ends_with(in, ".font") || ends_with(in, ".fnt")) return build_font(w, in);
    if (ends_with(in, ".dlg"))    return build_dialogue(w, in);
    std::fprintf(stderr, "phx: unknown source type '%s'\n", in.c_str());
    return false;
}

} // namespace phxtool
#endif // PHX_TOOLS_BUILDERS_H
