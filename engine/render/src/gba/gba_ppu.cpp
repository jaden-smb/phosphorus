// engine/render/src/gba/gba_ppu.cpp — the GBA-native render backend (render tier 0, PPU).
// Instead of CPU-rasterizing arbitrary 24-bit sprites like the software reference, this
// backend speaks the GBA's actual hardware language:
//
//   · RGBA8 atlases are quantized into 4bpp tiles indexing 16-colour PALETTE BANKS
//     (16 banks of 16, like PALRAM) — a whole texture shares one bank when its colours
//     fit (required for OBJ use), otherwise each 8×8 tile gets its own bank (fine for
//     BG tilesets). A single tile needing >15 opaque colours, or exhausting all banks,
//     fails the upload — exactly what would make the art unshippable on hardware.
//   · Tilemaps of ANY size are drawn by STREAMING the camera-visible window into one of
//     the PPU's four 32×32-cell text backgrounds per draw call (slot 0 = furthest back),
//     with the raw scroll in HOFS/VOFS — so a 320-tile level and per-layer parallax cost
//     the same as the GBA's free BG scrolling, which is the whole point of the machine.
//   · Sprites become OAM entries whose tiles are copied per frame into a CONTIGUOUS
//     1D-mapped OBJ tile run (atlas sub-rects are scattered in the char store; OAM's 1D
//     mapping cannot address that, so the visible sprites' tiles are re-packed each frame
//     — bounded, ~40 tiles/frame in practice). Shapes the hardware has no OBJ size for
//     are rejected in EVERY build, keeping the host model an honest golden oracle.
//
// The whole model is fully exercised and verified HEADLESSLY: `ppu_compose`
// (engine/render/src/gba/ppu_model.h) produces the exact frame the PPU would scan out.
// On real hardware (PHX_GBA_HW) end() pushes the same data into VRAM/OAM/PALRAM — tiles
// and palette incrementally (including reclaimed runs at load time), windows/OAM/OBJ tiles per
// frame — and the silicon rasterizes it.
//
// Exactly one render backend TU is linked; this one defines phx_make_render_backend()
// (selected at link time instead of soft/gl/gu).
#include "backend.h"
#include "phx/platform/gfx_soft.h"
#include "ppu_model.h"
#include <cstring>

#if defined(PHX_GBA_HW)
#include <stdint.h>
// Platform hook (engine/platform/src/gba): take over the display so present() skips the Mode-3
// software blit and lets the PPU scan our VRAM/OAM/PALRAM out directly.
extern "C" void phx_gba_set_direct(int on);
#endif

namespace phx {
namespace {

using namespace phx::gba;

constexpr uint16_t kMaxTextures = 256;
constexpr uint16_t kMaxTilemaps = 32;
// BG char-store capacity. 512 4bpp tiles = 16 KB — half of BG charblocks 0–1, well clear of
// the four screenblocks at blocks 24–27 (tile index 768+), and within the 10-bit screen-entry
// tile field. The CPU-side store is the same packed 4bpp layout VRAM wants (32 B/tile).
constexpr uint16_t kTileCap    = 512;
// Per-frame OBJ tile run: 128 OBJs × a few tiles each in practice; 256 tiles (8 KB) is a
// quarter of OBJ char VRAM. Sprites beyond it are dropped and counted (graceful overflow).
constexpr uint16_t kObjTileCap = 256;
// Rows streamed per BG window: the screen samples at most 21 (160/8 + 1); stream 22 so fine
// scroll never reads a stale edge. Columns always stream the full 32-cell window width.
constexpr int      kWinRows    = kScreenH / kTile + 2;
constexpr uint8_t  kBankNone   = 0xFF;   // TexRec.bank: per-tile banks (BG-only texture)

// A quantized texture: a contiguous run of tiles in the char store, addressed in 2D as a
// `cols` x `rows` grid (so a sprite/tile source sub-rect maps to a tile rectangle).
// `bank` is the single palette bank the whole texture indexes, or kBankNone when its
// colours spilled across per-tile banks (then tile_bank_[] holds each tile's bank).
struct TexRec {
    uint16_t base = 0, cols = 0, rows = 0;
    bool used = false, owned = false;
    uint8_t bank = kBankNone;
    TextureId handle = 0;
};
struct MapRec {
    const uint16_t* idx;
    uint16_t w, h;
    uint8_t  layers, tw, th;
    TextureId tileset;
    int32_t  scroll_x, scroll_y;
};

// Floor division/modulo for tile coordinates (scroll can be negative in principle).
inline int floor_div(int a, int b) { int q = a / b; return (a % b != 0 && (a ^ b) < 0) ? q - 1 : q; }

// Map a w_tiles x h_tiles OBJ to a hardware (shape,size). Returns false if the GBA has no
// such OBJ shape — those sprites are rejected in every build (honest hardware limit).
bool obj_shape_size(uint8_t w, uint8_t h, uint16_t& shape, uint16_t& size) {
    struct E { uint8_t w, h, shape, size; };
    static const E kT[] = {
        {1,1,0,0},{2,2,0,1},{4,4,0,2},{8,8,0,3},   // square
        {2,1,1,0},{4,1,1,1},{4,2,1,2},{8,4,1,3},   // horizontal
        {1,2,2,0},{1,4,2,1},{2,4,2,2},{4,8,2,3},   // vertical
    };
    for (const E& e : kT) if (e.w == w && e.h == h) { shape = e.shape; size = e.size; return true; }
    return false;
}

class GbaPpuBackend final : public IRenderBackend {
public:
    // False when the arena can't hold the backend's stores (Config::total_ram too small): the
    // renderer then fails to create with an error instead of writing through a null store.
    bool init(phx_gfx* gfx, ArenaAllocator& a, const Caps& caps) {
        gfx_       = gfx;
        tiles_     = a.alloc_array<PpuTile>(kTileCap);
        tile_bank_ = a.alloc_array<uint8_t>(kTileCap);
        obj_tiles_ = a.alloc_array<PpuTile>(kObjTileCap);
        tex_       = a.alloc_array<TexRec>(kMaxTextures);
        maps_      = a.alloc_array<MapRec>(kMaxTilemaps);
        for (int s = 0; s < kBgSlots; ++s)
            win_[s] = a.alloc_array<PpuScreenEntry>(kBgWin * kBgWin);
        oam_       = a.alloc_array<PpuObj>(kObjMax);
#if !defined(PHX_GBA_HW)
        scratch_ = a.alloc_array<uint32_t>(kScreenW * kScreenH);   // compose target (host/sw tier)
        if (!scratch_) return false;
#endif
        if (!tiles_ || !tile_bank_ || !obj_tiles_ || !tex_ || !maps_ || !oam_) return false;
        for (int s = 0; s < kBgSlots; ++s)
            if (!win_[s]) return false;

        for (uint16_t i = 0; i < kMaxTextures; ++i) tex_[i] = TexRec{};

        // Tile 0 is the reserved fully-transparent tile (BG "empty" cells point here).
        for (auto& r : tiles_[0].row) r = 0;
        tile_bank_[0] = 0;
        tile_count_ = 1;

        // Palette bank 0, index 0 is the backdrop AND every bank's transparent slot. Match
        // the software reference's clear colour so headless golden images line up.
        for (auto& p : palette_) p = 0;
        palette_[0] = rgba8_to_bgr555(rgba(30, 30, 46));

        // The hardware OBJ ceiling is 128; respect a tighter caps budget if one is set.
        obj_limit_ = caps.max_sprites && caps.max_sprites < kObjMax ? uint16_t(caps.max_sprites)
                                                                    : kObjMax;
#if defined(PHX_GBA_HW)
        phx_gba_set_direct(1);   // we own the display; the platform must not Mode-3-blit over VRAM
#endif
        return true;
    }

    // Accepts the PPU's two texture encodings and fails (kNoTexture) on the honest GBA
    // constraints (non-8px-aligned, char-store full, >15 opaque colours in one tile, all
    // 16 banks exhausted):
    //   · PAL4_TILES (the tier-0 bake, docs/06 §4): tiles arrive pre-quantized — upload just
    //     claims a palette bank per baked palette and remaps nibbles through a 16-entry LUT.
    //   · RGBA8 (tests, tier-2 bundles, un-bakeable art): the full upload-time quantizer.
    //     Both paths produce the same composed frame for the same source art.
    // Palette mutations are transactional. Tiles are written only into a free run and
    // become live on commit, so a failed encode needs no tile rollback.
    TextureId upload_tex(const TextureDesc& d) override {
        uint16_t saved_palette[kPalTotal], saved_used[kPalBanks];
        std::memcpy(saved_palette, palette_, sizeof(palette_));
        std::memcpy(saved_used, bank_used_, sizeof(bank_used_));
        const uint32_t stamp = pal_stamp_;
        const TextureId id = upload_impl(d);
        if (id == kNoTexture) {
            std::memcpy(palette_, saved_palette, sizeof(palette_));
            std::memcpy(bank_used_, saved_used, sizeof(bank_used_));
            pal_stamp_ = stamp;
        }
        return id;
    }

    TextureId upload_impl(const TextureDesc& d) {
        if (!d.pixels || d.width == 0 || d.height == 0)       return kNoTexture;
        if ((d.width % kTile) || (d.height % kTile))          return kNoTexture;  // tile-align
        if (d.format == PixelFormat::PAL4_TILES)              return upload_pal4(d);
        if (d.format != PixelFormat::RGBA8)                   return kNoTexture;
        const uint16_t cols = uint16_t(d.width  / kTile);
        const uint16_t rows = uint16_t(d.height / kTile);
        const uint32_t need = uint32_t(cols) * rows;
        const uint16_t base = find_run(need);
        const int slot = find_slot();
        if (base == 0 || slot < 0) return kNoTexture;

        const uint32_t* src = static_cast<const uint32_t*>(d.pixels);

        // Pass 1: gather the texture's unique opaque colours. If they fit one bank the whole
        // texture shares it (a requirement for OBJ use, where one OAM entry = one bank);
        // otherwise fall back to per-tile banks (fine for BG tilesets).
        uint16_t uniq[kPalSize]; int uniq_n = 0;
        bool one_bank = true;
        for (uint32_t i = 0; i < uint32_t(d.width) * d.height && one_bank; ++i) {
            if ((src[i] >> 24) == 0) continue;
            const uint16_t c = rgba8_to_bgr555(src[i]);
            bool found = false;
            for (int k = 0; k < uniq_n; ++k) if (uniq[k] == c) { found = true; break; }
            if (found) continue;
            if (uniq_n >= kPalSize - 1) one_bank = false;      // >15: spill to per-tile banks
            else uniq[uniq_n++] = c;
        }
        int tex_bank = -1;
        if (one_bank) {
            tex_bank = claim_bank(uniq, uniq_n);
            if (tex_bank < 0) return kNoTexture;               // all 16 banks exhausted
        }

        // Pass 2: encode each 8×8 tile (per-tile bank claim when the texture spilled).
        for (uint16_t tr = 0; tr < rows; ++tr) {
            for (uint16_t tc = 0; tc < cols; ++tc) {
                PpuTile& t = tiles_[base + tr * cols + tc];
                int bank = tex_bank;
                if (bank < 0) {                                // per-tile: gather + claim
                    uint16_t tu[kPalSize]; int tn = 0;
                    for (int y = 0; y < kTile; ++y)
                        for (int x = 0; x < kTile; ++x) {
                            const uint32_t texel = src[(tr * kTile + y) * d.width + (tc * kTile + x)];
                            if ((texel >> 24) == 0) continue;
                            const uint16_t c = rgba8_to_bgr555(texel);
                            bool found = false;
                            for (int k = 0; k < tn; ++k) if (tu[k] == c) { found = true; break; }
                            if (found) continue;
                            if (tn >= kPalSize - 1) return kNoTexture;   // >15 colours in ONE tile
                            tu[tn++] = c;
                        }
                    bank = claim_bank(tu, tn);
                    if (bank < 0) return kNoTexture;
                }
                for (int y = 0; y < kTile; ++y) {
                    t.row[y] = 0;
                    for (int x = 0; x < kTile; ++x) {
                        const uint32_t texel = src[(tr * kTile + y) * d.width + (tc * kTile + x)];
                        if ((texel >> 24) == 0) continue;      // alpha 0 -> nibble 0
                        ppu_tile_set(t, x, y, bank_slot(uint8_t(bank), rgba8_to_bgr555(texel)));
                    }
                }
                tile_bank_[base + tr * cols + tc] = uint8_t(bank);
            }
        }
        return commit(slot, base, cols, rows, tex_bank >= 0 ? uint8_t(tex_bank) : kBankNone);
    }

    bool free_tex(TextureId id) override {
        TexRec* t = texture(id);
        if (!t || !t->owned) return false;
        t->owned = false;
        // Maps belong to the renderer until its arena is destroyed. Pin their tilesets:
        // releasing the uploader's ownership must never leave a map pointing at reused tiles.
        for (uint16_t m = 0; m < map_n_; ++m)
            if (maps_[m].tileset == id) return false;
        t->used = false;
        reclaim_palette();
        return true;
    }

    // The PAL4_TILES fast path: the bake already ran this backend's quantizer offline
    // (tools/phxpack/tex_encode.h mirrors it exactly), so upload is claim-banks + LUT-remap
    // — no per-texel colour scan on the 16 MHz CPU, and the ROM carries 4bpp, not RGBA8.
    TextureId upload_pal4(const TextureDesc& d) {
        // Blob payloads are 8-aligned (16-aligned blob start + the 8-byte blob header), so
        // header/palette casts are safe on ARM; the tile words are 4-aligned by the format.
        const uint8_t* pay = static_cast<const uint8_t*>(d.pixels);
        if (d.size && d.size < sizeof(TexturePal4Header)) return kNoTexture;
        const TexturePal4Header& ph = *reinterpret_cast<const TexturePal4Header*>(pay);
        const uint16_t cols = uint16_t(d.width  / kTile);
        const uint16_t rows = uint16_t(d.height / kTile);
        const uint32_t need = uint32_t(cols) * rows;
        if (ph.pal_count == 0 || ph.pal_count > kPalBanks)    return kNoTexture;
        const uint16_t base = find_run(need);
        const int slot = find_slot();
        if (base == 0 || slot < 0) return kNoTexture;

        if (d.size && d.size < pal4_payload_size(ph.pal_count, need)) return kNoTexture;

        const uint16_t* pals  = reinterpret_cast<const uint16_t*>(pay + pal4_palettes_off());
        const uint8_t*  used  = pay + pal4_pal_used_off(ph.pal_count);
        const uint8_t*  tpal  = pay + pal4_tile_pal_off(ph.pal_count);
        const uint32_t* tdata = reinterpret_cast<const uint32_t*>(
                                    pay + pal4_tile_data_off(ph.pal_count, need));

        // One global bank per baked palette; LUT = baked slot -> global bank slot. When the
        // palette lands in a fresh bank the LUT is the identity and the remap is a straight copy.
        uint8_t gbank[kPalBanks];
        uint8_t lut[kPalBanks][kPalSize] = {};
        for (uint16_t p = 0; p < ph.pal_count; ++p) {
            if (used[p] > kPalSize - 1) return kNoTexture;
            const int n = used[p];
            uint16_t colors[kPalSize - 1];
            for (int k = 0; k < n; ++k) colors[k] = pals[p * kPalSize + 1 + k];
            const int b = claim_bank(colors, n);
            if (b < 0) return kNoTexture;                     // all 16 banks exhausted
            gbank[p] = uint8_t(b);
            for (int s = 1; s <= n; ++s) lut[p][s] = bank_slot(uint8_t(b), pals[p * kPalSize + s]);
        }

        for (uint32_t t = 0; t < need; ++t) {
            const uint8_t p = tpal[t];
            if (p >= ph.pal_count) return kNoTexture;         // malformed payload
            PpuTile& tile = tiles_[base + t];
            for (int r = 0; r < kTile; ++r) {
                const uint32_t in = tdata[t * 8 + r];
                uint32_t out = 0;
                for (int x = 0; x < kTile; ++x) {
                    const uint8_t index = uint8_t((in >> (x * 4)) & 0xF);
                    if (index > used[p]) return kNoTexture;
                    out |= uint32_t(lut[p][index]) << (x * 4);
                }
                tile.row[r] = out;
            }
            tile_bank_[base + t] = gbank[p];
        }
        return commit(slot, base, cols, rows, ph.pal_count == 1 ? gbank[0] : kBankNone);
    }

    TilemapId upload_map(const TilemapDesc& d) override {
        if (!d.indices || !d.width || !d.height || !d.layers || !texture(d.tileset)) return kNoTilemap;
        // Identical retained map descriptors share one slot.
        for (uint16_t m = 0; m < map_n_; ++m) {
            const MapRec& r = maps_[m];
            if (r.idx == d.indices && r.w == d.width && r.h == d.height && r.layers == d.layers &&
                r.tw == d.tile_w && r.th == d.tile_h && r.tileset == d.tileset) return m;
        }
        if (map_n_ >= kMaxTilemaps) return kNoTilemap;
        TilemapId id = map_n_++;
        maps_[id] = MapRec{ d.indices, d.width, d.height, d.layers, d.tile_w, d.tile_h,
                            d.tileset, 0, 0 };
        return id;
    }

    void set_scroll(TilemapId id, vec2 px) override {
        if (id >= map_n_) return;
        maps_[id].scroll_x = s_to_int(px.x);
        maps_[id].scroll_y = s_to_int(px.y);
    }

    // Cells changed in place since upload -> force a re-stream on the next draw (defeats the
    // window cache, whose key otherwise assumes immutable baked cells).
    void invalidate_map(TilemapId id) override { if (id < map_n_) map_dirty_[id] = true; }

    void begin(const Camera2D& cam) override {
        // Camera pan maps to free BG/OBJ scroll (the GBA's superpower). Camera ZOOM is
        // deliberately NOT applied: a text-BG + plain OBJ can't scale, so zoom would need the
        // affine BG/OBJ path (Mode-1/2 + REG_BGxPA.., a per-scanline matrix) — the opt-in 2.5D
        // route in docs/03 §4, out of the MVP. The PPU renders 1:1; cam.zoom is ignored here.
        cam_x_ = s_to_int(cam.pos.x);
        cam_y_ = s_to_int(cam.pos.y);
        obj_n_ = 0;
        obj_tile_n_ = 0;
        slot_n_ = 0;
        for (int s = 0; s < kBgSlots; ++s) st_.bg[s].enabled = false;
        stats_ = RenderStats{};
    }

    // One draw call = one of the PPU's four text backgrounds, in painter order (first call
    // is the furthest back). The camera-visible part of the (arbitrarily large) source map
    // is streamed into the slot's 32×32 screenblock window; the raw scroll goes to HOFS/VOFS
    // and both the model and the silicon wrap at 256 px. Tiles must be 8×8 (hardware BG size);
    // a fifth layer in one frame is ignored (the machine has four backgrounds — honest limit).
    //
    // DIRTY-TRACKED: the window content is a pure function of (layer cells, window tile
    // origin) — fine scroll within a tile only moves HOFS/VOFS. Re-streaming all four layers
    // every frame (~2800 cells + the matching VRAM push) was ~half the ARM7 frame budget, for
    // windows that are byte-identical most frames (parallax layers shift origin only every
    // 8/factor camera px). Skip the stream on a cache hit; `win_stamp_` tells the hardware
    // push which screenblocks actually changed. Map cell data is immutable (baked assets), so
    // the cells pointer + origin is a sound key.
    void draw_tilemap(TilemapId id, uint8_t layer) override {
        if (id >= map_n_) return;
        const MapRec& m = maps_[id];
        if (layer >= m.layers || m.tw != kTile || m.th != kTile) return;
        const TexRec* texture_rec = texture(m.tileset);
        if (!texture_rec) return;
        if (slot_n_ >= kBgSlots)                                 return;
        const TexRec& ts = *texture_rec;
        const uint16_t* cells = m.idx + size_t(layer) * size_t(m.w) * size_t(m.h);
        const int slot = slot_n_++;
        PpuScreenEntry* win = win_[slot];

        const int eff_x = m.scroll_x + cam_x_;
        const int eff_y = m.scroll_y + cam_y_;
        // The screen samples at most 31×21 cells from the wrapped window; stream 32×kWinRows
        // so fine scroll never reads a stale edge. (kScreenW/kTile+1 = 31.)
        const int tx0 = floor_div(eff_x, kTile), ty0 = floor_div(eff_y, kTile);
        const bool same_map = !map_dirty_[id] && win_cells_[slot] == cells
                              && win_base_[slot] == ts.base;
        const int dx = tx0 - win_tx0_[slot], dy = ty0 - win_ty0_[slot];
        // The entry for map cell (tx,ty) — the incremental twin of the full-stream loop
        // below; the two must stay identical (out-of-map/empty cells stream entry 0).
        auto cell_at = [&](int tx, int ty) {
            PpuScreenEntry e{ 0, 0, 0 };
            if (ty >= 0 && ty < int(m.h) && tx >= 0 && tx < int(m.w)) {
                const uint16_t c = cells[size_t(ty) * m.w + tx];
                if (c != 0 && uint32_t(c - 1) < uint32_t(ts.cols) * ts.rows) {
                    e.tile = uint16_t(ts.base + c - 1);
                    e.bank = tile_bank_[e.tile];
                }
            }
            return e;
        };
        if (same_map && dx == 0 && dy == 0) {
            stats_.tiles_drawn += win_tiles_[slot];   // same window content: stream skipped
        } else if (same_map && dx > -kBgWin && dx < kBgWin && dy > -kWinRows && dy < kWinRows) {
            // INCREMENTAL RE-STREAM: same layer, origin moved by (dx,dy) tiles — ordinary
            // scrolling. Only the entering rows/columns are streamed (~22–32 cells per tile
            // boundary crossed), not the whole 32×22 window: a full re-stream is ~700 cells
            // per layer, and with stacked parallax layers the crossings periodically
            // COINCIDE (factors 0.25/0.5/1.0 all cross every 32 camera px) — a per-frame
            // cost spike that blew the ARM7 vblank budget and read as a rhythmic stutter
            // while scrolling. win_tiles_ stays exact by retiring each departing row/column
            // before its slot is overwritten (a streamed row's 32 slots are all counted).
            uint32_t tiles = win_tiles_[slot];
            const int tx0_old = tx0 - dx;
            if (dy != 0) {                      // row pass, over the OLD column range
                const int n   = dy > 0 ? dy : -dy;
                const int dep = dy > 0 ? ty0 - dy : ty0 + kWinRows;    // leaving rows
                const int arr = dy > 0 ? ty0 + kWinRows - dy : ty0;    // entering rows
                for (int k = 0; k < n; ++k) {
                    const PpuScreenEntry* row = win + size_t((dep + k) & (kBgWin - 1)) * kBgWin;
                    for (int i = 0; i < kBgWin; ++i) if (row[i].tile) --tiles;
                }
                for (int k = 0; k < n; ++k) {
                    const int ty = arr + k;
                    PpuScreenEntry* row = win + size_t(ty & (kBgWin - 1)) * kBgWin;
                    for (int tx = tx0_old; tx < tx0_old + kBgWin; ++tx) {
                        const PpuScreenEntry e = cell_at(tx, ty);
                        if (e.tile) ++tiles;
                        row[tx & (kBgWin - 1)] = e;
                    }
                }
            }
            if (dx != 0) {                      // column pass, over the NEW row range
                const int n   = dx > 0 ? dx : -dx;
                const int arr = dx > 0 ? tx0 + kBgWin - dx : tx0;      // entering columns
                for (int ty = ty0; ty < ty0 + kWinRows; ++ty) {
                    PpuScreenEntry* row = win + size_t(ty & (kBgWin - 1)) * kBgWin;
                    for (int k = 0; k < n; ++k) {
                        PpuScreenEntry& dst = row[(arr + k) & (kBgWin - 1)];
                        if (dst.tile) --tiles;  // the departing column shared this slot
                        const PpuScreenEntry e = cell_at(arr + k, ty);
                        if (e.tile) ++tiles;
                        dst = e;
                    }
                }
            }
            stats_.tiles_drawn += tiles;
            win_tx0_[slot] = tx0; win_ty0_[slot] = ty0; win_tiles_[slot] = tiles;
            ++win_stamp_[slot];
            // Tell the hardware push which way the window slid; if it fully cycled since
            // the last push, fall back to a whole-screenblock rewrite.
            const int adx = win_push_dx_[slot] + dx, ady = win_push_dy_[slot] + dy;
            if (adx <= -kBgWin || adx >= kBgWin || ady <= -kWinRows || ady >= kWinRows) {
                win_push_full_[slot] = true;
                win_push_dx_[slot] = win_push_dy_[slot] = 0;
            } else {
                win_push_dx_[slot] = int8_t(adx); win_push_dy_[slot] = int8_t(ady);
            }
        } else {
            uint32_t streamed = 0;
            for (int ty = ty0; ty < ty0 + kWinRows; ++ty) {
                const bool yin = ty >= 0 && ty < int(m.h);
                PpuScreenEntry* row = win + size_t(ty & (kBgWin - 1)) * kBgWin;
                for (int tx = tx0; tx < tx0 + kBgWin; ++tx) {
                    PpuScreenEntry e{ 0, 0, 0 };
                    if (yin && tx >= 0 && tx < int(m.w)) {
                        const uint16_t c = cells[size_t(ty) * m.w + tx];
                        if (c != 0 && uint32_t(c - 1) < uint32_t(ts.cols) * ts.rows) {
                            e.tile = uint16_t(ts.base + c - 1);
                            e.bank = tile_bank_[e.tile];
                            ++streamed;
                        }
                    }
                    row[tx & (kBgWin - 1)] = e;
                }
            }
            stats_.tiles_drawn += streamed;
            win_cells_[slot] = cells; win_tx0_[slot] = tx0; win_ty0_[slot] = ty0;
            win_base_[slot] = ts.base; win_tiles_[slot] = streamed;
            map_dirty_[id] = false;
            ++win_stamp_[slot];
            win_push_full_[slot] = true;
            win_push_dx_[slot] = win_push_dy_[slot] = 0;
        }
        st_.bg[slot].win      = win;
        st_.bg[slot].scroll_x = eff_x;
        st_.bg[slot].scroll_y = eff_y;
        st_.bg[slot].enabled  = true;
    }

    // Sprites -> OAM (OBJ) + a contiguous per-frame OBJ tile run. Source rects must be
    // 8px-aligned and map to a real hardware OBJ shape; each OBJ indexes ONE palette bank
    // (per-tile-banked textures are BG-only). Dest scaling and per-channel tint are not
    // expressible on plain OBJ, so they are ignored — honest hardware behaviour. Beyond the
    // OBJ or tile-run ceilings sprites are dropped and counted (graceful overflow).
    void submit_sprites(const DrawSprite* s, uint32_t n) override {
        for (uint32_t i = 0; i < n; ++i) {
            const DrawSprite& d = s[i];
            const TexRec* texture_rec = texture(d.tex);
            if (!texture_rec) continue;
            if ((d.sw % kTile) || (d.sh % kTile) || d.sw <= 0 || d.sh <= 0) continue;
            if ((d.sx % kTile) || (d.sy % kTile) || d.sx < 0 || d.sy < 0)   continue;
            const TexRec& t = *texture_rec;
            const uint8_t wt = uint8_t(d.sw / kTile), ht = uint8_t(d.sh / kTile);
            uint16_t shape, size;
            if (!obj_shape_size(wt, ht, shape, size))        continue;  // no such OBJ shape
            if (t.bank == kBankNone)                         continue;  // per-tile banks: BG-only
            if (obj_n_ >= obj_limit_ || obj_tile_n_ + uint32_t(wt) * ht > kObjTileCap) {
                ++stats_.sprites_dropped;
                continue;
            }
            const uint16_t tc = uint16_t(d.sx / kTile), tr = uint16_t(d.sy / kTile);
            if (tr + ht > t.rows || tc + wt > t.cols)        continue;  // rect outside atlas

            // Re-pack the sprite's atlas tiles into the 1D run OAM can address.
            const uint16_t run = obj_tile_n_;
            for (uint8_t ry = 0; ry < ht; ++ry)
                for (uint8_t rx = 0; rx < wt; ++rx)
                    obj_tiles_[run + ry * wt + rx] =
                        tiles_[t.base + (tr + ry) * t.cols + (tc + rx)];
            obj_tile_n_ = uint16_t(run + wt * ht);

            PpuObj& o = oam_[obj_n_++];
            o.x       = int16_t(s_to_int(d.pos.x) - cam_x_);
            o.y       = int16_t(s_to_int(d.pos.y) - cam_y_);
            o.tile    = run;
            o.w_tiles = wt;
            o.h_tiles = ht;
            o.flip    = uint8_t(((d.flags & kFlipX) ? 1 : 0) | ((d.flags & kFlipY) ? 2 : 0));
            o.bank    = t.bank;
        }
        ++stats_.batches;
    }

    void end() override {
        st_.tiles     = tiles_;
        st_.obj_tiles = obj_tiles_;
        st_.palette   = palette_;
        st_.oam       = oam_;
        st_.obj_count = obj_n_;

#if defined(PHX_GBA_HW)
        // Real hardware: program the PPU (tiles/palette incrementally, windows/OAM/OBJ tiles
        // per frame) and let the silicon scan it out. `ppu_compose` (the else branch) is the
        // exact golden definition of the frame this must produce.
        submit_hardware();
#else
        // Headless / software-tier present: compose exactly what the PPU would scan out.
        ppu_compose(st_, scratch_);

        // Blit the 240x160 PPU frame into the platform framebuffer (clipped if smaller).
        phx_soft_fb fb = phx_gfx_soft_lock(gfx_);
        if (fb.pixels) {
            const int cw = fb.w < kScreenW ? fb.w : kScreenW;
            const int ch = fb.h < kScreenH ? fb.h : kScreenH;
            for (int y = 0; y < ch; ++y)
                for (int x = 0; x < cw; ++x)
                    fb.pixels[y * fb.w + x] = scratch_[y * kScreenW + x];
        }
#endif
    }

    RenderStats& stats() override { return stats_; }

private:
#if defined(PHX_GBA_HW)
    // Push the built model into the PPU's memory-mapped state. VRAM/PALRAM/OAM forbid 8-bit
    // writes, so everything goes out as 16-/32-bit units. Layout: BG charblocks 0–1
    // @0x06000000 hold the (packed 4bpp) BG tiles; the four screenblock windows live in
    // blocks 24–27 @0x0600C000 (tile index 768+, clear of kTileCap=512); the per-frame OBJ
    // tile run goes to OBJ char VRAM @0x06010000; the 16 palette banks go to both the BG and
    // OBJ palette RAM (the model shares one bank table for tiles and sprites).
    void submit_hardware() {
        volatile uint16_t* const VCOUNT   = reinterpret_cast<volatile uint16_t*>(0x04000006);
        volatile uint16_t* const PAL_BG   = reinterpret_cast<volatile uint16_t*>(0x05000000);
        volatile uint16_t* const PAL_OBJ  = reinterpret_cast<volatile uint16_t*>(0x05000200);
        volatile uint32_t* const CHAR_BG  = reinterpret_cast<volatile uint32_t*>(0x06000000);
        volatile uint32_t* const CHAR_OBJ = reinterpret_cast<volatile uint32_t*>(0x06010000);
        volatile uint16_t* const OAM      = reinterpret_cast<volatile uint16_t*>(0x07000000);
        volatile uint16_t* const DISPCNT  = reinterpret_cast<volatile uint16_t*>(0x04000000);
        volatile uint16_t* const BGCNT    = reinterpret_cast<volatile uint16_t*>(0x04000008);
        volatile uint16_t* const BGOFS    = reinterpret_cast<volatile uint16_t*>(0x04000010);
        constexpr uint16_t kScreenblock0 = 24;    // 0x0600C000 = VRAM base + 24*0x800

        // VBLANK GATE: everything below writes live PPU state (OAM, OBJ char VRAM, scroll,
        // screenblocks) that the silicon is scanning out RIGHT NOW if we're in the draw
        // period — end() runs before present()'s vblank wait, i.e. at a raster position set
        // by how long the game frame took. Racing the beam is visible: the OAM pass below
        // hides all 128 OBJs before re-emitting them, so any scanline drawn inside that
        // window shows NO sprites — a fixed-position horizontal cut on a static scene (the
        // title text sliced in half) and roaming sprite flicker in-game as frame time
        // jitters; mid-draw HOFS/VOFS writes tear scrolling BGs the same way. So reach this
        // frame's vblank (68 scanlines — ample for the typical push) before touching
        // anything. Already inside vblank (a slow frame) = no wait; present()'s own
        // vblank_wait then no-ops and still returns at the vblank END, so pacing is
        // unchanged (one return per hardware frame).
        while (*VCOUNT < kScreenH) {}

        // palette + BG tiles grow only at load time: push just what's new since last frame.
        if (pal_pushed_ < pal_stamp_) {
            for (int i = 0; i < kPalTotal; ++i) { PAL_BG[i] = palette_[i]; PAL_OBJ[i] = palette_[i]; }
            pal_pushed_ = pal_stamp_;
        }
        for (; tiles_pushed_ < tile_count_; ++tiles_pushed_)
            for (int r = 0; r < kTile; ++r)
                CHAR_BG[tiles_pushed_ * 8 + r] = tiles_[tiles_pushed_].row[r];

        // per-frame OBJ tile run (packed 4bpp words, straight copy)
        for (uint16_t t = 0; t < obj_tile_n_; ++t)
            for (int r = 0; r < kTile; ++r)
                CHAR_OBJ[t * 8 + r] = obj_tiles_[t].row[r];

        // the four windows: pack entries into hardware screen entries. Slot 0 is furthest
        // back -> lowest BG priority value wins in front, so slot s gets priority 3-s.
        // Screenblocks are only rewritten when draw_tilemap actually re-streamed the window
        // (win_stamp_ advanced) — VRAM retains the previous content, so an unchanged window
        // costs zero writes — and an ordinary scroll (win_push_full_ clear) writes only the
        // entering columns/rows the incremental stream touched, not the whole 1024-entry
        // block. Scroll registers are per-frame (fine scroll moves every frame).
        const auto pack = [](const PpuScreenEntry& e) {
            return uint16_t((e.tile & 0x3FF)
                            | ((e.flip & 1) ? (1u << 10) : 0)
                            | ((e.flip & 2) ? (1u << 11) : 0)
                            | (uint16_t(e.bank & 0xF) << 12));
        };
        uint16_t bg_enable = 0;
        for (int s = 0; s < kBgSlots; ++s) {
            if (!st_.bg[s].enabled) continue;
            bg_enable |= uint16_t(1u << (8 + s));
            if (win_pushed_[s] != win_stamp_[s]) {
                volatile uint16_t* blk =
                    reinterpret_cast<volatile uint16_t*>(0x06000000 + (kScreenblock0 + s) * 0x800);
                const PpuScreenEntry* w = win_[s];
                if (win_push_full_[s]) {
                    for (int i = 0; i < kBgWin * kBgWin; ++i) blk[i] = pack(w[i]);
                } else {
                    // Entering columns are pushed over all 32 row slots (rows outside the
                    // streamed range are stale in BOTH copies and never sampled), entering
                    // rows in full — so every slot the incremental stream changed since the
                    // last push is covered, even across an unpushed frame or a column/row
                    // that slid under a later crossing.
                    const int cdx = win_push_dx_[s];
                    const int cn  = cdx > 0 ? cdx : -cdx;
                    const int c0  = cdx > 0 ? win_tx0_[s] + kBgWin - cdx : win_tx0_[s];
                    for (int k = 0; k < cn; ++k) {
                        const int cs = (c0 + k) & (kBgWin - 1);
                        for (int r = 0; r < kBgWin; ++r)
                            blk[r * kBgWin + cs] = pack(w[r * kBgWin + cs]);
                    }
                    const int cdy = win_push_dy_[s];
                    const int rn  = cdy > 0 ? cdy : -cdy;
                    const int r0  = cdy > 0 ? win_ty0_[s] + kWinRows - cdy : win_ty0_[s];
                    for (int k = 0; k < rn; ++k) {
                        const int rs = (r0 + k) & (kBgWin - 1);
                        for (int i = 0; i < kBgWin; ++i)
                            blk[rs * kBgWin + i] = pack(w[rs * kBgWin + i]);
                    }
                }
                win_pushed_[s]    = win_stamp_[s];
                win_push_full_[s] = false;
                win_push_dx_[s]   = win_push_dy_[s] = 0;
            }
            // 4bpp, char base 0, 32×32, screenblock 24+s, priority 3-s
            BGCNT[s]        = uint16_t(uint16_t(3 - s) | (uint16_t(kScreenblock0 + s) << 8));
            BGOFS[s * 2]     = uint16_t(st_.bg[s].scroll_x);
            BGOFS[s * 2 + 1] = uint16_t(st_.bg[s].scroll_y);
        }

        // OBJ -> OAM (4 halfwords/entry; 4th is affine, unused). Hide all, then emit ours.
        // The model paints oam_[] in painter order (LATER entries on top), but the silicon
        // gives LOWER OAM indices priority — so write the list reversed to match ppu_compose.
        for (int i = 0; i < kObjMax; ++i) OAM[i * 4 + 0] = 0x0200;   // attr0 bit9 = disabled
        for (uint16_t i = 0; i < obj_n_; ++i) {
            const PpuObj& o = oam_[i];
            uint16_t shape, size;
            if (!obj_shape_size(o.w_tiles, o.h_tiles, shape, size)) continue;  // pre-validated
            const uint16_t a0 = uint16_t((uint16_t(o.y) & 0xFF) | (shape << 14));  // 16-col, normal
            const uint16_t a1 = uint16_t((uint16_t(o.x) & 0x1FF)
                                         | ((o.flip & 1) ? (1u << 12) : 0)          // H flip
                                         | ((o.flip & 2) ? (1u << 13) : 0)         // V flip
                                         | (size << 14));
            const uint16_t a2 = uint16_t((o.tile & 0x3FF)                          // priority 0
                                         | (uint16_t(o.bank & 0xF) << 12));
            const uint16_t slot = uint16_t(obj_n_ - 1 - i);
            OAM[slot * 4 + 0] = a0; OAM[slot * 4 + 1] = a1; OAM[slot * 4 + 2] = a2;
        }

        // Mode 0 | enabled BGs | OBJ on (bit12) | 1D OBJ tile mapping (bit6).
        *DISPCNT = uint16_t(bg_enable | (1u << 12) | (1u << 6));
    }
#endif // PHX_GBA_HW

    TexRec* texture(TextureId id) {
        if (id == kNoTexture) return nullptr;
        TexRec& t = tex_[id & 255u];
        return t.used && t.handle == id ? &t : nullptr;
    }

    int find_slot() const {
        for (int i = 0; i < kMaxTextures; ++i)
            // Never wrap a generation: retire a slot after 16M uses rather than alias.
            if (!tex_[i].used && (tex_[i].handle >> 8) < 0xFFFFFEu) return i;
        return -1;
    }

    uint16_t find_run(uint32_t need) const {
        if (!need || need >= kTileCap) return 0;
        uint32_t base = 1;
        for (;;) {
            if (base + need > kTileCap) return 0;
            bool overlap = false;
            for (int i = 0; i < kMaxTextures; ++i) {
                const TexRec& t = tex_[i];
                const uint32_t end = t.base + uint32_t(t.cols) * t.rows;
                if (t.used && base < end && base + need > t.base) {
                    base = end; overlap = true; break;
                }
            }
            if (!overlap) return uint16_t(base);
        }
    }

    TextureId commit(int slot, uint16_t base, uint16_t cols, uint16_t rows, uint8_t bank) {
        TexRec& t = tex_[slot];
        const TextureId id = (((t.handle >> 8) + 1u) << 8) | uint32_t(slot);
        t = TexRec{base, cols, rows, true, true, bank, id};
        const uint16_t end = uint16_t(base + uint32_t(cols) * rows);
        if (end > tile_count_) tile_count_ = end;
#if defined(PHX_GBA_HW)
        // Reused runs must reach VRAM too, not just the host compositor.
        if (base < tiles_pushed_) tiles_pushed_ = base;
#endif
        // Reusing a base can change tile palette banks even at identical map/camera pointers.
        for (int s = 0; s < kBgSlots; ++s) win_cells_[s] = nullptr;
        return id;
    }

    void reclaim_palette() {
        for (auto& mask : bank_used_) mask = 0;
        // Rebuild usage from live packed texels, preserving every live bank/slot index.
        // Bounded load-time work (at most 511 tiles); no moving colours under live textures.
        for (int i = 0; i < kMaxTextures; ++i) {
            const TexRec& t = tex_[i];
            if (!t.used) continue;
            for (uint32_t n = t.base; n < t.base + uint32_t(t.cols) * t.rows; ++n)
                for (uint32_t row : tiles_[n].row)
                    for (int x = 0; x < kTile; ++x) {
                        const uint8_t index = uint8_t((row >> (x * 4)) & 15u);
                        if (index) bank_used_[tile_bank_[n]] |= uint16_t(1u << index);
                    }
        }
    }

    // Claim a palette bank able to hold all `n` colours (absorbing the missing ones), or -1
    // when every bank is full — the honest 16×15-colour ceiling. Greedy first fit keeps
    // similar textures sharing banks.
    int claim_bank(const uint16_t* cols, int n) {
        for (int b = 0; b < kPalBanks; ++b) {
            int missing = 0;
            for (int k = 0; k < n; ++k) {
                bool found = false;
                for (int s = 1; s < kPalSize; ++s)
                    if ((bank_used_[b] & (1u << s)) && palette_[b * kPalSize + s] == cols[k]) { found = true; break; }
                if (!found) ++missing;
            }
            int available = 0;
            for (int s = 1; s < kPalSize; ++s) if (!(bank_used_[b] & (1u << s))) ++available;
            if (missing > available) continue;
            for (int k = 0; k < n; ++k) {                       // absorb the missing colours
                bool found = false;
                for (int s = 1; s < kPalSize; ++s)
                    if ((bank_used_[b] & (1u << s)) && palette_[b * kPalSize + s] == cols[k]) { found = true; break; }
                if (!found) {
                    int s = 1;
                    while (bank_used_[b] & (1u << s)) ++s;
                    bank_used_[b] |= uint16_t(1u << s);
                    palette_[b * kPalSize + s] = cols[k];
                    ++pal_stamp_;
                }
            }
            return b;
        }
        return -1;
    }

    // Slot of `c` within bank `b` (1..15). Colours were interned by claim_bank, so this hits.
    uint8_t bank_slot(uint8_t b, uint16_t c) const {
        for (int s = 1; s < kPalSize; ++s)
            if ((bank_used_[b] & (1u << s)) && palette_[b * kPalSize + s] == c) return uint8_t(s);
        return 0;   // unreachable for interned colours; 0 keeps it visibly transparent
    }

    phx_gfx*   gfx_ = nullptr;
    PpuTile*   tiles_     = nullptr;  uint16_t tile_count_ = 0;
    uint8_t*   tile_bank_ = nullptr;
    PpuTile*   obj_tiles_ = nullptr;  uint16_t obj_tile_n_ = 0;
    uint16_t   palette_[kPalTotal] = {};
    uint16_t   bank_used_[kPalBanks] = {};   // colours interned per bank (excluding slot 0)
    uint32_t   pal_stamp_ = 1;             // bumped on every interned colour (dirty tracking)
    TexRec*    tex_  = nullptr;
    MapRec*    maps_ = nullptr;   uint16_t map_n_  = 0;
    bool       map_dirty_[kMaxTilemaps] = {};   // set by invalidate_map(): force a window re-stream
    PpuScreenEntry* win_[kBgSlots] = {};
    // Per-slot window cache: the layer streamed into the slot and its tile origin (the full
    // key of the window's content), the non-empty cell count (for stats on a cache hit), and
    // a change stamp the hardware push compares against to skip unchanged screenblocks.
    const uint16_t* win_cells_[kBgSlots] = {};
    int        win_tx0_[kBgSlots] = {}, win_ty0_[kBgSlots] = {};
    uint16_t   win_base_[kBgSlots] = {};   // tileset char-store base (guards id recycling)
    uint32_t   win_tiles_[kBgSlots] = {};
    uint16_t   win_stamp_[kBgSlots] = { 1, 1, 1, 1 };
    // How far each slot's window origin slid since the hardware push last wrote its
    // screenblock (columns/rows crossed), or whether it needs a full rewrite. Maintained by
    // draw_tilemap in every build; consumed (and reset) only by the PHX_GBA_HW push.
    bool       win_push_full_[kBgSlots] = { true, true, true, true };
    int8_t     win_push_dx_[kBgSlots] = {}, win_push_dy_[kBgSlots] = {};
    uint8_t    slot_n_ = 0;
    PpuObj*    oam_ = nullptr;    uint16_t obj_n_ = 0, obj_limit_ = kObjMax;
    uint32_t*  scratch_ = nullptr;
    int32_t    cam_x_ = 0, cam_y_ = 0;
#if defined(PHX_GBA_HW)
    uint16_t   tiles_pushed_ = 0;
    uint32_t   pal_pushed_   = 0;
    uint16_t   win_pushed_[kBgSlots] = {};   // last win_stamp_ written to each screenblock
#endif
    PpuState   st_{};
    RenderStats stats_{};
};

} // namespace

IRenderBackend* phx_make_render_backend(phx_gfx* gfx, ArenaAllocator& arena, const Caps& caps) {
    GbaPpuBackend* be = arena.make<GbaPpuBackend>();
    if (!be || !be->init(gfx, arena, caps)) return nullptr;
    return be;
}

} // namespace phx
