// engine/runtime/src/level.cpp — see phx/runtime/level.h. Linked by game projects (the generic
// `make game*` rules) and the level suite, not by every App build: it needs the resource cache.
#include "phx/runtime/level.h"
#include "phx/runtime/app.h"
#include "phx/core/log.h"

#include <cstddef>
#include <cstring>

namespace phx {

// Level runs an Animator on a sprite's baked transitions in place: the two layouts must agree.
static_assert(sizeof(AnimEdge) == sizeof(SpriteTransDef) && offsetof(AnimEdge, from) == offsetof(SpriteTransDef, from) &&
              offsetof(AnimEdge, to) == offsetof(SpriteTransDef, to) &&
              offsetof(AnimEdge, trigger) == offsetof(SpriteTransDef, trigger) && kAnyClip == kSpriteTransAny,
              "AnimEdge and the baked SpriteTransDef must share a layout");

namespace {


TextureId upload(ResourceCache& res, Renderer& r, NameHash name, uint16_t* w = nullptr, uint16_t* h = nullptr) {
    auto t = res.texture(name);
    if (!t) return kNoTexture;
    const TextureView v = t.unwrap();
    TextureDesc d{};
    d.pixels = v.pixels; d.width = v.width; d.height = v.height; d.format = v.format;
    if (w) *w = v.width;
    if (h) *h = v.height;
    return r.load_texture(d);
}

int32_t clampi(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

} // namespace

const Level::SpriteSlot* Level::sprite(ResourceCache& res, Renderer& r, NameHash name) {
    for (uint32_t i = 0; i < sprite_count_; ++i)
        if (sprites_[i].name == name) return sprites_[i].tex != kNoTexture ? &sprites_[i] : nullptr;
    if (sprite_count_ == kMaxSprites) {
        PHX_LOG_WARN("level: more than %u sprite assets; the rest are not drawn", unsigned(kMaxSprites));
        return nullptr;
    }
    SpriteSlot& s = sprites_[sprite_count_++];
    s = SpriteSlot{};
    s.name = name;
    if (res.has(name, AssetType::Sprite)) {                 // a sprite: frames + named clips
        const SpriteView v = res.sprite(name).unwrap();
        s.tex = upload(res, r, v.texture);
        s.sheet = SpriteSheet{ v.frame_w, v.frame_h, v.cols ? v.cols : uint16_t(1) };
        s.w = int16_t(v.frame_w); s.h = int16_t(v.frame_h);
        for (uint16_t i = 0; i < v.clip_count && s.clip_count < kMaxClips; ++i, ++s.clip_count) {
            const SpriteClipDef& c = v.clips[i];
            s.clips[s.clip_count] = AnimClip{ c.first, c.count, c.fps, c.loop != 0 };
            s.clip_names[s.clip_count] = c.name;
        }
        if (v.clip_count > kMaxClips)
            PHX_LOG_WARN("level: sprite %08x has %u clips; the first %u play", unsigned(name), unsigned(v.clip_count),
                         unsigned(kMaxClips));
        // its state machine, read in place: the baked SpriteTransDef IS an AnimEdge (an edge into a
        // clip past kMaxClips is harmless: the animator keeps its last frame)
        if (v.trans_count) s.edges = Span<const AnimEdge>{ reinterpret_cast<const AnimEdge*>(v.trans), v.trans_count };
    } else {                                                // a plain texture: one frame
        uint16_t w = 0, h = 0;
        s.tex = upload(res, r, name, &w, &h);
        s.sheet = SpriteSheet{ w, h, 1 };
        s.w = int16_t(w); s.h = int16_t(h);
    }
    if (s.tex == kNoTexture) PHX_LOG_WARN("level: sprite %08x is neither a sprite nor a texture", unsigned(name));
    return s.tex != kNoTexture ? &s : nullptr;
}

Status Level::load(App& app, ResourceCache& res, PhysicsWorld* physics, const LevelOptions& opt) {
    auto tm = res.tilemap(opt.map);
    if (!tm) { PHX_LOG_ERROR("level: no tilemap %08x in the mounted bundles", unsigned(opt.map)); return Status::NotFound; }
    if (loaded()) unload(app);
    if (physics) physics->set_tilemap(TileGrid{});
    Renderer& r = app.render();
    ecs::World& w = app.world();
    view_ = tm.unwrap();

    // The map: backdrops first, the gameplay layer last; parallax as authored. A map this Level
    // uploaded before reuses its slot and tileset (the cache), so revisiting a level costs none.
    const NameHash texture_name = opt.tileset ? opt.tileset : view_.tileset;
    map_ = kNoTilemap;
    for (uint32_t i = 0; i < cache_n_; ++i)
        if (cache_[i].map == opt.map && cache_[i].texture == texture_name) { map_ = cache_[i].id; tileset_ = cache_[i].tileset; }
    if (map_ == kNoTilemap) {
        // Retained maps are renderer-owned for its arena lifetime. Bound the supported
        // revisit set before allocating anything; never leave an uncached map with freed tiles.
        if (cache_n_ == kMapCache) { view_ = TilemapView{}; return Status::OutOfMemory; }
        tileset_ = upload(res, r, texture_name);
        if (tileset_ == kNoTexture) { view_ = TilemapView{}; return Status::OutOfMemory; }
        TilemapDesc d{};
        d.indices = view_.indices; d.width = view_.width; d.height = view_.height; d.layers = view_.layers;
        d.tile_w = view_.tile_w; d.tile_h = view_.tile_h; d.tileset = tileset_;
        map_ = r.upload_tilemap(d);
        if (map_ == kNoTilemap) {
            r.unload_texture(tileset_);
            tileset_ = kNoTexture; view_ = TilemapView{};
            return Status::OutOfMemory;
        }
        cache_[cache_n_++] = MapCache{ opt.map, texture_name, map_, tileset_ };
    }
    if (view_.parallax_q16)
        for (uint8_t l = 0; l < view_.layers; ++l)
            r.set_tilemap_parallax(map_, l, s_from_q16(view_.parallax_q16[l * 2 + 0]),
                                   s_from_q16(view_.parallax_q16[l * 2 + 1]));

    // Collision: the LAST tile layer, with the per-tile flags the map editor authored.
    grid_ = TileGrid{};
    grid_.tiles = view_.indices + size_t(view_.layers - 1) * view_.width * view_.height;
    grid_.w = view_.width; grid_.h = view_.height;
    grid_.tile_w = view_.tile_w; grid_.tile_h = view_.tile_h; grid_.solid_from = 1;
    grid_.flags = view_.tile_flags; grid_.flag_count = view_.tile_flag_count;
    if (physics) physics->set_tilemap(grid_);

    // Entities: every spawn, built from its prefab row.
    if (!TableView::load(res, opt.prefabs, prefabs_)) prefabs_ = TableView{};
    spawned_ = 0;
    auto sp = res.spawns(opt.map);
    spawns_ = sp ? sp.unwrap() : SpawnsView{};
    for (uint32_t i = 0; i < spawns_.count; ++i) {
        const SpawnDef& s = spawns_.spawns[i];
        const int32_t row = prefabs_.find("type"_hash, s.type);
        if (w.count() >= w.capacity()) { PHX_LOG_ERROR("level: the World is full (raise Config::max_entities)"); break; }
        const ecs::Entity e = w.spawn();
        ++spawned_;
        const vec2 centre{ s_from_int(s.x + s.w / 2), s_from_int(s.y + s.h / 2) };
        w.add<Transform>(e, Transform{ centre });
        const PrefabRef ref{ s.type, row, uint16_t(i) };
        w.add<PrefabRef>(e, ref);

        // Built from its prefab row, with this spawn's own properties overriding the columns. A
        // spawn with neither a row nor properties is a bare marker (Transform + PrefabRef).
        bool described = row >= 0;
        for (uint32_t p = 0; !described && p < spawns_.prop_count; ++p) {
            SpawnPropDef d;
            std::memcpy(&d, spawns_.props + size_t(p) * sizeof(SpawnPropDef), sizeof(d));
            described = d.spawn == i;
        }
        if (described) {
            int32_t fw = s.w ? s.w : 8, fh = s.h ? s.h : 8;   // the collider's default size
            if (const NameHash spr = get_hash(ref, "sprite"_hash)) {
                if (const SpriteSlot* ss = sprite(res, r, spr)) {
                    fw = ss->w; fh = ss->h;
                    SpriteRenderer sr{};
                    sr.tex = ss->tex; sr.sw = ss->w; sr.sh = ss->h;
                    sr.clip_names = ss->clip_names; sr.clip_count = ss->clip_count;
                    sr.layer = uint8_t(clampi(get_int(ref, "z"_hash, 10), 0, 255));
                    w.add<SpriteRenderer>(e, sr);
                    if (ss->clip_count) {
                        Animator an{};
                        an.clips = Span<const AnimClip>{ ss->clips, ss->clip_count };
                        an.edges = ss->edges;
                        an.sheet = ss->sheet;
                        const NameHash want = get_hash(ref, "clip"_hash);
                        uint16_t start = 0;
                        for (uint16_t c = 0; c < ss->clip_count; ++c)
                            if (ss->clip_names[c] == (want ? want : "idle"_hash)) start = c;
                        an.play(start);
                        AnimationSystem::apply_rect(an);
                        w.add<Animator>(e, an);
                    }
                } else {
                    unload(app);
                    if (physics) physics->set_tilemap(TileGrid{});
                    return Status::OutOfMemory;
                }
            }
            fw = clampi(get_int(ref, "w"_hash, fw), 0, 4096);
            fh = clampi(get_int(ref, "h"_hash, fh), 0, 4096);
            if (get_int(ref, "collide"_hash, 1)) {
                AABBColl c{};
                c.half  = vec2{ s_from_q16(fw * 32768), s_from_q16(fh * 32768) };   // w/2, exact on both tiers
                c.layer = uint16_t(get_int(ref, "layer"_hash, 1));
                c.mask  = uint16_t(get_int(ref, "mask"_hash, 0xFFFF));
                w.add<AABBColl>(e, c);
            }
            if (get_int(ref, "body"_hash, 0)) w.add<Body>(e, Body{});
            if (const char* list = get_str(ref, "components"_hash)) attach_components(w, e, ref, list);
        }
        if (opt.on_spawn) opt.on_spawn(opt.user, w, e, s, prefabs_, row);
    }
    level_detail::g_active = this;
    PHX_LOG_INFO("level: %ux%u tiles, %u layer(s), %u entities (%u prefab types)", unsigned(view_.width),
                 unsigned(view_.height), unsigned(view_.layers), unsigned(spawned_), unsigned(prefabs_.count()));
    return Status::Ok;
}

void Level::unload(App& app) {
    ecs::World& w = app.world();
    w.each<PrefabRef>([&](ecs::Entity e, PrefabRef&) { w.defer().despawn(e); });
    w.flush_deferred();
    for (uint32_t i = 0; i < sprite_count_; ++i)
        if (sprites_[i].tex != kNoTexture) app.render().unload_texture(sprites_[i].tex);
    if (level_detail::g_active == this) level_detail::g_active = nullptr;
    sprite_count_ = 0; spawned_ = 0; tileset_ = kNoTexture; map_ = kNoTilemap;
    view_ = TilemapView{}; grid_ = TileGrid{}; prefabs_ = TableView{}; spawns_ = SpawnsView{};
}

namespace {
// fnv1a(a + "_" + b) without building the string: the column a reflected field is authored in.
NameHash hash_column(const char* a, size_t an, const char* b) {
    NameHash h = kFnvOffset;
    for (size_t i = 0; i < an; ++i) h = (h ^ NameHash(uint8_t(a[i]))) * kFnvPrime;
    h = (h ^ NameHash(uint8_t('_'))) * kFnvPrime;
    for (; *b; ++b) h = (h ^ NameHash(uint8_t(*b))) * kFnvPrime;
    return h;
}
} // namespace

// Attach the reflected components named in `list` ("Enemy Coin", spaces or commas) and fill each
// field from its `Comp_field` setting (the spawn's, else the prefab's); absent ones keep the C++
// default the component was constructed with.
void Level::attach_components(ecs::World& w, ecs::Entity e, const PrefabRef& ref, const char* list) {
    for (const char* p = list; *p;) {
        while (*p == ' ' || *p == ',' || *p == '\t') ++p;
        const char* s = p;
        while (*p && *p != ' ' && *p != ',' && *p != '\t') ++p;
        if (p == s) break;
        const NameHash name = fnv1a_n(s, size_t(p - s));
        const ComponentInfo* c = find_reflected(name);
        if (!c) { PHX_LOG_WARN("level: no component named '%.*s' (declare it with PHX_COMPONENT)", int(p - s), s); continue; }
        uint8_t* data = static_cast<uint8_t*>(c->add(w, e));
        for (uint8_t f = 0; data && f < c->field_count; ++f) {
            const FieldInfo& fi = c->fields[f];
            const NameHash key = hash_column(s, size_t(p - s), fi.name);
            uint8_t* at = data + fi.offset;
            if (fi.type == FieldType::Scalar) {
                int32_t q;
                if (get_q16(ref, key, q)) { const scalar v = s_from_q16(q); std::memcpy(at, &v, sizeof(v)); }
                continue;
            }
            if (fi.type == FieldType::Hash) {
                if (const NameHash h = get_hash(ref, key)) std::memcpy(at, &h, sizeof(h));
                continue;
            }
            if (!spawns_.has(ref.spawn, key) && !prefabs_.has(key)) continue;   // keep the default
            const int32_t v = get_int(ref, key, 0);
            switch (fi.type) {
                case FieldType::I8:   { const int8_t x = int8_t(v);     std::memcpy(at, &x, 1); break; }
                case FieldType::U8:   { const uint8_t x = uint8_t(v);   std::memcpy(at, &x, 1); break; }
                case FieldType::I16:  { const int16_t x = int16_t(v);   std::memcpy(at, &x, 2); break; }
                case FieldType::U16:  { const uint16_t x = uint16_t(v); std::memcpy(at, &x, 2); break; }
                case FieldType::I32:  std::memcpy(at, &v, 4); break;
                case FieldType::U32:  { const uint32_t x = uint32_t(v); std::memcpy(at, &x, 4); break; }
                case FieldType::Bool: { const bool x = v != 0;          std::memcpy(at, &x, sizeof(x)); break; }
                default: break;
            }
        }
    }
}

bool Level::get_q16(const PrefabRef& ref, NameHash key, int32_t& out) const {
    if (spawns_.get_q16(ref.spawn, key, out)) return true;
    return prefabs_.get_q16(ref.row, key, out);
}

int32_t Level::get_int(const PrefabRef& ref, NameHash key, int32_t def) const {
    SpawnPropDef d;
    if (spawns_.prop(ref.spawn, key, d) && d.type != kPropStr) return spawns_.get_int(ref.spawn, key, def);
    return prefabs_.get_int(ref.row, key, def);
}

const char* Level::get_str(const PrefabRef& ref, NameHash key) const {
    if (const char* s = spawns_.get_str(ref.spawn, key)) return s;
    return prefabs_.get_str(ref.row, key);
}

NameHash Level::get_hash(const PrefabRef& ref, NameHash key) const {
    if (spawns_.has(ref.spawn, key)) return spawns_.get_hash(ref.spawn, key);
    return prefabs_.get_hash(ref.row, key);
}

ecs::Entity Level::find_named(ecs::World& w, NameHash name) const {
    const int32_t idx = spawns_.find_named(name);
    ecs::Entity found = ecs::kInvalid;
    if (idx < 0) return found;
    w.each<PrefabRef>([&](ecs::Entity e, PrefabRef& p) { if (p.spawn == uint16_t(idx)) found = e; });
    return found;
}

void Level::draw(Renderer& r) const {
    if (map_ == kNoTilemap) return;
    for (uint8_t l = 0; l < view_.layers; ++l) r.draw_tilemap(map_, l);
}

ecs::Entity Level::find(ecs::World& w, NameHash type) const {
    ecs::Entity found = ecs::kInvalid;
    uint16_t first = 0xFFFF;
    w.each<PrefabRef>([&](ecs::Entity e, PrefabRef& p) {
        if (p.type == type && p.spawn < first) { first = p.spawn; found = e; }
    });
    return found;
}

void draw_sprites(ecs::World& w, Renderer& r) {
    w.each<SpriteRenderer, Transform>([&](ecs::Entity e, SpriteRenderer& s, Transform& t) {
        DrawSprite ds{};
        ds.tex = s.tex; ds.sx = s.sx; ds.sy = s.sy; ds.sw = s.sw; ds.sh = s.sh;
        if (const Animator* an = w.get<Animator>(e)) {
            ds.sx = an->cur_sx; ds.sy = an->cur_sy; ds.sw = an->cur_sw; ds.sh = an->cur_sh;
        }
        if (ds.tex == kNoTexture || ds.sw <= 0 || ds.sh <= 0) return;
        ds.pos = vec2{ t.pos.x - s_from_int(ds.sw / 2), t.pos.y - s_from_int(ds.sh / 2) };
        ds.flags = s.flags; ds.layer = s.layer;
        r.draw_sprite(ds);
    });
}

} // namespace phx
