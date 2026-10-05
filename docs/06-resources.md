# Phosphorus Engine — Resources & Asset Format

> `engine/resource/` (runtime) + the pack format produced by `tools/` (offline).
> Principle: **assets are baked offline, resident as one stable in-memory image, read
> in place by zero-copy views, never parsed at runtime.** PNG/JSON/WAV/Tiled `.tmj` exist only on the developer's machine (no XML
> input anywhere in the pipeline today — Tiled maps are read as `.tmj`, not `.tmx`).

## 1. The `.phxp` bundle format

A bundle is a flat, append-only container the engine reads in place from a stable
in-memory image (heap-loaded once on PC; linked into the program image on GBA/PSP). Layout:

```
 offset 0
 ┌──────────────────────────────────────────────────────────────┐
 │ Header (24 B)                                                  │
 │  magic "PHXP" | version u16 | target u8 | flags u8             │
 │  asset_count u32 | toc_offset u32 | total_size u32 | crc32 u32 │
 ├──────────────────────────────────────────────────────────────┤
 │ TOC[asset_count]   (sorted by name_hash for binary search)     │
 │  name_hash u32 | type u16 | flags u16 | offset u32 | size u32  │
 │  uncompressed_size u32                                         │
 ├──────────────────────────────────────────────────────────────┤
 │ Blob region  (each blob aligned to 16 B; type-specific layout) │
 │  [texture][texture][tilemap][sound][bin]...                    │
 └──────────────────────────────────────────────────────────────┘
```

- **Names are 32-bit FNV-1a hashes**, not strings — no string table at runtime, no
  `std::string`. Name hashes are one-way, so for dev-build debugging
  `phxpack --manifest` writes a human-readable `<out>.manifest.txt` sidecar
  (hash ↔ name ↔ source path, one line per asset).
- **TOC is sorted** → `O(log n)` lookup, or `O(1)` if the game caches the handle at
  load (it should).
- **Specialized at pack time, per target, where it matters.** Textures and sounds are
  per-target encoded (§4); the bundle's `target` byte guards a mismatched-target mount
  (§2), and `phxpack` refuses to merge an intermediate baked for a different tier.

## 2. Validation & lifecycle

`mount()` never trusts the file it's handed — a bundle can be truncated by an interrupted
write, bit-rotted on a flash cart, or simply the wrong file — so every mount runs the full
chain below before a single asset becomes visible, cheapest checks first:

1. **Magic + version.** Wrong magic (not a `.phxp`, or a big-endian host) or a version newer
   than the reader understands both fail closed (`Status::IoError` / `Status::Unsupported`).
2. **Structural bounds.** `total_size` can't exceed the mapped file; the TOC and every single
   entry's `[offset, offset+size)` must land fully inside the bundle. This runs BEFORE the
   checksum pass, so a corrupt offset is caught precisely (`Status::IoError`), not lumped in
   with a generic checksum failure.
3. **Checksum.** `BundleHeader.blob_crc32` (phx/core/crc32.h) covers the TOC + every blob;
   a mismatch is `Status::Corrupt`. Pass `verify_checksum=false` to `mount()` to skip this pass
   (still get every structural check) when mount-time latency matters more than catching
   bit-rot — the default is on.
4. **Target tier.** On a REAL console build (never the host, including `TIER=gba_sim`, which
   only simulates the GBA's fixed-point *scalar* tier — see CLAUDE.md) the bundle's `target`
   byte must match the tier the binary actually ships on, so a bundle baked for the wrong
   console can never load silently — `Status::Unsupported`.

`mount()` is idempotent (mounting the same path twice is a no-op, not a wasted slot).
`unmount()`/`unmount_all()` close the platform file/mapping and free the slot for reuse —
mounting a fresh bundle for a new level, or a clean shutdown. Every view/pointer this cache
handed out from that bundle (a `TextureView`, a raw blob pointer, …) is invalidated the
instant unmount returns: they alias the mapping and are never copied out.

## 3. Runtime API (`phx/resource/cache.h`)

```cpp
namespace phx {

enum class AssetType : uint16_t { Texture, Tilemap, Sprite, Sound, Music, Font, Bin };

class ResourceCache {
public:
    static Result<ResourceCache*> create(ArenaAllocator&);

    // load-once heap image (PC) or linked-in bundle (PSP EBOOT / GBA ROM pointer),
    // fully validated (§2) before anything is visible.
    Status mount(const phx_platform*, const char* bundle_path, bool verify_checksum = true);
    Status unmount(const phx_platform*, const char* bundle_path);
    Status unmount_all(const phx_platform*);

    // O(1) typed views into the mounted blob — no copy, no parse:
    Result<TextureView> texture(NameHash);
    Result<TilemapView> tilemap(NameHash);
    Result<SoundDataView> sound(NameHash);
    Result<BlobView>    blob(NameHash);

    uint32_t asset_count() const;
    uint32_t mount_count() const;
};

// compile-time name hashing so call sites cost nothing:
constexpr NameHash operator""_hash(const char* s, size_t n);   // "player.png"_hash
} // namespace phx
```

Usage:

```cpp
res->mount(app.platform(), "assets.phxp");
TextureView tv = res->texture("hero_atlas"_hash).unwrap();   // zero-copy view
TextureId   player = ctx->render->load_texture({ tv.pixels, tv.width, tv.height, tv.format });
auto map = ctx->res->tilemap("level1"_hash).unwrap();         // zero-copy view
```

## 4. Per-platform specialization (same logical asset, different bytes)

**This table is as built** — what `tools/phxpack` produces per `--target` today.

| Asset    | GBA blob (`--target 0`)             | PSP blob (`--target 1`)   | PC blob (`--target 2`) |
|----------|-------------------------------------|---------------------------|------------------|
| Texture  | **4bpp paletted 8×8 tiles + 16-colour BGR555 palettes** (`PAL4_TILES`, phx/core/pixel.h) — the PPU's native layout, ~8× less texel data; falls back to RGBA8 when the art isn't tier-0 expressible | **swizzled RGBA8** (`RGBA8_SWZ`, GU block order, sampled zero-copy with the swizzle bit; pure reorder → still bit-identical to the soft golden); linear RGBA8 when the size doesn't block-align | RGBA8 |
| Tilemap  | `uint16` index layers + optional per-layer Q16 parallax table + optional per-tile collision-flags table (solid/oneway/hazard, from Tiled tileset properties) | same       | same             |
| Sound SFX| mono 16-bit PCM **resampled to the 18157 Hz device rate at bake** | mono 16-bit PCM @ source rate | mono 16-bit PCM @ source rate |
| Music    | (same PCM path as SFX today)         | same                      | same             |
| Dialogue | conversations: lines, choices, conditions and effects as index tables + a string table (`DialogueHeader` …, read in place; every index checked at load) | same | same |
| Font     | a glyph table (`FontBlobHeader` + `FontGlyphDef`: each character's rect, offset, advance; read in place) over its atlas, a separate Texture asset that encodes like any texture | same | same |

`phxpack --target 0|1|2` chooses the encoder (`tools/phxpack/tex_encode.h` mirrors the
GBA PPU upload quantizer exactly, so a baked blob composes the same frame the RGBA8
upload path would — asserted by `make ppu`). The **runtime view structs are identical**;
only the decode behind texture-upload/audio differs, selected by the same capability
tier. A game's code is byte-for-byte identical.

> Still future work (design only): ADPCM/tracker music, BCn on PC, and a CLUT/RGBA4444
> option for PSP (rejected for now — 4444 would break the GU's bit-identity with the
> software golden reference, which is worth more than the bytes).

## 5. Caching & memory policy

```
 ResourceCache budget (from Config.cache_bytes)
 ┌───────────────────────────────────────────────────────────┐
 │ bundle image (PC: heap buffer, bundle-sized, OUTSIDE the root  │
 │ arena and this budget; PSP: inside the loaded EBOOT; GBA: ROM) │
 │ ───────────────────────────────────────────────────────────│
 │ decoded GPU resources (LRU)   │ free                         │
 │  tex#1  tex#2  tex#3  ...      │                              │
 └───────────────────────────────────────────────────────────┘
```

- **Zero-copy assets** (tilemaps, fonts, PCM on PC) cost nothing beyond the resident
  bundle image. On PC that image is a real heap buffer the size of the bundle — budget
  for it when sizing total RAM; it is not OS-backed (there is no mmap in the SDL
  backend, by design — see docs/02 §3.1).
- **Decoded/uploaded assets** (GPU textures, decompressed audio) live in an LRU bounded
  by `cache_bytes`. Eviction frees the least-recently-used until the new asset fits;
  if a single asset can't fit, that's a boot-time budgeting bug, surfaced loudly.
- On GBA there is no LRU churn — everything fits in VRAM/ROM by design; the "cache" is
  just the VRAM allocation map.

## 6. Streaming (PSP/PC)

Large music and big maps stream rather than fully resident:

```cpp
class AudioStream {           // double-buffered, filled on a low-priority tick
    SoundView src; uint32_t cursor; RingBuffer<int16_t> ring;
    void pump(StackAllocator& scratch);   // decode next chunk into ring
};
```

The mixer pulls from the ring; the resource system refills it ahead of the read
cursor. GBA does not stream (no FS); its music is a resident tracker module played by
the audio mixer's pattern player.

## 7. Compression

- **Per-blob, optional, type-aware.** TOC `flags` bit marks compression; `size` is
  compressed, `uncompressed_size` is the decode target.
- **Algorithm:** LZ77-family (we ship a tiny in-tree decompressor, ~1 KB, no external
  dep) — decompresses fast on ARM7TDMI. The GBA BIOS also has built-in LZ77/Huffman
  decoders (`SWI LZ77UnCompVram`) which the GBA backend uses for free.
- **What we compress:** tilemaps, large binary blobs, music. **What we don't:** GPU
  textures the driver wants raw, small assets where header overhead dominates.
- Compression is a *pack-time* decision per asset; the runtime just sees a flag.

## 8. Versioning

- Bundle `version u16` gates format compatibility; the loader refuses a newer major.
  **Implemented and enforced** — `mount()` rejects a version newer than the reader (§2).
- Each `AssetType` has its own `struct` version embedded in its blob header, so a
  texture format change doesn't invalidate sounds.

**Implemented — the lock-file pipeline (`tools/phxpack/lock.h`):** every `phxpack` bake
writes `<out>.lock` recording the tool version, bundle-format version, target tier,
compression flag, each input's content hash (CRC32) + size, which assets each input
produced, and the CRC32 of the written bundle. On the next invocation: unchanged inputs
are **reused from the previous bundle** (per-asset incremental), a fully unchanged input
list skips the bake entirely ("up to date"), and `phxpack --upgrade old.phxp` re-bakes a
bundle from its own lock's recorded source list after a tool/format upgrade. CI can flag
a stale bundle by diffing the lock's `output crc32` against the file. An incremental
rebake is **byte-identical** to a `--full` bake — asserted by the `make phxpack` gate
(touch one input, rebake, `cmp` against a `--full` bake) — so the lock is an
optimization, never a semantic. (The `pipeline_test` suite separately asserts that two
independent *full* bakes of the same sources are byte-identical to each other; it does
not exercise the lock/incremental-skip path.) Bumping either the
tool version (`kPhxpackToolVersion`) or `kBundleVersion` invalidates every lock.

## 9. Why baked + read-in-place (justification)

| Approach          | Runtime cost            | RAM            | GBA viable | Determinism |
|-------------------|-------------------------|----------------|------------|-------------|
| Parse PNG/JSON at runtime | decode + allocate | high, transient | no (no libpng/heap) | no |
| **Baked + in-place (Phosphorus)** | pointer cast | the bundle image only, no transient parse allocations (free on GBA — it's ROM) | yes (ROM ptr) | yes |

Baking moves all parsing/validation to the developer's machine where errors are
caught early and where we can afford slow, thorough optimization. The console does the
minimum: point and cast. This is the RenderWare/classic-SDK discipline, updated.

See `docs/08-tooling.md` for the converters that produce these blobs.
