# G2: GBA PPU texture ownership and level reloads

## Lifecycle before the fix

`ResourceCache::texture()` returns a view into a mounted, stable bundle. `Level::sprite()`
uploads that view into the renderer for each distinct sprite asset in the active level.
`Level::unload()` despawns entities before releasing their sprite texture IDs. Maps are
retained by the renderer; a Level caches eight map/tileset pairs for revisits.

The PPU backend previously recycled only texture record IDs. Its 512-tile char store
(including transparent tile zero) and sixteen 15-color palette banks were append-only.
Freeing an atlas did not release either resource. Palette claims happened before encoding
completed, and tile allocation happened before checking for an available texture record.
Failures could therefore consume palette colors or tiles without returning a texture.
Reusing an ID let stale sprite or map references resolve to unrelated textures. When the
Level's eight-entry cache filled, it still uploaded additional retained maps, then freed
their tilesets on unload, leaving those renderer-owned maps referencing recycled IDs.

## Ownership after the fix

- The bundle owns source pixels and map indices. Callers must keep those buffers alive
  for their documented lifetime; a retained map's indices live until renderer teardown.
- An uploaded texture owns one contiguous tile run. Allocation searches the existing
  511 usable tiles in deterministic first-fit order against **all live texture runs**.
  Freeing a transient texture makes its entire run available without moving other tiles.
  The tile high-water mark only bounds hardware transfers; it no longer allocates resources.
- Each palette bank tracks occupied slots. Destruction rebuilds those masks from live
  packed texels, including retained map tilesets. Live palette indices never move;
  unused slots can immediately hold new colors. The backdrop/transparent slot is reserved.
  This is bounded load/unload work, with no frame-time allocation or heap use.
- Upload snapshots the 512-byte palette and 32-byte occupancy masks on the stack. On
  failure it restores those masks, colors and dirty stamp. Encoding writes only into a
  free tile run; the texture record, handle and hardware dirty state commit on success.
  Slot/run exhaustion is checked before claiming colors. Both RGBA8 and PAL4 use this path.
- PPU texture handles have an eight-bit slot and a 24-bit generation. `TextureId` is now
  an opaque 32-bit value (no serialized asset format changes). Draws and frees validate
  the entire handle. Generations never wrap: a slot retires after 16,777,214 successful
  uploads, rather than aliasing a stale handle. Other backends keep their existing slot
  implementation; the shared sort preserves layer/z ordering with the wider handle.
- Renderer-owned maps pin their tilesets until the renderer arena is destroyed. Releasing
  the uploader's ownership of a pinned texture is idempotent and preserves the map.
  `live_textures()` includes those pinned resources and changes only on an actual release.
  Identical PPU map descriptors reuse a retained slot. No map deletion API was introduced.
- Level caches exact `(map asset, resolved tileset asset)` pairs, without a lossy combined
  hash. It rejects a ninth pair **before allocating**, and retains existing pairs for
  subsequent revisits. Texture or map upload failures return `OutOfMemory`; failed map
  uploads release their temporary tileset, and failed sprite uploads despawn partially
  loaded entities and release sprite textures. Calling load on a loaded Level first unloads it.
- A successful upload invalidates streamed-window cache keys and rewinds the hardware
  tile-transfer cursor to the reused run, so real VRAM receives replacement tile contents.

Limits remain 256 live texture records, 512 BG tiles, sixteen palette banks, 256 per-frame
OBJ tiles and 32 retained maps per renderer. A Level supports eight retained pairs whose
combined resident tiles/palettes fit the hardware; sprites are transient. Genuine capacity
or contiguous-run fragmentation failures remain explicit and recoverable. The retained-map
strategy intentionally trades reclamation of unused maps for bounded, allocation-free revisits.

## Regression evidence

The new PPU tests were linked against the pre-fix backend from `HEAD`, adapting only its
internal `free_tex` return type to the current interface. Ten lifecycle checks failed.
The same pre-fix backend linked into the generated-project stress runner completed only
**eight** revisits before its sprite upload failed. The frame hash before failure was
`a1d03c7c`. The updated Level reports that upload failure immediately, making it observable.

With the fix, `make ppu` passes 65 checks, including:

- 1,000 alternating RGBA8/PAL4 64-tile upload/free cycles, changing colors each cycle;
  a live pinned BG, current OBJ pixels, stale OBJ rejection, double frees and live counts
  are checked throughout.
- 1,000 partially encoded RGBA8 failures and 1,000 malformed PAL4 failures, followed by
  successfully filling all sixteen banks while preserving the live BG's palette slot.
- All 256 texture slots occupied, followed by destruction and successful allocation of
  all 511 usable tiles; full-store and fragmented-run rejection followed by exact-hole reuse.
- 1,000 one-entry `TextureCache` LRU evictions, checking current sprite pixels every time
  and zero remaining transient textures after final eviction.

`make level` covers failed tileset/map uploads, replacing an already-loaded level,
eight supported retained pairs, overflow without allocation, and revisiting after overflow.

`make project-check` now also runs the freshly generated and tier-0-baked project's own
flow/maps/prefabs/sprites through 1,000 title → level revisits on the fixed-point PPU host
model. Every full framebuffer hash is identical (`a1d03c7c`), entity/sprite counts are
stable, exactly **seven textures** remain live, and persistent arena usage stays at
**306,872 bytes**, including the host-only 153,600-byte PPU composition buffer. The real
GBA profile remains 160 KB; only that documented host framebuffer allowance is added.
The stress path is in `tests/suites/project_entry.cpp`, invoked by `project-check` with
`RELOAD_STRESS=1`; it also verifies that leaving the level removes all its entities.

Validation: `make check`, `make determinism`, `make sanitize`, `make release`, CMake
PPU/level tests, `make size-gate` and the generated project's
`make game-gba PROJECT=build/projcheck/game` pass. The platformer ROM uses 125,980 bytes,
17,460 bytes static IWRAM and 8 bytes static EWRAM. The generated project ROM uses
307,792 bytes, 21,104 bytes static IWRAM and 8 bytes static EWRAM. The PPU model stress
verifies pixel behavior on the host; these cross builds verify compilation and size,
without claiming an on-device reload soak.
