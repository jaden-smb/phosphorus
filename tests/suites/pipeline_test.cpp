// tests/pipeline_test.cpp — the two-stage asset pipeline end to end (docs/08): the per-format
// CONVERTERS (phxsprite/phxtile/phxsnd/phxbin) bake author sources into intermediate `.phx*`
// files, then the phxpack ASSEMBLER MERGES those intermediates into one `assets.phxp`, which the
// runtime ResourceCache mounts and reads back. Exercises the real converter builders + the
// intermediate write/read(merge) paths in-process, then verifies every asset type from the
// assembled bundle. It also drops the source fixtures + runs the .gen.h emit so the CLI smoke
// (`make tools` / ctest `tools_cli`) can re-run the actual tool binaries over them.
#include "phx/platform/platform.h"
#include "phx/resource/cache.h"
#include "phx/resource/table.h"
#include "phx/core/caps.h"

#include "builders.h"        // the converter bake logic
#include "bundle_reader.h"   // the assembler's merge logic
#include "editor.h"                        // phxtmap's document model (load/edit/save .tmj)
#include "../../tools/phxentity/editor.h"     // phxentity's document model (phxbin JSON tables)
#include "../../tools/phxstudio/model.h"      // Phosphorus Studio's headless model
#include "../../tools/phxstudio/budget.h"     // ... and its Budget view

#include "fixtures/png_fixtures.h"
#include "ascii_font.h"                  // tools/common: the font sheet the font tests bake
#include "png_write.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace phx;

namespace {
int g_checks = 0, g_fail = 0;
void check(bool ok, const char* what) { ++g_checks; if (!ok) { ++g_fail; std::printf("    FAIL %s\n", what); } }

void write_file(const char* path, const void* data, size_t n) {
    if (FILE* f = std::fopen(path, "wb")) { std::fwrite(data, 1, n, f); std::fclose(f); }
}

// Minimal 16-bit mono WAV (RIFF), same layout as audio_test's.
std::vector<uint8_t> make_wav_mono16(uint32_t rate, const int16_t* s, uint32_t frames) {
    auto put16 = [](std::vector<uint8_t>& v, uint16_t x){ v.push_back(uint8_t(x)); v.push_back(uint8_t(x>>8)); };
    auto put32 = [](std::vector<uint8_t>& v, uint32_t x){ for (int i=0;i<4;++i) v.push_back(uint8_t(x>>(8*i))); };
    auto tag   = [](std::vector<uint8_t>& v, const char* t){ for (int i=0;i<4;++i) v.push_back(uint8_t(t[i])); };
    std::vector<uint8_t> w;
    const uint32_t dataSize = frames * 2;
    tag(w,"RIFF"); put32(w,36+dataSize); tag(w,"WAVE");
    tag(w,"fmt "); put32(w,16); put16(w,1); put16(w,1); put32(w,rate); put32(w,rate*2); put16(w,2); put16(w,16);
    tag(w,"data"); put32(w,dataSize);
    for (uint32_t i = 0; i < frames; ++i) { w.push_back(uint8_t(s[i])); w.push_back(uint8_t(s[i]>>8)); }
    return w;
}

// A small Tiled map (one tile layer + one object group), like tiled_test's.
const char* kTmj =
"{ \"width\":3, \"height\":2, \"tilewidth\":8, \"tileheight\":8,"
"  \"tilesets\":[ { \"firstgid\":1, \"name\":\"tiles\" } ],"
"  \"layers\":["
"    { \"type\":\"tilelayer\", \"name\":\"ground\", \"width\":3, \"height\":2, \"data\":[1,2,1,2,1,2] },"
"    { \"type\":\"objectgroup\", \"name\":\"things\", \"objects\":["
"       { \"name\":\"p\", \"type\":\"player\", \"x\":8,  \"y\":16, \"width\":8, \"height\":8 },"
"       { \"name\":\"c\", \"type\":\"coin\",   \"x\":16, \"y\":8,  \"width\":8, \"height\":8 } ] } ] }";

const char* kSprdef = "sheet p_sheet.png 2 2\nclip walk 0 4 8 1\nclip idle 0 1 0 0\n"
                      "trans idle walk move\ntrans * idle stop\n";

const char* kItems =
"{ \"struct\":\"ItemRecord\","
"  \"fields\":[ {\"name\":\"id\",\"type\":\"u16\"}, {\"name\":\"price\",\"type\":\"u32\"}, {\"name\":\"atk\",\"type\":\"i16\"} ],"
"  \"records\":[ {\"id\":1,\"price\":100,\"atk\":5}, {\"id\":2,\"price\":250,\"atk\":-3} ] }";

// Assemble (== phxpack's merge): read each intermediate bundle and re-add its assets by hash.
void merge(phxtool::BundleWriter& out, const char* path) {
    std::vector<phxtool::ReadAsset> assets;
    check(phxtool::bundle_read(path, assets), "read intermediate bundle");
    for (auto& a : assets) out.add_raw(a.hash, a.type, std::move(a.blob));
}
} // namespace

int main() {
    // --- 1. drop author-source fixtures to disk -----------------------------
    write_file("build/p_sheet.png", kSheet8x2, sizeof(kSheet8x2));
    write_file("build/p_hero.sprdef", kSprdef, std::strlen(kSprdef));
    write_file("build/p_level.tmj", kTmj, std::strlen(kTmj));
    const int16_t tone[8] = { 0, 1000, 2000, 3000, -3000, -2000, -1000, 0 };
    std::vector<uint8_t> wav = make_wav_mono16(22050, tone, 8);
    write_file("build/p_tone.wav", wav.data(), wav.size());
    write_file("build/p_items.json", kItems, std::strlen(kItems));

    // --- 2. CONVERTERS: each source -> a one-asset(-group) intermediate bundle ----
    { phxtool::BundleWriter w(2); check(phxtool::build_sprite(w, "build/p_hero.sprdef", "hero"), "phxsprite build");
      check(w.write("build/p_hero.phxspr"), "write .phxspr"); }
    { phxtool::BundleWriter w(2); check(phxtool::build_tmj(w, "build/p_level.tmj", "level"), "phxtile build");
      check(w.write("build/p_level.phxtmap"), "write .phxtmap"); }
    { phxtool::BundleWriter w(2); check(phxtool::build_wav(w, "build/p_tone.wav", "tone"), "phxsnd build");
      check(w.write("build/p_tone.phxsnd"), "write .phxsnd"); }
    { phxtool::BundleWriter w(2); check(phxtool::build_bin(w, "build/p_items.json", "items", "build/p_items.gen.h"), "phxbin build");
      check(w.write("build/p_items.phxbin"), "write .phxbin"); }

    // --- 2b. bake determinism (docs/08 §9): identical input -> byte-identical bundle ----
    {
        phxtool::BundleWriter w2(2);
        check(phxtool::build_sprite(w2, "build/p_hero.sprdef", "hero"), "re-bake hero");
        check(w2.write("build/p_hero2.phxspr"), "write re-baked .phxspr");
        std::vector<uint8_t> a, b;
        check(phxtool::read_file("build/p_hero.phxspr", a)
              && phxtool::read_file("build/p_hero2.phxspr", b) && a == b,
              "two bakes of the same source are byte-identical (reproducible)");
    }

    // --- 3. ASSEMBLER: merge the intermediates into one bundle (phxpack) -----
    const char* bundle = "build/p_assets.phxp";
    { phxtool::BundleWriter out(2);
      merge(out, "build/p_hero.phxspr");
      merge(out, "build/p_level.phxtmap");
      merge(out, "build/p_tone.phxsnd");
      merge(out, "build/p_items.phxbin");
      check(out.write(bundle), "assemble merged bundle"); }

    // --- 4. mount the assembled bundle and verify every asset type ----------
    const phx_platform* plat = phx_platform_get();
    phx_platform_desc desc{}; desc.title = "pipeline_test"; desc.width = 16; desc.height = 16;
    if (plat->init(&desc) != 0) { std::printf("platform init failed\n"); return 1; }

    static uint8_t arena_buf[8 << 20];
    ArenaAllocator arena; arena.init(arena_buf, sizeof(arena_buf));
    ResourceCache* cache = ResourceCache::create(arena).unwrap();
    check(cache->mount(plat, bundle) == Status::Ok, "mount assembled bundle");

    // sprite (+ its sheet texture), from phxsprite
    auto tv = cache->texture("p_sheet"_hash);
    check(tv.ok() && tv.unwrap().width == 8 && tv.unwrap().height == 2, "sheet texture merged from .phxspr");
    auto spr = cache->sprite("hero"_hash);
    check(spr.ok(), "sprite('hero') merged");
    if (spr.ok()) { SpriteView s = spr.unwrap();
        check(s.texture == "p_sheet"_hash && s.frame_w == 2 && s.frame_h == 2 && s.cols == 4, "sprite frame grid");
        check(s.clip_count == 2 && s.clips && s.clips[0].name == "walk"_hash && s.clips[0].count == 4, "sprite clips");
        check(s.trans_count == 2 && s.trans && s.trans[0].from == 1 && s.trans[0].to == 0 &&
              s.trans[0].trigger == "move"_hash && s.trans[1].from == kSpriteTransAny && s.trans[1].to == 1 &&
              s.trans[1].trigger == "stop"_hash, "sprite transitions: clip names resolved to indices, '*' = any"); }

    // tilemap (+ spawns), from phxtile
    auto mv = cache->tilemap("level"_hash);
    check(mv.ok(), "tilemap('level') merged");
    if (mv.ok()) { TilemapView m = mv.unwrap();
        check(m.width == 3 && m.height == 2 && m.tileset == "tiles"_hash, "tilemap meta merged from .phxtmap");
        check(m.indices && m.indices[0] == 1 && m.indices[1] == 2 && m.indices[5] == 2, "tilemap indices"); }
    auto sp = cache->spawns("level"_hash);
    check(sp.ok() && sp.unwrap().count == 2, "spawns('level') merged");
    if (sp.ok()) { SpawnsView s = sp.unwrap();
        check(s.spawns[0].type == "player"_hash && s.spawns[0].x == 8 && s.spawns[1].type == "coin"_hash, "spawns resolve"); }

    // sound, from phxsnd
    auto snd = cache->sound("tone"_hash);
    check(snd.ok(), "sound('tone') merged");
    if (snd.ok()) { SoundDataView s = snd.unwrap();
        check(s.frames == 8 && s.rate == 22050, "sound meta from .phxsnd");
        check(s.samples && s.samples[1] == 1000 && s.samples[4] == -3000, "sound samples"); }

    // per-target encode: the SAME 22050 Hz WAV baked for tier 0 (GBA) is resampled at bake
    // time to the 18157 Hz device rate (fewer ROM bytes, 1:1 runtime mixing); tier 2 kept
    // the source rate above. First sample must survive; frame count scales by 18157/22050.
    {
        phxtool::BundleWriter w0(0);
        check(phxtool::build_wav(w0, "build/p_tone.wav", "tone"), "phxsnd build (tier 0)");
        check(w0.write("build/p_tone0.phxp"), "write tier-0 bundle");
        ResourceCache* c0 = ResourceCache::create(arena).unwrap();
        check(c0->mount(plat, "build/p_tone0.phxp") == Status::Ok, "mount tier-0 bundle");
        auto s0 = c0->sound("tone"_hash);
        check(s0.ok(), "sound('tone') tier 0");
        if (s0.ok()) { SoundDataView s = s0.unwrap();
            check(s.rate == 18157, "tier-0 sound resampled to the GBA device rate");
            check(s.frames == uint32_t((uint64_t(8) << 16) / ((uint64_t(22050) << 16) / 18157)),
                  "tier-0 frame count scaled by 18157/22050");
            check(s.samples && s.samples[0] == 0, "tier-0 first sample intact"); }
    }

    // per-target TEXTURE encode (docs/06 §4, the texture counterpart of the sound check
    // above): the same RGBA8 art bakes to PAL4_TILES on tier 0, swizzled RGBA8 on tier 1,
    // plain RGBA8 on tier 2 — and art a tier can't express honestly stays RGBA8.
    {
        static uint32_t art[8 * 8];
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x)
                art[y * 8 + x] = (x < 4) ? rgba(216, 40, 40) : rgba(40, 216, 40);
        art[9] = 0;                                    // one transparent texel at (1,1)

        for (int tier = 0; tier <= 2; ++tier) {
            char path[64]; std::snprintf(path, sizeof(path), "build/p_tex%d.phxp", tier);
            phxtool::BundleWriter wt{uint8_t(tier)};
            wt.add_texture("art", art, 8, 8);
            if (tier == 0) wt.add_texture("odd", art, 8, 2);   // h % 8 -> not tier-0 encodable
            check(wt.write(path), "write per-tier texture bundle");

            ResourceCache* ct = ResourceCache::create(arena).unwrap();
            check(ct->mount(plat, path) == Status::Ok, "mount per-tier texture bundle");
            auto tvr = ct->texture("art"_hash);
            check(tvr.ok(), "texture('art') per tier");
            if (!tvr.ok()) continue;
            TextureView v = tvr.unwrap();
            const uint8_t* pay = static_cast<const uint8_t*>(v.pixels);
            if (tier == 0) {
                check(v.format == PixelFormat::PAL4_TILES, "tier 0 baked PAL4_TILES");
                const TexturePal4Header* ph = reinterpret_cast<const TexturePal4Header*>(pay);
                check(ph->pal_count == 1, "2-colour art fits one palette (OBJ-safe)");
                const uint16_t* pal   = reinterpret_cast<const uint16_t*>(pay + pal4_palettes_off());
                const uint8_t*  tiles = pay + pal4_tile_data_off(ph->pal_count, 1);
                check(pal4_texel(tiles, 0, 1, 1) == 0, "transparent texel -> nibble 0");
                const uint8_t s00 = pal4_texel(tiles, 0, 0, 0);
                check(s00 != 0 && pal[s00] == rgba8_to_bgr555(rgba(216, 40, 40)),
                      "PAL4 texel (0,0) decodes to the quantized source colour");
                // the fallback texture kept RGBA8 (and the runtime treats it like a v1 blob)
                auto odd = ct->texture("odd"_hash);
                check(odd.ok() && odd.unwrap().format == PixelFormat::RGBA8,
                      "non-8px-aligned art honestly stays RGBA8 on tier 0");
            } else if (tier == 1) {
                check(v.format == PixelFormat::RGBA8_SWZ, "tier 1 baked swizzled RGBA8");
                const uint32_t* px = reinterpret_cast<const uint32_t*>(pay);
                check(px[swz_texel_index(5, 3, 8)] == art[3 * 8 + 5],
                      "swizzled texel reads back bit-exact");
            } else {
                check(v.format == PixelFormat::RGBA8, "tier 2 keeps plain RGBA8");
                check(std::memcmp(pay, art, sizeof(art)) == 0, "tier-2 texels untouched");
            }
        }
    }

    // data table, from phxbin: [u32 count][u32 stride][records...], ItemRecord{u16 id; u32 price; i16 atk;}
    auto bl = cache->blob("items"_hash);
    check(bl.ok(), "blob('items') merged");
    if (bl.ok()) { BlobView b = bl.unwrap();
        const uint8_t* p = static_cast<const uint8_t*>(b.data);
        uint32_t count = 0, stride = 0;
        std::memcpy(&count, p + 0, 4); std::memcpy(&stride, p + 4, 4);
        // ItemRecord natural C layout: id u16 @0, price u32 @4 (aligned), atk i16 @8 -> pad to 12.
        check(count == 2 && stride == 12, "table count/stride matches natural C layout");
        uint16_t id0 = 0; uint32_t price0 = 0; int16_t atk1 = 0;
        std::memcpy(&id0,    p + 8 + 0, 2);
        std::memcpy(&price0, p + 8 + 4, 4);
        std::memcpy(&atk1,   p + 8 + stride + 8, 2);
        check(id0 == 1 && price0 == 100 && atk1 == -3, "table record fields");
        // the schema trailer: the same table read by COLUMN NAME (phx/resource/table.h)
        TableView t;
        check(t.parse(b) && t.has_schema() && t.count() == 2 && t.stride() == 12, "table schema trailer parses");
        check(t.get_int(0, "price"_hash) == 100 && t.get_int(1, "atk"_hash) == -3 && t.get_int(1, "id"_hash) == 2,
              "table columns read by name");
        check(t.get_int(0, "nope"_hash, 7) == 7 && t.get_int(5, "id"_hash, -1) == -1 && !t.has("nope"_hash),
              "absent column / row -> the default"); }

    // phxtmap editor document model: blank -> paint tiles + place spawns + a parallax layer
    // -> save .tmj -> the REAL importer parses it back identically (editors emit author
    // formats the converters bake, docs/08 §1) -> erase round-trips too.
    {
        using phxtool::TmapDoc;
        TmapDoc d = TmapDoc::blank(4, 3, 8, 8, "tiles");
        d.layers.insert(d.layers.begin(), std::vector<uint16_t>(12, 0));   // backdrop under main
        d.layer_names.insert(d.layer_names.begin(), "backdrop");
        d.layer_parallax.insert(d.layer_parallax.begin(), { 0.5, 1.0 });
        d.set_tile(0, 1, 0, 7);                    // a cloud on the backdrop
        d.set_tile(1, 2, 2, 3);                    // ground on the main layer
        d.add_spawn("player", 8, 16);
        d.add_spawn("coin", 24, 16);
        check(d.tile(1, 2, 2) == 3 && d.tile(0, 1, 0) == 7, "editor set/get tiles");
        check(d.dirty, "edits mark the doc dirty");

        TmapDoc r;
        check(TmapDoc::load(d.save_tmj(), r), "editor .tmj re-parses via the real importer");
        check(r.width == 4 && r.height == 3 && r.layers.size() == 2, "round-trip dimensions/layers");
        check(r.tile(0, 1, 0) == 7 && r.tile(1, 2, 2) == 3, "round-trip painted tiles");
        check(r.layer_parallax.size() == 2 && r.layer_parallax[0].first == 0.5 &&
              r.layer_parallax[1].first == 1.0, "round-trip parallax factors");
        check(r.spawns.size() == 2 && r.spawns[0].type == "player" && r.spawns[0].x == 8 &&
              r.spawns[1].type == "coin" && r.spawns[1].x == 24, "round-trip spawns");

        check(r.remove_spawn_at(25, 17), "erase the spawn under the pointer's cell");
        check(r.spawns.size() == 1 && r.spawns[0].type == "player", "right spawn removed");
        r.set_tile(1, 2, 2, 0);
        TmapDoc r2;
        check(TmapDoc::load(r.save_tmj(), r2) && r2.tile(1, 2, 2) == 0 && r2.spawns.size() == 1,
              "erase edits survive a second round-trip");

        // Layer names authored in the doc survive the importer (not synthesized "layerN").
        check(r2.layer_names.size() == 2 && r2.layer_names[0] == "backdrop" &&
              r2.layer_names[1] == "main", "round-trip layer names");

        // The doc's spawn vocabulary is harvested for the GUI's placeable list.
        auto tys = r2.spawn_types();
        check(tys.size() == 1 && tys[0] == "player", "spawn_types harvests the doc's vocabulary");
    }

    // Per-spawn properties: the map editor's model -> .tmj (Tiled custom properties) -> the real
    // importer -> the bake's spawn extension -> SpawnsView by name + key.
    {
        using phxtool::TmapDoc;
        TmapDoc d = TmapDoc::blank(4, 2, 8, 8, "tiles");
        d.add_spawn("door", 8, 0);
        d.add_spawn("enemy", 16, 8);
        d.add_spawn("coin", 24, 8);
        d.spawns[0].name = "door_a";
        d.spawns[2].name = "";                                        // an unnamed spawn
        d.set_prop(0, "target", "string", "level2 \"b\"");            // quotes survive the JSON
        d.set_prop(1, "range", "int", "-24");
        d.set_prop(1, "speed", "float", "1.5");
        d.set_prop(1, "angry", "bool", "yes");                        // normalised to true
        d.set_prop(1, "range", "int", "32");                          // replaces, not appends
        d.set_prop(1, "tmp", "int", "abc");                           // not a number -> 0
        d.remove_prop(1, "tmp");
        check(d.spawns[1].props.size() == 3 && d.spawns[1].props[0].value == "32" && d.spawns[1].props[2].value == "true",
              "spawn props: set replaces by name, values normalise to their type, remove drops");
        check(TmapDoc::normalise_prop("int", "-") == "0" && TmapDoc::normalise_prop("float", "2") == "2" &&
              TmapDoc::normalise_prop("bool", "0") == "false", "prop values normalise per type");
        TmapDoc r;
        check(TmapDoc::load(d.save_tmj(), r) && r.spawns.size() == 3 && r.spawns[0].props.size() == 1 &&
              r.spawns[0].props[0].value == "level2 \"b\"" && r.spawns[1].props == d.spawns[1].props,
              "spawn props round-trip through the .tmj and the real importer");
        check(d.save_file("build/p_props.tmj"), "write the props map");
        phxtool::BundleWriter w(2);
        check(phxtool::build_tmj(w, "build/p_props.tmj", "props") && w.write("build/p_props.phxp"), "bake the props map");
        ResourceCache* cp = ResourceCache::create(arena).unwrap();
        check(cp->mount(plat, "build/p_props.phxp") == Status::Ok, "mount the props map");
        auto sp = cp->spawns("props"_hash);
        check(sp.ok() && sp.unwrap().count == 3 && sp.unwrap().prop_count == 4, "the spawn extension is baked");
        if (sp.ok()) {
            const SpawnsView v = sp.unwrap();
            check(v.name(0) == "door_a"_hash && v.name(2) == 0 && v.find_named("door_a"_hash) == 0 && v.find_named("x"_hash) == -1,
                  "spawn names by index; an unnamed spawn is 0");
            check(v.get_str(0, "target"_hash) && std::strcmp(v.get_str(0, "target"_hash), "level2 \"b\"") == 0 &&
                  v.get_hash(0, "target"_hash) == phx::fnv1a("level2 \"b\""), "a string property");
            check(v.get_int(1, "range"_hash) == 32 && v.get_int(1, "speed"_hash) == 1 && v.get_int(1, "angry"_hash) == 1,
                  "int, float (truncated) and bool properties");
            check(v.get_int(1, "nope"_hash, 7) == 7 && v.get_int(0, "range"_hash, 7) == 7 && v.get_str(1, "range"_hash) == nullptr,
                  "absent keys / other spawns' keys / wrong type -> the default");
        }
        // a map whose spawns have no names and no properties bakes exactly as before (no extension)
        TmapDoc plain = TmapDoc::blank(2, 2, 8, 8, "tiles");
        plain.add_spawn("coin", 0, 0);
        plain.spawns[0].name = "";
        phxtool::BundleWriter wp(2);
        check(plain.save_file("build/p_plain.tmj") && phxtool::build_tmj(wp, "build/p_plain.tmj", "plain") && wp.write("build/p_plain.phxp"),
              "bake a map with bare spawns");
        check(cp->mount(plat, "build/p_plain.phxp") == Status::Ok && cp->spawns("plain"_hash).ok() &&
              cp->spawns("plain"_hash).unwrap().names == nullptr, "bare spawns carry no extension");
    }

    // phxtmap editor: per-GID collision flags author-edit + round-trip (saved as Tiled
    // tileset per-tile properties, re-imported by the same tiled_load the bake uses), and
    // bounded undo/redo over paint/spawn/flag/layer gestures.
    {
        using phxtool::TmapDoc;
        TmapDoc d = TmapDoc::blank(4, 3, 8, 8, "tiles");

        // collision authoring: cycle none -> solid -> oneway -> hazard -> none
        d.cycle_tile_flag(3);
        check(d.tile_flag(3) == phx::kTileFlagSolid, "cycle: none -> solid");
        d.cycle_tile_flag(3);
        check(d.tile_flag(3) == phx::kTileFlagOneWay, "cycle: solid -> oneway");
        d.cycle_tile_flag(3);
        check(d.tile_flag(3) == phx::kTileFlagHazard, "cycle: oneway -> hazard");
        d.set_tile_flag(2, phx::kTileFlagSolid);
        d.set_tile_flag(0, phx::kTileFlagSolid);            // GID 0 is air: must be refused
        check(d.tile_flag(0) == 0, "GID 0 never takes a flag");

        TmapDoc fr;
        check(TmapDoc::load(d.save_tmj(), fr), "flagged .tmj re-parses via the real importer");
        check(fr.tile_flag(2) == phx::kTileFlagSolid && fr.tile_flag(3) == phx::kTileFlagHazard,
              "collision flags survive save -> import (tileset per-tile properties)");

        // undo/redo: each gesture is one step; redo is cleared by a new edit
        TmapDoc u = TmapDoc::blank(4, 3, 8, 8, "tiles");
        check(!u.undo() && !u.redo(), "nothing to undo/redo on a fresh doc");
        u.push_undo(); u.set_tile(0, 1, 1, 5);              // gesture 1: paint
        u.push_undo(); u.add_spawn("coin", 8, 8);           // gesture 2: spawn
        u.push_undo(); u.add_layer("fg");                   // gesture 3: layer
        check(u.layers.size() == 2 && u.spawns.size() == 1 && u.tile(0, 1, 1) == 5, "edits applied");
        check(u.undo() && u.layers.size() == 1, "undo pops the layer");
        check(u.undo() && u.spawns.empty(), "undo pops the spawn");
        check(u.undo() && u.tile(0, 1, 1) == 0, "undo pops the paint");
        check(!u.undo(), "undo stack exhausted");
        check(u.redo() && u.tile(0, 1, 1) == 5, "redo re-applies the paint");
        check(u.redo() && u.spawns.size() == 1, "redo re-applies the spawn");
        u.push_undo(); u.set_tile(0, 0, 0, 9);              // a new edit...
        check(!u.redo(), "...clears the redo stack");
        // a no-op gesture can be dropped so undo never "does nothing"
        u.push_undo();
        const size_t depth = u.undo_depth();
        u.drop_undo();
        check(u.undo_depth() == depth - 1, "drop_undo discards the no-op gesture");
    }

    // phxtmap editor: the area paint tools (fill/rect; the picker is tile() + GUI wiring).
    // Flood fill is 4-connected and bounded by different GIDs; rect fill normalizes its
    // corners and clamps to the map; both report how many cells actually changed so a no-op
    // click can drop its undo step.
    {
        using phxtool::TmapDoc;
        TmapDoc d = TmapDoc::blank(6, 4, 8, 8, "tiles");
        // a vertical wall of GID 2 at x=3 splits the empty map into two regions
        for (int y = 0; y < 4; ++y) d.set_tile(0, 3, y, 2);
        check(d.flood_fill(0, 0, 0, 5) == 12, "flood fill floods the left region only (3x4)");
        check(d.tile(0, 2, 3) == 5 && d.tile(0, 4, 0) == 0 && d.tile(0, 3, 1) == 2,
              "fill stops at the wall; the far side is untouched");
        check(d.flood_fill(0, 0, 0, 5) == 0, "re-filling with the same GID is a no-op (0 changed)");
        check(d.flood_fill(0, -1, 0, 5) == 0 && d.flood_fill(9, 0, 0, 5) == 0,
              "out-of-bounds / bad layer fill is refused");
        check(d.flood_fill(0, 3, 1, 7) == 4, "filling the wall itself follows its 4 connected cells");

        TmapDoc r = TmapDoc::blank(6, 4, 8, 8, "tiles");
        check(r.fill_rect(0, 4, 2, 1, 1, 9) == 8, "rect fill normalizes swapped corners (4x2)");
        check(r.tile(0, 1, 1) == 9 && r.tile(0, 4, 2) == 9 && r.tile(0, 0, 0) == 0,
              "rect fill covers exactly the marquee");
        check(r.fill_rect(0, 1, 1, 4, 2, 9) == 0, "repainting the same rect changes nothing");
        check(r.fill_rect(0, -3, -3, 99, 0, 4) == 6, "rect fill clamps to the map (top row)");
        check(r.fill_rect(2, 0, 0, 1, 1, 4) == 0, "rect fill on a missing layer is refused");
        r.dirty = false;
        check(r.fill_rect(0, 0, 0, 0, 0, 4) == 0 && !r.dirty,
              "a no-change rect fill leaves the doc clean");

        // the fill round-trips through the real importer like any other edit
        TmapDoc rr;
        check(TmapDoc::load(r.save_tmj(), rr) && rr.tile(0, 3, 0) == 4 && rr.tile(0, 2, 1) == 9,
              "area-tool edits survive save -> import");
    }

    // phxentity editor document model: load the items table, edit (clamped to the field's
    // declared type), clone + delete records, save — and prove the saved JSON still BAKES
    // through the real phxbin builder (editors emit author formats the converters accept).
    {
        using phxtool::BinDoc;
        BinDoc d;
        check(BinDoc::load(kItems, d), "entity editor loads the phxbin JSON");
        check(d.struct_name == "ItemRecord" && d.fields.size() == 3 && d.records.size() == 2,
              "entity doc shape");
        check(d.records[0][1] == 100 && d.records[1][2] == -3, "entity doc values");

        d.step(0, 1, +10);                              // price 100 -> 110
        d.step(1, 2, -1000000);                         // atk clamps to i16 min
        check(d.records[0][1] == 110 && d.records[1][2] == -32768, "step + type clamp");
        d.add_record(0);                                // clone record 0
        d.remove_record(1);
        check(d.records.size() == 2 && d.records[1][1] == 110, "clone + delete records");

        write_file("build/p_items_edited.json", d.save_json().data(), d.save_json().size());
        BinDoc r;
        check(BinDoc::load(d.save_json(), r) && r.records.size() == 2 &&
              r.records[1][1] == 110 && r.records[1][2] == 5, "entity doc round-trip");
        phxtool::BundleWriter wb(2);
        check(phxtool::build_bin(wb, "build/p_items_edited.json", "items2",
                                 "build/p_items_edited.gen.h"),
              "edited table still bakes through the real phxbin builder");
    }

    // phxentity schema authoring: a fresh table from a CLI-style spec (--new/--fields),
    // field add/remove keeping every record in shape, and honest failures on bad specs.
    {
        using phxtool::BinDoc;
        BinDoc d;
        std::string err;
        check(BinDoc::blank("Enemy", { "hp:u16", "atk:i8" }, d, &err), "blank table from a schema");
        check(d.struct_name == "Enemy" && d.fields.size() == 2 && d.records.empty(), "blank shape");
        d.add_record(0); d.add_record(0);
        check(d.add_field("speed", "u8"), "add a field");
        check(d.fields.size() == 3 && d.records[0].size() == 3 && d.records[1][2] == 0,
              "records grew with the schema");
        check(!d.add_field("hp", "u16"), "duplicate field name refused");
        check(!d.add_field("x", "f64"), "unknown field type refused");
        check(d.remove_field(1), "remove a field");
        check(d.fields.size() == 2 && d.fields[1].name == "speed" && d.records[0].size() == 2,
              "records shrank with the schema");

        BinDoc bad;
        check(!BinDoc::blank("Enemy", { "hp=u16" }, bad, &err) && !err.empty(),
              "bad field spec reports why");
        check(!BinDoc::blank("Enemy", {}, bad, &err), "empty schema refused");

        // the new table's saved JSON still bakes through the real phxbin builder
        const std::string j = d.save_json();
        write_file("build/p_new_table.json", j.data(), j.size());
        phxtool::BundleWriter wb(2);
        check(phxtool::build_bin(wb, "build/p_new_table.json", "enemies"),
              "schema-authored table bakes through phxbin");

        // malformed author JSON reports a positioned parse error (the diagnostics seam)
        BinDoc m;
        check(!BinDoc::load("{ \"struct\":\"X\",\n  \"fields\":[{\"name\":\"a\" \"type\":\"u8\"}] }", m, &err) &&
              err.find("line 2") != std::string::npos,
              "parse errors carry line/col positions");
    }

    // The PREFAB SCHEMA seam (docs/08 §8): ONE phxbin record table with a string "type"
    // column is (a) phxentity's editable stats table, (b) the vocabulary phxtmap places in
    // entity mode (name_column), and (c) a baked blob whose char[N] name the game hashes to
    // match spawn types. Prove all three from the same author JSON.
    {
        const char* kPrefabs =
        "{ \"struct\":\"Prefab\","
        "  \"fields\":[ {\"name\":\"type\",\"type\":\"str16\"},"
        "               {\"name\":\"hp\",\"type\":\"u16\"}, {\"name\":\"speed\",\"type\":\"i16\"} ],"
        "  \"records\":[ {\"type\":\"player\",\"hp\":3,\"speed\":70},"
        "                {\"type\":\"enemy\",\"hp\":1,\"speed\":28},"
        "                {\"type\":\"a-very-long-prefab-name\",\"hp\":9,\"speed\":0} ] }";
        write_file("build/p_prefabs.json", kPrefabs, std::strlen(kPrefabs));

        // (a) the entity editor loads/edits/saves it, strings preserved verbatim
        using phxtool::BinDoc;
        BinDoc d;
        check(BinDoc::load(kPrefabs, d), "prefab table loads in the entity editor");
        check(d.field_is_str(0) && !d.field_is_str(1), "str16 column recognized");
        check(d.str_cell(0, 0) == "player" && d.records[0][1] == 3, "string + int cells coexist");
        d.step(0, 0, +1);
        check(d.str_cell(0, 0) == "player" && !d.dirty, "stepping a string cell is a no-op");
        d.add_record(1);                                  // clone 'enemy'
        d.set_str(3, 0, "boss");
        d.step(3, 1, +9);
        BinDoc r;
        check(BinDoc::load(d.save_json(), r) && r.str_cell(3, 0) == "boss" && r.records[3][1] == 10,
              "string cells survive the save round-trip");
        check(BinDoc::blank("P", { "type:str16", "hp:u8" }, r), "str fields allowed in --new specs");
        BinDoc esc; esc.struct_name = "E";
        esc.fields = { BinDoc::Field{ "type", "str8" } };
        esc.add_record(99); esc.set_str(0, 0, "a\"b\\c");
        check(BinDoc::load(esc.save_json(), r) && r.str_cell(0, 0) == "a\"b\\c",
              "quotes/backslashes in a name survive save -> load");

        // (b) the tilemap editor's placeable vocabulary comes from the same table
        auto names = d.name_column();
        check(names.size() == 4 && names[0] == "player" && names[3] == "boss",
              "name_column harvests the type column for phxtmap --prefabs");

        // (c) phxbin bakes the string column as inline NUL-terminated char[N]
        phxtool::BundleWriter wp(2);
        check(phxtool::build_bin(wp, "build/p_prefabs.json", "prefabs", "build/p_prefabs.gen.h"),
              "prefab table bakes through phxbin");
        check(wp.write("build/p_prefabs.phxp"), "write prefab bundle");
        ResourceCache* cp = ResourceCache::create(arena).unwrap();
        check(cp->mount(plat, "build/p_prefabs.phxp") == Status::Ok, "mount prefab bundle");
        auto pb = cp->blob("prefabs"_hash);
        check(pb.ok(), "blob('prefabs') baked");
        if (pb.ok()) {
            const uint8_t* p = static_cast<const uint8_t*>(pb.unwrap().data);
            uint32_t count = 0, stride = 0;
            std::memcpy(&count, p + 0, 4); std::memcpy(&stride, p + 4, 4);
            // Prefab natural C layout: char type[16] @0, u16 hp @16, i16 speed @18 -> stride 20.
            check(count == 3 && stride == 20, "prefab count/stride (char[16] inline)");
            const char* t0 = reinterpret_cast<const char*>(p + 8);
            const char* t2 = reinterpret_cast<const char*>(p + 8 + 2 * stride);
            check(std::strcmp(t0, "player") == 0, "record 0 name baked NUL-terminated");
            check(phx::fnv1a(t0) == "player"_hash, "baked name hashes to the spawn-type hash");
            check(std::strlen(t2) == 15 && std::memcmp(t2, "a-very-long-pre", 15) == 0,
                  "overlong name truncates to N-1 chars + NUL");
            uint16_t hp0 = 0; std::memcpy(&hp0, p + 8 + 16, 2);
            check(hp0 == 3, "int field after the string column reads back");
            TableView t;
            check(t.parse(pb.unwrap()) && t.find("type"_hash, "player"_hash) == 0 && t.find("type"_hash, "ghost"_hash) == -1,
                  "a prefab row is found by its type column");
            check(t.get_int(0, "hp"_hash) == 3 && std::strcmp(t.get_str(0, "type"_hash), "player") == 0 &&
                  t.get_hash(0, "type"_hash) == "player"_hash && t.get_str(0, "hp"_hash) == nullptr,
                  "prefab columns by name: int, str, hash; an int column is not a str");
        }
        bool gen_str = false;
        if (FILE* h = std::fopen("build/p_prefabs.gen.h", "rb")) {
            std::string s; int c; while ((c = std::fgetc(h)) != EOF) s += char(c); std::fclose(h);
            gen_str = s.find("char      type[16];") != std::string::npos &&
                      s.find("static_assert(sizeof(Prefab) == 20") != std::string::npos;
        }
        check(gen_str, "generated header declares char type[16] with the right stride");
    }

    // the generated accessor header exists and declares the struct
    bool gen_ok = false;
    if (FILE* h = std::fopen("build/p_items.gen.h", "rb")) {
        std::string s; int c; while ((c = std::fgetc(h)) != EOF) s += char(c); std::fclose(h);
        gen_ok = s.find("struct ItemRecord") != std::string::npos &&
                 s.find("static_assert(sizeof(ItemRecord)") != std::string::npos;
    }
    check(gen_ok, "phxbin emitted a matching .gen.h");


    // --- Phosphorus Studio: the headless document model behind tools/phxstudio ----------------
    // Everything the studio draws comes from these functions, so they are held to the real
    // tree (depcheck's layer table, caps.h, the Makefile) and to the real bundle format.
    {
        using namespace phxstudio;

        // the engine map agrees with depcheck.py about layers AND violations
        EngineMap em;
        std::string err;
        check(scan_engine(".", em, &err), "studio: scan_engine reads depcheck.py + engine/");
        check(em.layers.size() == 5 && em.layer_notes.size() == 5, "studio: five layers (+ notes) parsed");
        const int core = em.find("core"), render = em.find("render"), runtime = em.find("runtime");
        check(core >= 0 && em.modules[size_t(core)].layer == 0, "studio: core is L0");
        check(runtime >= 0 && em.modules[size_t(runtime)].layer == 4, "studio: runtime is L4");
        check(em.violations.empty(), "studio: no upward edges (matches the depcheck gate)");
        check(render >= 0 && em.reaches(render, core) && !em.reaches(core, render),
              "studio: render depends on core, never the reverse");
        check(runtime >= 0 && em.modules[size_t(runtime)].users.empty() && em.edges > 10,
              "studio: runtime is the composition root (nothing includes it)");
        check(render >= 0 && std::find(em.modules[size_t(render)].backends.begin(),
                                       em.modules[size_t(render)].backends.end(), "soft") !=
                                 em.modules[size_t(render)].backends.end(),
              "studio: render's per-tier backends are listed");
        check(header_summary("// phx/x/y.h \xE2\x80\x94 does a thing.\n// More.\n#ifndef X\n") == "does a thing. More.",
              "studio: header_summary strips the self-reference and joins lines");
        check(header_summary("/* phx/p.h \xE2\x80\x94 C seam\n * second line */\n") == "C seam second line",
              "studio: header_summary reads C block comments");
        const auto inc = included_modules("#include \"phx/core/types.h\"\n#  include <phx/render/renderer.h>\n#include \"x.h\"\n");
        check(inc.size() == 2 && inc[0] == "core" && inc[1] == "render", "studio: included_modules finds phx/<mod>/");

        // capability tiers come from caps.h, not a copy
        std::string caps_h;
        std::vector<TierCaps> tiers;
        check(read_text("engine/core/include/phx/core/caps.h", caps_h) && parse_caps(caps_h, tiers) &&
              tiers.size() == 3, "studio: caps.h parses into three tiers");
        if (tiers.size() == 3) {
            check(tiers[0].name == "GBA" && tiers[0].max_sprites == 128 && tiers[0].render_tier == 0 &&
                  tiers[0].has_float_hw == 0, "studio: GBA caps (OAM ceiling, tier 0, no FPU)");
            check(tiers[0].total_ram == 224u * 1024u, "studio: caps expressions evaluate (224u * 1024u)");
            check(tiers[2].name == "PC" && tiers[2].render_tier == 2 && tiers[2].max_sprites > tiers[1].max_sprites,
                  "studio: PC caps from the #else block");
        }

        // names: literals, manifests, and the fallback label
        NameBook nb;
        nb.add_literals("w.add_sprite(\"hero\", \"hero_sheet\"); x == \"coin\"_hash; \"not a name!\"; \"a\\\"b\"");
        check(nb.find(phx::fnv1a("hero")) && nb.find(phx::fnv1a("coin")) && nb.find(phx::fnv1a("hero_sheet")),
              "studio: NameBook harvests identifier literals");
        check(!nb.find(phx::fnv1a("not a name!")), "studio: NameBook skips prose literals");
        nb.add_manifest("# phxpack manifest\n0x0cb633fd texture  li_t   <- build/li_t.ppm\n");
        check(nb.label(0x0cb633fdu) == "li_t", "studio: NameBook reads phxpack manifests");
        check(nb.label(0x12345678u) == "#12345678", "studio: unknown hashes print as #hex");

        // a bundle with one of every asset type, through BundleDoc + the typed views
        uint32_t px[16 * 8];
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 16; ++x)
                px[y * 16 + x] = (x + y) % 3 == 0 ? 0u : x < 8 ? rgba(250, 40, 20) : rgba(20, uint8_t(60 + x * 10), 200);
        const uint16_t cells[2 * 3 * 2] = { 1, 2, 0, 2, 1, 0,   0, 0, 1, 2, 2, 2 };
        const std::vector<std::pair<double, double>> par = { { 0.5, 1.0 }, { 1.0, 1.0 } };
        const std::vector<uint8_t> flags = { 0, kTileFlagSolid, kTileFlagHazard };
        const int16_t pcm[6] = { 0, 12000, -32768, 32767, -5, 5 };
        std::vector<SpawnDef> spawns(2);
        spawns[0] = SpawnDef{ phx::fnv1a("player"), 8, 16, 8, 8 };
        spawns[1] = SpawnDef{ phx::fnv1a("coin"), -4, 2, 0, 0 };
        phxtool::BundleWriter sw(2);
        sw.add_texture("s_tiles", px, 16, 8);
        sw.add_tilemap("s_map", cells, 3, 2, 2, 8, 8, "s_tiles", &par, &flags);
        sw.add_sprite("s_hero", "s_tiles", 8, 8, 2, { phx::SpriteClipDef{ phx::fnv1a("walk"), 0, 2, 8, 1, 0 } });
        sw.add_sound("s_tone", pcm, 6, 22050);
        sw.add_spawns("s_map", spawns);
        const char blob[] = "phosphorus";
        sw.add_blob("s_blob", blob, sizeof(blob));
        check(sw.write("build/p_studio.phxp"), "studio: write the fixture bundle");

        BundleDoc bd;
        check(BundleDoc::load("build/p_studio.phxp", bd) && bd.ok && bd.crc == 1 && bd.assets.size() == 6,
              "studio: BundleDoc loads + CRC-verifies a fresh bundle");
        const int ti = bd.find(phx::fnv1a("s_tiles"), AssetType::Texture);
        TexView tv;
        std::vector<uint32_t> back;
        check(ti >= 0 && view_texture(bd.assets[size_t(ti)], tv) && tv.w == 16 && tv.h == 8 &&
              decode_rgba8(tv, back) && std::memcmp(back.data(), px, sizeof(px)) == 0,
              "studio: texture view decodes RGBA8 exactly");
        MapView mv;
        const int mi = bd.find(phx::fnv1a("s_map"), AssetType::Tilemap);
        check(mi >= 0 && view_tilemap(bd.assets[size_t(mi)], mv) && mv.w == 3 && mv.h == 2 && mv.layers == 2 &&
              mv.indices[3] == 2 && mv.tileset == phx::fnv1a("s_tiles"), "studio: tilemap view (cells + tileset)");
        check(mv.parallax_q16.size() == 4 && mv.parallax_q16[0] == 1 << 15 && mv.parallax_q16[2] == 1 << 16,
              "studio: tilemap view reads per-layer parallax (Q16)");
        check(mv.tile_flags && mv.tile_flag_count == 3 && mv.tile_flags[2] == kTileFlagHazard,
              "studio: tilemap view reads the tile-flag table");
        SpriteInfo si;
        const int spi = bd.find(phx::fnv1a("s_hero"), AssetType::Sprite);
        check(spi >= 0 && view_sprite(bd.assets[size_t(spi)], si) && si.clips.size() == 1 &&
              si.clips[0].count == 2 && si.texture == phx::fnv1a("s_tiles"), "studio: sprite view (clips)");
        SoundInfo so;
        const int soi = bd.find(phx::fnv1a("s_tone"), AssetType::Sound);
        check(soi >= 0 && view_sound(bd.assets[size_t(soi)], so) && so.frames == 6 && so.rate == 22050 &&
              so.samples[2] == -32768, "studio: sound view (PCM + rate)");
        std::vector<SpawnDef> sp;
        const int sgi = bd.find(phx::fnv1a("s_map"), AssetType::Spawns);
        check(sgi >= 0 && view_spawns(bd.assets[size_t(sgi)], sp) && sp.size() == 2 && sp[1].x == -4,
              "studio: spawns view (signed coords)");
        check(describe(bd.assets[size_t(ti)]) == "16x8 RGBA8" && describe(bd.assets[size_t(mi)]) == "3x2 tiles x2 layers",
              "studio: describe() one-liners");
        std::vector<int16_t> wmn, wmx;
        waveform(so.samples, so.frames, 3, wmn, wmx);
        check(wmx[0] == 12000 && wmn[1] == -32768 && wmx[1] == 32767 && wmn[2] == -5, "studio: waveform min/max columns");

        // per-tier analysis: the studio's GBA/PSP previews decode back to what the bake encodes
        const TierReport tr = analyse_tiers(back, 16, 8);
        check(tr.swz_ok && tr.pal4_ok && tr.pal4_palettes >= 1 && tr.tiles_x == 2 && tr.tiles_over == 0,
              "studio: tier report (swizzle + 4bpp encode succeed)");
        TexView sv{ 16, 8, PixelFormat::RGBA8_SWZ, tr.swz.data(), uint32_t(tr.swz.size()), 0 };
        std::vector<uint32_t> from_swz;
        check(decode_rgba8(sv, from_swz) && from_swz == back, "studio: RGBA8_SWZ decodes pixel-identical");
        TexView pv{ 16, 8, PixelFormat::PAL4_TILES, tr.pal4.data(), uint32_t(tr.pal4.size()), tr.pal4_palettes };
        std::vector<uint32_t> from_pal4;
        bool pal4_same = decode_rgba8(pv, from_pal4);
        for (size_t i = 0; pal4_same && i < back.size(); ++i) {
            const uint32_t want = (back[i] >> 24) ? bgr555_to_rgba8(rgba8_to_bgr555(back[i])) : 0u;
            pal4_same = from_pal4[i] == want;
        }
        check(pal4_same, "studio: PAL4_TILES decodes to the BGR555-quantized source");
        std::vector<uint32_t> busy(8 * 8);
        for (size_t i = 0; i < busy.size(); ++i) busy[i] = rgba(uint8_t(i * 4), uint8_t(255 - i * 3), 9);
        const TierReport over = analyse_tiers(busy, 8, 8);
        check(!over.pal4_ok && over.tiles_over == 1 && over.tile_colors[0] > 15 && over.swz_ok,
              "studio: a >15-colour tile is flagged and falls back on GBA");
        check(!analyse_tiers(back, 12, 8).pal4_ok, "studio: non-8px-aligned art can't go 4bpp");

        // validation mirrors ResourceCache::mount: corrupt / truncated / foreign files are refused
        std::vector<uint8_t> raw;
        read_bytes("build/p_studio.phxp", raw);
        std::vector<uint8_t> bad = raw;
        bad.back() ^= 0x5A;
        write_file("build/p_studio_crc.phxp", bad.data(), bad.size());
        BundleDoc bc;
        BundleDoc::load("build/p_studio_crc.phxp", bc);
        check(bc.ok && bc.crc == -1 && !bc.error.empty(), "studio: a flipped byte is a CRC mismatch (still viewable)");
        write_file("build/p_studio_cut.phxp", raw.data(), raw.size() - 7);
        BundleDoc bt;
        check(!BundleDoc::load("build/p_studio_cut.phxp", bt) && !bt.ok, "studio: a truncated bundle is refused");
        bad = raw; bad[0] = 'X';
        write_file("build/p_studio_magic.phxp", bad.data(), bad.size());
        BundleDoc bm;
        check(!BundleDoc::load("build/p_studio_magic.phxp", bm) && bm.error.find("magic") != std::string::npos,
              "studio: a foreign file is refused by magic");

        // the launch catalog is derived from the Makefile (every check suite is runnable)
        std::string mk;
        check(read_text("Makefile", mk), "studio: read the Makefile");
        const auto prereqs = make_prereqs(mk, "check");
        check(std::find(prereqs.begin(), prereqs.end(), "pipeline") != prereqs.end() &&
              std::find(prereqs.begin(), prereqs.end(), "depcheck") != prereqs.end(),
              "studio: `check:` prerequisites parsed");
        const auto crlf = make_prereqs("x: y\r\ncheck: a b \\\r\n  c\r\nd: e\r\n", "check");
        check(crlf.size() == 3 && crlf[1] == "b" && crlf[2] == "c", "studio: prerequisites of a CRLF Makefile");
        check(make_has_target(mk, "studio") && make_has_target(mk, "emberwing-sdl") && !make_has_target(mk, "CXXFLAGS"),
              "studio: make_has_target (rules, not variables)");
        const auto launches = default_launches(mk, "/nonexistent/devkitARM");
        size_t suites = 0;
        bool gba_needs_devkit = false;
        for (const Launch& l : launches) {
            suites += l.group == Group::Suite;
            if (l.make_target == "gba-emberwing-ppu") gba_needs_devkit = !missing_need(l).empty();
        }
        check(suites == prereqs.size(), "studio: one suite launch per `check:` prerequisite");
        check(gba_needs_devkit, "studio: a missing toolchain disables its launch");
        check(make_prereqs("a: x \\\n y | z\n\tcmd\n", "a") == std::vector<std::string>({ "x", "y" }),
              "studio: continuation lines joined, order-only prerequisites dropped");

        // console classification + the line ring
        check(classify_line("PIPELINE PASS") == Tone::Good && classify_line("depcheck: OK (28 edges)") == Tone::Good,
              "studio: verdict lines are good");
        check(classify_line("    FAIL something") == Tone::Bad && classify_line("make: *** [Makefile:1: x] Error 2") == Tone::Bad &&
              classify_line("x.cpp:3:1: error: nope") == Tone::Bad, "studio: failures and errors are bad");
        check(classify_line("anim_test: rc=0  4 checks, 0 failures") == Tone::Plain, "studio: '0 failures' is not a failure");
        check(classify_line("x.cpp:9: warning: unused") == Tone::Warn && classify_line("g++ -std=c++17 a.cpp") == Tone::Dim,
              "studio: warnings and compiler lines");
        LogRing lr(3);
        const char chunk[] = "one\r\ntw";
        lr.feed(chunk, sizeof(chunk) - 1);
        lr.feed("o\n\x1b[32mPASS\x1b[0m\nprogress 10%\rprogress 99%\n", 43);
        lr.push("four");
        check(lr.size() == 3 && lr.total() == 5 && lr.at(0).text == "PASS" && lr.at(0).tone == Tone::Good &&
              lr.at(1).text == "progress 99%" && lr.at(2).text == "four", "studio: LogRing splits, strips ANSI, honours \\r, caps");
        check(exit_code_from_status(0) == 0 && exit_code_from_status(3 << 8) == 3 && exit_code_from_status(15) == 143,
              "studio: wait status -> exit code (signals as 128+N)");

        // tools on PATH + the Windows shell lookup (pure; the fake installs live under build/)
        {
            const std::vector<std::string> pl = split_path_list("C:\\a;;C:\\b c;", ';');
            check(pl.size() == 2 && pl[0] == "C:\\a" && pl[1] == "C:\\b c", "studio: PATH list split, empties dropped");
            check(split_path_list("/usr/bin::/bin", ':').size() == 2, "studio: POSIX PATH list split");
            namespace sfs = std::filesystem;
            std::error_code ec;
            const std::string t = sfs::absolute("build/p_tools").generic_string();
            sfs::remove_all(t, ec);
            for (const char* d : { "/bin", "/msys/usr/bin", "/msys/ucrt64/bin", "/msys/opt/dk/bin", "/git/usr/bin" })
                sfs::create_directories(t + d, ec);
            for (const char* f : { "/bin/sdl2-config", "/bin/make.exe", "/msys/usr/bin/sh.exe", "/msys/usr/bin/make.exe",
                                   "/msys/opt/dk/bin/arm-g++.exe", "/git/usr/bin/sh.exe" })
                write_file((t + f).c_str(), "x", 1);
            const std::vector<std::string> dirs = { t + "/bin" };
            check(resolve_tool("sdl2-config", dirs, true) == t + "/bin/sdl2-config" &&
                  resolve_tool("make", dirs, true) == t + "/bin/make.exe" && resolve_tool("make", dirs, false).empty(),
                  "studio: tools resolve bare, and with .exe on Windows only");
            check(resolve_tool("/opt/dk/bin/arm-g++", {}, true, t + "/msys") == t + "/msys/opt/dk/bin/arm-g++.exe" &&
                  resolve_tool("/opt/dk/bin/arm-g++", {}, false, t + "/msys").empty(),
                  "studio: a POSIX-absolute need resolves under the shell's root on Windows");
            // sh.exe beside make wins; the toolchain folder and the shell's bin go in front of PATH
            const PosixShell s1 = find_posix_shell({ t + "/msys/usr/bin" }, "", "", { t + "/git/usr/bin/sh.exe" });
            check(s1.sh == t + "/msys/usr/bin/sh.exe" && s1.root == t + "/msys" && s1.add_path.size() == 1 &&
                  s1.add_path[0] == t + "/msys/ucrt64/bin", "studio: MSYS2 shell beside make, ucrt64 added to PATH");
            const PosixShell s2 = find_posix_shell({ t + "/bin" }, "", "UCRT64", { t + "/msys/usr/bin/sh.exe" });
            check(s2.sh == t + "/msys/usr/bin/sh.exe" && s2.add_path.size() == 2 && s2.add_path[1] == t + "/msys/usr/bin",
                  "studio: shell from the fallbacks, its bin added to PATH");
            const PosixShell s3 = find_posix_shell({}, t + "/git/usr/bin/sh.exe", "", {});
            check(s3.sh == t + "/git/usr/bin/sh.exe" && s3.root == t + "/git", "studio: PHX_SH overrides the search");
            check(find_posix_shell({ t + "/bin" }, "", "", {}).sh.empty(), "studio: no shell found");
            // launch needs name SDK variables; DEVKITPRO/DEVKITARM default like the Makefile's
            auto no_env = [](const std::string&) { return std::string(); };
            auto dkp_env = [](const std::string& k) { return std::string(k == "DEVKITPRO" ? "/c/dkp" : ""); };
            check(expand_need_vars("$DEVKITARM/bin/arm-none-eabi-g++", no_env) == "/opt/devkitpro/devkitARM/bin/arm-none-eabi-g++" &&
                  expand_need_vars("${DEVKITARM}/bin/x", dkp_env) == "/c/dkp/devkitARM/bin/x" &&
                  expand_need_vars("$NOPE/x $", no_env) == "$NOPE/x $" && expand_need_vars("psp-g++", no_env) == "psp-g++",
                  "studio: launch needs expand $VAR / ${VAR} with the SDK defaults");
            sfs::remove_all(t, ec);
        }

        // layout math
        const Rect f1 = fit_rect(16, 8, Rect{ 0, 0, 100, 100 });
        check(f1.w == 96 && f1.h == 48 && f1.x == 2 && f1.y == 26, "studio: fit_rect integer upscale, centered");
        const Rect f2 = fit_rect(400, 100, Rect{ 0, 0, 200, 200 });
        check(f2.w == 200 && f2.h == 50, "studio: fit_rect shrinks oversize art by aspect");
        check(clamp_scroll(50, 60, 20) == 40 && clamp_scroll(-3, 60, 20) == 0 && clamp_scroll(5, 10, 20) == 0,
              "studio: clamp_scroll keeps the last page full");
        int pos = 0, len = 0;
        scroll_thumb(100, 25, 75, 100, pos, len);
        check(len == 25 && pos == 75, "studio: scrollbar thumb geometry");
        check(scroll_from_track(100, 25, 100, 0) == 0 && scroll_from_track(100, 25, 100, 100) == 75 &&
              scroll_from_track(100, 25, 100, 50) == 38, "studio: scrollbar track -> first row");
        check(wrap("the quick brown fox", 9) == std::vector<std::string>({ "the quick", "brown fox" }) &&
              wrap("abcdefghij", 4) == std::vector<std::string>({ "abcd", "efgh", "ij" }), "studio: word wrap");
        check(human_bytes(224 * 1024) == "224 KB" && human_bytes(1536) == "1.5 KB" && human_bytes(12) == "12 B",
              "studio: human_bytes");
        check(!find_repo_root(".").empty(), "studio: the repo root is found from the working directory");
    }

    // ---- synthesized audio: .sfx sound effects + .song tracker music (tools/phxpack/synth.h) ----
    {
        using namespace phxtool;
        // sound effects: deterministic, length = attack + sustain + decay, round-trips through JSON
        SfxParams p = sfx_preset("pickup", 7);
        const std::vector<int16_t> a = render_sfx(p), b = render_sfx(p);
        check(!a.empty() && a == b && a.size() == size_t(p.length() * kSynthRate),
              "sfx: a render is deterministic and as long as its envelope");
        int peak = 0;
        for (int16_t v : a) peak = std::max(peak, v < 0 ? -int(v) : int(v));
        check(peak > 4000, "sfx: the pickup preset is audible");
        check(sfx_preset("pickup", 7) == p && sfx_preset("pickup", 8) != p && sfx_random(3) == sfx_random(3) &&
              sfx_mutate(p, 5) == sfx_mutate(p, 5) && sfx_mutate(p, 5).wave == p.wave,
              "sfx: presets / randomize / mutate are reproducible from their seed");
        for (const std::string& n : sfx_preset_names()) {
            const SfxParams q = sfx_preset(n, 1);
            check(!render_sfx(q).empty(), ("sfx: preset '" + n + "' renders").c_str());
        }
        SfxParams r;
        check(sfx_from_json(sfx_to_json(p), r) && r == p, "sfx: JSON round trip");
        check(sfx_from_json("{ \"wave\": \"noise\", \"freq\": 200 }", r) && r.wave == Wave::Noise && r.freq == 200 &&
              r.decay == SfxParams{}.decay, "sfx: missing keys keep their defaults");
        std::string err;
        check(!sfx_from_json("{ \"wave\": \"kazoo\" }", r, &err) && err.find("kazoo") != std::string::npos,
              "sfx: an unknown wave is an error");
        SfxParams fall; fall.freq = 800; fall.slide = -8; fall.freq_min = 200; fall.sustain = 1; fall.decay = 1;
        check(render_sfx(fall).size() < size_t(0.3 * kSynthRate), "sfx: a falling slide stops at freq_min");

        // notes and cells
        check(note_from_text("A-4") == 57 && note_from_text("C#4") == 49 && note_from_text("c-0") == 0 &&
              note_from_text("B-7") == 95 && note_from_text("H-4") < 0 && note_text(49) == "C#4" &&
              std::fabs(note_freq(57) - 440.0) < 1e-9, "song: note names <-> numbers (A-4 = 440 Hz)");
        Song s = song_starter();
        SongCell c;
        check(cell_from_text(s, "E-3 bass", c) && c.note == 40 && c.inst == 1 && cell_text(s, c) == "E-3 bass" &&
              cell_from_text(s, "off", c) && c.note == kCellOff && cell_from_text(s, "", c) && c.note == kCellEmpty &&
              !cell_from_text(s, "E-3 tuba", c, &err) && !cell_from_text(s, "X-9", c), "song: cell text");

        // songs: length is rows x row frames; JSON round trip keeps every cell
        const std::vector<int16_t> pcm = render_song(s);
        check(s.total_rows() == 32 && pcm.size() == size_t(s.total_rows()) * s.row_frames() && pcm == render_song(s),
              "song: the render is deterministic and exactly rows x row length");
        int speak = 0;
        for (int16_t v : pcm) speak = std::max(speak, v < 0 ? -int(v) : int(v));
        check(speak > 3000 && speak < 20000, "song: audible, with headroom for the sound effects over it");
        Song t;
        check(song_from_json(song_to_json(s), t) && t.patterns.size() == 2 && t.order == s.order &&
              t.patterns[0].rows == s.patterns[0].rows && t.instruments.size() == 4 && t.instruments[2] == s.instruments[2],
              "song: JSON round trip");
        check(!song_from_json("{ \"patterns\": [], \"order\": [\"Z\"] }", t, &err) && err.find("Z") != std::string::npos,
              "song: the order naming an unknown pattern is an error");
        int oi = -1, row = -1;
        check(s.position_at(s.row_seconds() * 17.5, oi, row) && oi == 1 && row == 1 && !s.position_at(s.seconds() + 1, oi, row),
              "song: position_at maps a time to the playing pattern row");

        // editing keeps indices consistent
        Song e = s;
        const int d = e.duplicate_pattern(0);
        e.order.push_back(d);
        e.remove_pattern(0);
        check(e.patterns.size() == 2 && e.patterns[0].name == "B" && e.order == std::vector<int>({ 0, 1 }),
              "song: removing a pattern drops it from the order and renumbers the rest");
        e.remove_instrument(0);                                    // "lead": its notes fall back
        check(e.instruments.size() == 3 && e.patterns[1].rows[0][0].inst == -1 && e.patterns[1].rows[0][1].inst == 0,
              "song: removing an instrument renumbers the cells that name later ones");
        check(e.rename_instrument(0, "sub") && !e.rename_instrument(0, "kick") && !e.rename_instrument(0, "a b"),
              "song: instrument names stay unique, without spaces");
        e.set_channels(2);
        check(e.channels == 2 && e.patterns[0].rows[0].size() == 2, "song: channel count resizes every row");
        e.resize_pattern(0, 8);
        check(e.patterns[0].rows.size() == 8 && e.add_pattern(4) == 2 && e.patterns[2].name == "A",
              "song: resize a pattern, add one with a fresh name");
        check(!render_note(s.instruments[0], 48).empty(), "song: a single instrument note renders (the editor's audition)");
        check(song_problems(Song{}).size() == 2 && song_problems(s).empty(), "song: problems (no instruments, empty order)");

        // the bake: .sfx / .song -> Sound assets, read back from a mounted bundle
        write_file("build/p_coin.sfx", sfx_to_json(p).data(), sfx_to_json(p).size());
        const std::string sj = song_to_json(s);
        write_file("build/p_theme.song", sj.data(), sj.size());
        BundleWriter w(2);
        check(build_from_source(w, "build/p_coin.sfx") && build_from_source(w, "build/p_theme.song") &&
              w.write("build/p_synth.phxp"), "sfx/song: the bake dispatches them to Sound assets");
        ResourceCache* cs = ResourceCache::create(arena).unwrap();
        check(cs->mount(plat, "build/p_synth.phxp") == Status::Ok, "sfx/song: mount");
        auto cv = cs->sound("p_coin"_hash);
        auto tv = cs->sound("p_theme"_hash);
        check(cv.ok() && cv.unwrap().rate == kSynthRate && cv.unwrap().frames == a.size() &&
              std::memcmp(cv.unwrap().samples, a.data(), a.size() * 2) == 0, "sfx: the baked sound is the editor's render");
        check(tv.ok() && tv.unwrap().frames == pcm.size(), "song: the baked music is the whole song");
        BundleWriter w0(0);                                        // tier 0: resampled to the GBA rate
        check(build_song(w0, "build/p_theme.song", "theme") && w0.write("build/p_synth.t0.phxp"), "song: tier-0 bake");
        ResourceCache* c0 = ResourceCache::create(arena).unwrap();
        check(c0->mount(plat, "build/p_synth.t0.phxp") == Status::Ok && c0->sound("theme"_hash).ok() &&
              c0->sound("theme"_hash).unwrap().rate == 18157, "song: the tier-0 bake is at the GBA device rate");
        check(!build_sfx(w, "build/nope.sfx"), "sfx: a missing file fails the bake");
    }

    // ---- fonts: a .font grid sheet (proportional / fixed) and a BMFont .fnt -> Texture + Font ----
    {
        using namespace phxtool;
        std::vector<uint32_t> atlas(size_t(kAsciiFontW) * kAsciiFontH);
        build_ascii_font(atlas.data());
        check(png_write_file("build/p_font.png", atlas.data(), kAsciiFontW, kAsciiFontH), "font: write the ASCII sheet");
        const std::string fj = "{ \"font\": 1, \"image\": \"p_font.png\", \"cell_w\": 8, \"cell_h\": 8, \"space\": 3, \"line_h\": 9 }";
        write_file("build/p_font.font", fj.data(), fj.size());
        FontDef fd;
        std::string err;
        check(load_fontdef(fj, "build/p_font.font", fd, &err) && fd.proportional && fd.spacing == 1 && fd.image == "p_font.png" &&
              font_image_path("build/p_font.font", fd.image) == "build/p_font.png", "font: .font defaults and the sheet path");
        FontDef rt;
        check(load_fontdef(fontdef_to_json(fd), "x.font", rt) && rt.space == 3 && rt.line_h == 9 && rt.cell_w == 8,
              "font: .font JSON round trip");
        phx::FontBlobHeader hdr{};
        std::vector<phx::FontGlyphDef> gl;
        check(font_glyphs_from_grid(fd, atlas, kAsciiFontW, kAsciiFontH, hdr, gl) && gl.size() == 96 && hdr.first_char == 32 &&
              hdr.line_h == 9 && (hdr.flags & phx::kFontProportional), "font: 96 glyphs from the grid");
        const auto& gi = gl['i' - 32]; const auto& gm = gl['M' - 32]; const auto& gs = gl[0];
        check(gs.w == 0 && gs.advance == 3 && gi.w > 0 && gi.w < gm.w && gi.advance == gi.w + 1 && gm.advance == gm.w + 1 &&
              gm.sx >= ('M' - 32) % 16 * 8 && gm.sx + gm.w <= ('M' - 32) % 16 * 8 + 8 && hdr.advance == gm.advance,
              "font: proportional widths are measured from the pixels (i narrower than M, space = 3)");
        FontDef fx = fd; fx.proportional = false; fx.advance = 6;
        check(font_glyphs_from_grid(fx, atlas, kAsciiFontW, kAsciiFontH, hdr, gl) && gl['i' - 32].advance == 6 &&
              gl['i' - 32].w == 8 && gl[0].w == 0 && !(hdr.flags & phx::kFontProportional) && hdr.advance == 6,
              "font: fixed-width fonts keep whole cells and one advance");
        FontDef few = fd; few.first = 65; few.count = 3;
        check(font_glyphs_from_grid(few, atlas, kAsciiFontW, kAsciiFontH, hdr, gl) && gl.size() == 3 && hdr.first_char == 65,
              "font: first / count pick a range");
        FontDef bad;
        check(!load_fontdef("{ \"font\": 1 }", "x.font", bad, &err) && err.find("image") != std::string::npos &&
              !load_fontdef("{ \"image\": \"a.png\", \"cell_w\": 0 }", "x.font", bad), "font: a .font without a sheet or cells fails");

        // a BMFont text export (two characters, one page)
        const std::string fnt =
            "info face=\"Tiny\" size=8\ncommon lineHeight=10 base=8 scaleW=128 scaleH=48 pages=1\n"
            "page id=0 file=\"p_font.png\"\nchars count=2\n"
            "char id=65 x=8 y=16 width=5 height=7 xoffset=1 yoffset=1 xadvance=7 page=0 chnl=15\n"
            "char id=67 x=24 y=16 width=4 height=7 xoffset=0 yoffset=-1 xadvance=5 page=0 chnl=15\n";
        write_file("build/p_bm.fnt", fnt.data(), fnt.size());
        BmFont bm;
        check(load_bmfont(fnt, "build/p_bm.fnt", bm) && bm.page == "p_font.png" && bm.line_h == 10 && bm.chars.size() == 2,
              "font: BMFont text is parsed");
        check(font_glyphs_from_bmfont(bm, hdr, gl) && hdr.first_char == 65 && gl.size() == 3 && gl[0].advance == 7 &&
              gl[0].xoff == 1 && gl[2].yoff == -1 && gl[1].w == 0 && hdr.line_h == 10, "font: BMFont chars become the glyph table");
        check(!load_bmfont("BMF\x03", "x.fnt", bm, &err), "font: a binary BMFont is refused with a reason");

        // bake + mount: Texture (the sheet) + Font (the table), by the def's name
        BundleWriter w(2);
        check(build_from_source(w, "build/p_font.font") && build_font(w, "build/p_bm.fnt", "p_bm") &&
              w.write("build/p_font.phxp"), "font: .font / .fnt bake");
        ResourceCache* cf = ResourceCache::create(arena).unwrap();
        check(cf->mount(plat, "build/p_font.phxp") == Status::Ok, "font: mount");
        auto fv = cf->font("p_font"_hash);
        check(fv.ok() && fv.unwrap().texture == "p_font"_hash && fv.unwrap().glyph_count == 96 && fv.unwrap().line_h == 9 &&
              fv.unwrap().glyphs[0].advance == 3 && cf->texture("p_font"_hash).ok(),
              "font: the Font asset names its sheet texture; glyphs read in place");
        auto bv = cf->font("p_bm"_hash);
        check(bv.ok() && bv.unwrap().first_char == 65 && bv.unwrap().glyphs[0].xoff == 1, "font: the BMFont import mounts");
        const std::string oob = "common lineHeight=10\npage id=0 file=\"p_font.png\"\nchar id=65 x=200 y=0 width=8 height=8 xadvance=8\n";
        write_file("build/p_oob.fnt", oob.data(), oob.size());
        check(!build_font(w, "build/p_oob.fnt"), "font: a glyph outside the sheet fails the bake");
    }

    // ---- the Studio's Budget view model (tools/phxstudio/budget.h) ----
    {
        using namespace phxstudio;
        const char* kRep =
            "{ \"budget\": 1, \"target\": \"gba\", \"title\": \"T\", \"frames\": 900, \"width\": 240, \"height\": 160,\n"
            "  \"arena\": { \"used\": 150000, \"capacity\": 160000 },\n"
            "  \"frame_scratch\": { \"peak\": 0, \"capacity\": 4096 },\n"
            "  \"entities\": { \"peak\": 300, \"max\": 256 },\n"
            "  \"sprites\": { \"peak\": 40, \"max\": 128, \"dropped\": 0 },\n"
            "  \"tiles_peak\": 118, \"batches_peak\": 1,\n"
            "  \"audio\": { \"sounds\": 3, \"peak\": 9000, \"channels\": 2 },\n"
            "  \"log\": { \"warnings\": 2, \"errors\": 0 } }\n";
        BudgetReport r;
        check(BudgetReport::parse(kRep, r) && r.target == "gba" && r.frames == 900 && r.arena_used == 150000 &&
              r.ents_max == 256 && r.sprites_max == 128 && r.warnings == 2, "budget: a report parses");
        check(!BudgetReport::parse("{ \"nope\": 1 }", r) && !BudgetReport::parse("not json", r), "budget: other JSON is refused");
        BudgetReport ok_rep;
        BudgetReport::parse(kRep, ok_rep);
        // a tier-0 bundle with a texture the GBA cannot hold as 4bpp tiles (more than 15 colours in a tile)
        std::vector<uint32_t> px(16 * 16);
        for (size_t i = 0; i < px.size(); ++i) px[i] = 0xFF000000u | uint32_t(i * 2654435761u & 0x00FFFFFFu);
        phxtool::BundleWriter w0(0);
        w0.add_texture("noisy", px.data(), 16, 16);
        check(w0.write("build/p_budget.t0.phxp"), "budget: write a tier-0 bundle");
        BundleDoc bd;
        check(BundleDoc::load("build/p_budget.t0.phxp", bd), "budget: load it");
        const BundleFacts f = bundle_facts(bd);
        check(f.ok && f.not_tiles.size() == 1 && f.biggest.size() == 1 && f.texture_bytes > 0,
              "budget: bundle facts (a texture kept RGBA8 on tier 0 is flagged)");
        const std::vector<BudgetLine> lines = budget_lines("gba", ok_rep, f);
        auto find = [&](const char* w) -> const BudgetLine* {
            for (const BudgetLine& l : lines) if (l.what.find(w) == 0) return &l;
            return nullptr;
        };
        const BudgetLine* mem = find("memory");
        const BudgetLine* ents = find("entities");
        const BudgetLine* spr = find("sprites");
        const BudgetLine* log = find("engine warnings");
        const BudgetLine* rom = find("ROM");
        const BudgetLine* tiles = find("textures not in GBA");
        check(mem && mem->level == Level::Close && ents && ents->level == Level::Over && spr && spr->level == Level::Ok &&
              log && log->level == Level::Close && rom && rom->level == Level::Ok && tiles && tiles->used == 1,
              "budget: verdicts (94% memory close, entities over, sprites ok, warnings flagged, ROM ok, tile warning)");
        BudgetReport dropped = ok_rep;
        dropped.sprites_dropped = 5;
        const auto dl = budget_lines("gba", dropped, BundleFacts{});
        bool over = false;
        for (const BudgetLine& l : dl) if (l.what == "sprites per frame") over = l.level == Level::Over;
        check(over, "budget: dropped sprites are over budget");
        check(budget_lines("psp", BudgetReport{}, BundleFacts{}).empty(), "budget: nothing measured, no bundle: no lines");

        // the profiler's trace (PHX_TRACE): work = update + render judged against the step
        const char* kTrace =
            "frame,update_us,render_us,present_us,frame_us,budget_us,steps,entities,sprites\n"
            "0,0,0,0,0,16666,0,0,0\n"
            "1,100,200,16300,16600,16666,1,5,10\n"
            "2,9000,9000,100,18100,16666,1,6,12\n"
            "3,300,400,15900,16600,16666,1,7,11\n"
            "4,50,60,16";                                        // cut short by a crash: skipped
        FrameTrace ft;
        check(FrameTrace::parse(kTrace, ft) && ft.rows.size() == 3 && ft.rows[1].update == 9000 && ft.rows[2].ents == 7,
              "profiler: a trace parses (frame 0 and a torn last line skipped)");
        check(ft.over_budget() == 1 && ft.worst(1).size() == 1 && ft.worst(1)[0] == 1,
              "profiler: one frame's work overran the step; it is the worst");
        const FrameTrace::Stat us = ft.stat(&FrameTrace::Row::update);
        check(us.max == 9000 && us.avg == (100 + 9000 + 300) / 3 && us.p50 == 300, "profiler: per-phase avg / p50 / max");
        check(!FrameTrace::parse("not,a,trace\n1,2,3\n", ft), "profiler: other CSV is refused");
    }

    plat->shutdown();
    std::printf("\npipeline_test: %d checks, %d failures\n", g_checks, g_fail);
    std::printf(g_fail == 0 ? "PIPELINE PASS\n\n" : "PIPELINE FAIL\n\n");
    return g_fail == 0 ? 0 : 1;
}
