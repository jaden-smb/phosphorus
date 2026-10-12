// tests/suites/level_test.cpp — the level loader (phx/runtime/level.h) end to end, headless: a map
// authored with the map editor's own document model (TmapDoc: tiles, per-tile collision, spawns),
// a prefab table and a sprite are baked by the real converters, mounted, and loaded into a running
// App. Checks that every spawn became an entity built from its prefab row (sprite + animation,
// collider, dynamic body, PrefabRef), that the collision grid is the map's last layer (a body falls
// and lands on the authored ground), that overlaps report, that sprites draw, and that unload
// clears it all. Runs on both scalar tiers (determinism: TIER=gba_sim).
#include "phx/runtime/app.h"
#include "phx/runtime/level.h"
#include "phx/resource/cache.h"

#include "builders.h"                 // tools/phxpack: the converters' bake logic
#include "editor.h"                   // tools/phxtmap: TmapDoc (what the map editor saves)
#include "fixtures/png_fixtures.h"

#include <cstdio>
#include <cstring>

extern "C" void phx_null_set_max_frames(uint64_t n);

using namespace phx;

// A game's own component, reflected: the level fills it from the prefab's `Guard_*` columns.
struct Guard {
    int16_t  range  = 24;
    scalar   speed  = s_from_int(30);
    bool     angry  = true;          // no Guard_angry column anywhere: keeps this default
    NameHash target = 0;
};
PHX_COMPONENT(Guard, PHX_FIELD(Guard, range), PHX_FIELD(Guard, speed), PHX_FIELD(Guard, angry),
              PHX_FIELD_HASH(Guard, target));

namespace {
int g_checks = 0, g_fail = 0;
void check(bool ok, const char* what) { ++g_checks; if (!ok) { ++g_fail; std::printf("    FAIL %s\n", what); } }
void write_file(const char* path, const void* data, size_t n) {
    if (FILE* f = std::fopen(path, "wb")) { std::fwrite(data, 1, n, f); std::fclose(f); }
}

// The prefab table the Studio's table editor would save: engine columns + a game's own ("value").
const char* kPrefabs =
"{ \"struct\":\"Prefab\","
"  \"fields\":[ {\"name\":\"type\",\"type\":\"str16\"}, {\"name\":\"sprite\",\"type\":\"str16\"},"
"              {\"name\":\"w\",\"type\":\"u8\"}, {\"name\":\"h\",\"type\":\"u8\"}, {\"name\":\"body\",\"type\":\"u8\"},"
"              {\"name\":\"layer\",\"type\":\"u16\"}, {\"name\":\"mask\",\"type\":\"u16\"}, {\"name\":\"value\",\"type\":\"i16\"},"
"              {\"name\":\"components\",\"type\":\"str32\"}, {\"name\":\"Guard_range\",\"type\":\"i16\"},"
"              {\"name\":\"Guard_speed\",\"type\":\"f32\"}, {\"name\":\"Guard_target\",\"type\":\"str16\"} ],"
"  \"records\":[ {\"type\":\"player\",\"sprite\":\"hero\",\"w\":6,\"h\":8,\"body\":1,\"layer\":1,\"mask\":2,"
"                \"components\":\"Guard\",\"Guard_range\":30,\"Guard_speed\":1.5,\"Guard_target\":\"home\"},"
"               {\"type\":\"coin\",\"w\":4,\"h\":4,\"layer\":2,\"mask\":1,\"value\":5,\"components\":\"Nope\"} ] }";
const char* kSprdef = "sheet l_sheet.png 2 2\nclip walk 0 4 8 1\nclip idle 1 1 0 0\n";

// Bake the fixtures into build/l_level.phxp the way `make game-assets` does (converters + merge).
bool bake() {
    write_file("build/l_sheet.png", kSheet8x2, sizeof(kSheet8x2));
    write_file("build/l_tiles.png", kSheet8x2, sizeof(kSheet8x2));
    write_file("build/l_hero.sprdef", kSprdef, std::strlen(kSprdef));
    write_file("build/l_prefabs.json", kPrefabs, std::strlen(kPrefabs));
    // 8x6 tiles of 8 px: a backdrop layer, then the gameplay layer with a floor on row 5.
    phxtool::TmapDoc d = phxtool::TmapDoc::blank(8, 6, 8, 8, "tiles");
    d.add_layer("main");
    for (int x = 0; x < 8; ++x) d.set_tile(1, x, 5, 1);
    d.set_tile(0, 3, 1, 2);                                 // decoration: not collidable (layer 0)
    d.set_tile_flag(1, kTileSolid);
    d.add_spawn("player", 20, 12);
    d.add_spawn("coin", 44, 12);
    d.add_spawn("decor", 60, 20);                            // a type with no prefab row
    // per-instance properties, as the map editor's spawn inspector writes them
    d.set_prop(0, "range", "int", "30");                     // the player's own data
    d.set_prop(0, "Guard_range", "int", "99");              // overrides the prefab's Guard_range (30)
    d.spawns[1].name = "big_coin";
    d.set_prop(1, "w", "int", "12");                         // overrides the coin prefab's w (4)
    d.set_prop(2, "message", "string", "hello");             // a marker with text...
    d.set_prop(2, "collide", "bool", "false");               // ...and explicitly no collider
    if (!d.save_file("build/l_level.tmj")) return false;
    phxtool::BundleWriter w(2);
    for (const char* name : {"visit0", "visit1", "visit2", "visit3", "visit4", "visit5", "visit6", "visit7"})
        if (!phxtool::build_tmj(w, "build/l_level.tmj", name)) return false;
    return phxtool::build_sprite(w, "build/l_hero.sprdef", "hero") && phxtool::build_png(w, "build/l_tiles.png", "tiles") &&
           phxtool::build_tmj(w, "build/l_level.tmj", "level") && phxtool::build_bin(w, "build/l_prefabs.json", "prefabs") &&
           w.write("build/l_level.phxp");
}

struct LevelGame final : Game {
    ResourceCache* res = nullptr;
    PhysicsWorld   physics;
    AnimationSystem anim;
    Level          level;
    Hit            hits[16];
    Status         loaded = Status::NotFound;
    ecs::Entity    player = ecs::kInvalid, coin = ecs::kInvalid, decor = ecs::kInvalid;
    uint32_t       frame = 0, player_coin_hits = 0, drawn = 0;
    bool           landed = false;
    scalar         landed_y{};

    void on_start(App& app) override {
        res = ResourceCache::create(app.mem().persistent()).unwrap();
        check(res->mount(app.platform(), "build/l_level.phxp") == Status::Ok, "mount the baked level");
        loaded = level.load(app, *res, &physics);
        physics.set_gravity(vec2{ s_from_int(0), s_from_int(300) });
        player = level.find(app.world(), "player"_hash);
        coin   = level.find(app.world(), "coin"_hash);
        decor  = level.find(app.world(), "decor"_hash);
        ecs::World& w = app.world();
        // the map + collision grid (checked here: on_stop unloads the level)
        check(loaded == Status::Ok && level.spawned() == 3, "every spawn became an entity");
        check(level.width_px() == 64 && level.height_px() == 48 && level.grid().w == 8 &&
              level.grid().tiles == level.tilemap().indices + 8 * 6, "the collision grid is the LAST layer");
        check(level.prefabs().count() == 2 && level.prefabs().get_int(1, "value"_hash) == 5,
              "the game's own prefab columns stay readable");
        if (player != ecs::kInvalid) {
            const AABBColl* c = w.get<AABBColl>(player);
            const Animator* an = w.get<Animator>(player);
            const SpriteRenderer* sr = w.get<SpriteRenderer>(player);
            check(w.get<Transform>(player)->pos.x == s_from_int(20) && w.has<Body>(player), "player: Transform at the spawn + a Body");
            check(c && c->half.x == s_from_int(3) && c->half.y == s_from_int(4) && c->layer == 1 && c->mask == 2,
                  "player: collider from the prefab's w/h/layer/mask");
            check(sr && sr->tex != kNoTexture && an && an->clip == 1 && an->cur_sx == 2 && an->cur_sw == 2,
                  "player: sprite + Animator starting in 'idle' (frame 1 of the sheet)");
            check(w.get<PrefabRef>(player)->row == 0, "player: PrefabRef row 0");
        }
        if (coin != ecs::kInvalid) {
            const AABBColl* c = w.get<AABBColl>(coin);
            check(c && c->half.x == s_from_int(6) && c->half.y == s_from_int(2) && !w.has<Body>(coin) && !w.has<SpriteRenderer>(coin),
                  "coin: its spawn's w=12 overrides the prefab's 4 (h stays the prefab's), no sprite");
            const PrefabRef& ref = *w.get<PrefabRef>(coin);
            check(level.get_int(ref, "w"_hash) == 12 && level.get_int(ref, "value"_hash) == 5 && level.get_int(ref, "nope"_hash, -1) == -1,
                  "get_int: the spawn's property, else the prefab's column, else the default");
            check(level.name(ref) == "big_coin"_hash && level.find_named(w, "big_coin"_hash) == coin &&
                  level.find_named(w, "nobody"_hash) == ecs::kInvalid, "a spawn is found by its editor name");
        }
        if (player != ecs::kInvalid) {
            check(level.get_int(*w.get<PrefabRef>(player), "range"_hash, 0) == 30, "a spawn's own int property");
            const Guard* pt = w.get<Guard>(player);
            check(pt != nullptr, "the prefab's `components` column attaches the reflected Guard");
            if (pt) {
                check(pt->range == 99, "Guard_range: the spawn's property (99) beats the prefab's column (30)");
                check(pt->speed == s_from_q16(98304) && pt->target == "home"_hash,
                      "Guard_speed from an f32 column (1.5, exact on both tiers); Guard_target hashed from text");
                check(pt->angry, "a field with no column keeps its C++ default");
            }
        }
        if (coin != ecs::kInvalid) check(!w.has<Guard>(coin), "an unknown component name attaches nothing (a warning)");
        check(find_reflected("Guard"_hash) && find_reflected("Guard"_hash)->field_count == 4 && !find_reflected("Nope"_hash),
              "the registry knows Guard and its 4 fields");
        if (decor != ecs::kInvalid) {
            const PrefabRef& ref = *w.get<PrefabRef>(decor);
            check(ref.row == -1 && !w.has<AABBColl>(decor) && w.has<Transform>(decor),
                  "a type with no prefab row is still an entity; collide=false leaves it without a collider");
            const char* msg = level.get_str(ref, "message"_hash);
            check(msg && std::strcmp(msg, "hello") == 0 && level.get_hash(ref, "message"_hash) == "hello"_hash,
                  "a spawn's string property");
        }
    }
    void on_fixed_update(App& app, scalar dt) override {
        ecs::World& w = app.world();
        const uint32_t n = physics.step(w, dt, Span<Hit>{ hits, 16 });
        for (uint32_t i = 0; i < n; ++i)
            if ((hits[i].a == player && hits[i].b == coin) || (hits[i].a == coin && hits[i].b == player)) ++player_coin_hits;
        anim.tick(w, dt);
        if (!landed && player != ecs::kInvalid)
            if (const Body* b = w.get<Body>(player); b && b->on_ground) { landed = true; landed_y = w.get<Transform>(player)->pos.y; }
        if (++frame == 60 && player != ecs::kInvalid && coin != ecs::kInvalid)   // put the player on the coin
            w.get<Transform>(player)->pos = w.get<Transform>(coin)->pos;
    }
    void on_render(App& app, scalar) override {
        Renderer& r = app.render();
        r.begin_frame(Camera2D{});
        level.draw(r);
        draw_sprites(app.world(), r);
        r.end_frame();
        drawn = r.stats().sprites_submitted;
    }
    void on_stop(App& app) override {
        ecs::World& w = app.world();
        const uint32_t before = w.count();
        level.unload(app);
        check(before == 3 && w.count() == 0 && !level.loaded(), "unload despawns the level's entities");
        Renderer& r = app.render();
        const uint16_t resident = r.live_textures();
        LevelOptions bad{}; bad.tileset = "missing"_hash;
        check(level.load(app, *res, &physics, bad) == Status::OutOfMemory && !level.loaded() &&
              r.live_textures() == resident && w.count() == 0, "failed tileset upload leaves no map or texture ownership");
        check(level.load(app, *res, &physics) == Status::Ok &&
              level.load(app, *res, &physics) == Status::Ok && w.count() == 3,
              "load on a loaded Level replaces entities and releases old sprite ownership");
        level.unload(app);
        static const NameHash maps[] = {"visit0"_hash, "visit1"_hash, "visit2"_hash, "visit3"_hash,
                                       "visit4"_hash, "visit5"_hash, "visit6"_hash, "visit7"_hash};
        LevelOptions opt{}; opt.prefabs = 0;   // test retained-map capacity without sprite uploads
        bool supported = true;
        for (int i = 0; i < 7; ++i) {
            opt.map = maps[i];
            supported = supported && level.load(app, *res, &physics, opt) == Status::Ok;
            level.unload(app);
        }
        check(supported, "eight supported map pairs are retained for revisits");
        const uint16_t eight = r.live_textures();
        opt.map = maps[7];
        check(level.load(app, *res, &physics, opt) == Status::OutOfMemory && !level.loaded() &&
              r.live_textures() == eight && w.count() == 0, "cache overflow fails before any resource allocation");
        opt.map = maps[0];
        check(level.load(app, *res, &physics, opt) == Status::Ok, "supported cached map remains revisitable after overflow");
        level.unload(app);
        TilemapDesc md{};   // fill the backend's remaining retained slots
        static uint16_t cell = 0;
        md.indices = &cell; md.width = md.height = 1;
        // Use a real live tileset (soft backend accepts empty too, PPU requires live).
        TextureDesc td{}; static uint32_t texels[64] = {};
        td.pixels = texels; td.width = td.height = 8;
        md.tileset = r.load_texture(td);
        for (int i = 0; i < 32; ++i) r.upload_tilemap(md);
        const uint16_t before_fail = r.live_textures();
        Level other;
        check(other.load(app, *res, &physics, opt) == Status::OutOfMemory && !other.loaded() &&
              r.live_textures() == before_fail, "backend map exhaustion reclaims its failed Level tileset upload");

    }
};
} // namespace

int main() {
    check(bake(), "bake map + prefabs + sprite with the real converters");

    phx_null_set_max_frames(120);
    Config cfg = Config::from_defaults();
    cfg.title = "level_test"; cfg.width = 64; cfg.height = 48;
    cfg.total_ram = 8u << 20; cfg.frame_scratch = 256u << 10; cfg.max_entities = 64;
    App app(cfg);
    static LevelGame g;
    check(app.run(&g) == 0, "the App runs the level");

    // what the run saw (the per-entity components are checked in on_start, on the live world)
    check(g.player != ecs::kInvalid && g.coin != ecs::kInvalid && g.decor != ecs::kInvalid, "find() by type");
    check(g.landed, "the player's Body fell under gravity and landed on the authored floor");
    // floor top = row 5 * 8 = 40; the collider is 8 high -> centre 36
    check(s_to_int(g.landed_y) == 36, "it rests exactly on the floor (collider from the prefab's h)");
    check(g.player_coin_hits > 0, "player/coin overlap reports (layer/mask from the prefab)");
    check(g.drawn >= 1, "the player's sprite draws");

    std::printf("level_test: %d checks, %d failures\n", g_checks, g_fail);
    std::printf(g_fail ? "LEVEL FAIL\n\n" : "LEVEL PASS\n\n");
    return g_fail ? 1 : 0;
}
