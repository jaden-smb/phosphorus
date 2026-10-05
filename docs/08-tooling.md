# Phosphorus Engine — Tooling & Asset Pipeline

> `tools/` — host-only C++17 (STL freely allowed here; these never ship to console).
> They turn author-friendly formats into the baked `.phxp` blobs the runtime reads in place.
> Philosophy: **do all the expensive, fallible work offline.**

## 1. Tool inventory

| Tool        | Input → Output                         | Role                                  |
|-------------|----------------------------------------|---------------------------------------|
| `phxsprite` | PNG (+ slice json) / `.font` / `.fnt` → `.phxspr` | sprite/atlas + animation slicing; fonts |
| `phxtile`   | Tiled `.tmj` → `.phxtmap`               | tilemap + layers + collision baking   |
| `phxsnd`    | WAV / `.sfx` / `.song` → `.phxsnd`     | audio: mono 16-bit PCM (GBA resampled at bake, see §5) |
| `phxbin`    | JSON → `.phxbin`                       | data tables → optimized binary         |
| `phxpack`   | the above `+` → `assets.phxp`          | bundle assembler (sorted TOC, optional LZ77; see §2 for what's actually per-target) |
| `phxtmap`   | GUI tilemap editor → `.tmj`            | authoring (wraps Tiled-compatible fmt); the Studio's map panel standalone |
| `phxentity` | GUI data-table editor → `.json`        | record/prefab authoring; the Studio's table panel standalone |
| `phxstudio` | Phosphorus Studio (the editor)            | code, sprite/pixel, tilemap and table editors + module graph, per-tier asset previews, one-click games/gates/suites (§10) |

(No tool accepts XML/`.tmx` input today, despite some of the design language below — Tiled
maps are `.tmj`/JSON only. ADPCM/8-bit-at-bake for `phxsnd` is target design, not built either
— see §5's "As built" note.)

`phxsprite/phxtile/phxsnd/phxbin` are the **converters**; `phxpack` is the
**assembler**; `phxstudio` (and its standalone panels `phxtmap/phxentity`) are the **editors**. Editors output author formats
that the converters then bake — editors never write engine blobs directly (keeps the
bake path single and testable).

## 2. `phxpack` — the bundle assembler

```
 phxpack --target {gba|psp|pc} --in assets/ --out assets.phxp [--compress auto]

 assets/                         build graph                    assets.phxp
 ├── hero.png ──► phxsprite ──► hero.phxspr ─┐
 ├── tiles.png ─► phxsprite ──► tiles.phxspr ┤
 ├── level1.tmj ► phxtile  ──► level1.phxtmap┼─► phxpack ─► [hdr][TOC][blobs]
 ├── music.wav ─► phxsnd   ──► music.phxsnd  ┤    (per-target encode + optional LZ77)
 └── items.json ► phxbin   ──► items.phxbin ─┘
```

Responsibilities:
- Resolve names → FNV-1a hashes, build the **sorted TOC** (§`docs/06`).
- Apply compression where it pays (`--compress auto` measures and keeps the smaller).
- Stamp the bundle `target` byte for the mount-time tier check (`docs/06` §2), and
  refuse to merge an intermediate baked for a *different* tier (blobs are per-target
  encoded, so a tier mix-up fails at pack time, not on a console).
- **Per-target texture encode** (`tools/phxpack/tex_encode.h`, docs/06 §4): `--target 0`
  bakes 4bpp paletted tiles + palettes (the GBA PPU's native layout), `--target 1` bakes
  GU-swizzled RGBA8, `--target 2` keeps RGBA8. The sound counterpart is the tier-0
  18157 Hz resample (§5). Both run inside `BundleWriter`, so converters get them too.
- **Lock file** (`<out>.lock`, `tools/phxpack/lock.h`): tool + format versions, per-input
  content hashes, per-input asset lists, output CRC32 — drives the incremental rebake
  (unchanged inputs are reused from the previous bundle), the "up to date" skip, CI
  stale-bundle detection, and `--upgrade` (re-bake from the recorded source list).
- **Manifest** (`--manifest` → `<out>.manifest.txt`): human-readable
  hash ↔ name ↔ source-path table for dev builds (hashes are one-way; this is the map).

## 3. `phxsprite` — sprites, atlases, animation, fonts

**Fonts** are atlases too. `phxsprite` bakes a `.font` or a BMFont `.fnt` to the sheet Texture
plus a Font asset: the glyph table in `phx/resource/bundle.h`, read in place by
`phx/runtime/font.h`.
- A `.font` is JSON over a grid sheet. It sets the cell size and the first character, plus
  `proportional` / `spacing` / `space` or a fixed `advance`, and `line_h`. Proportional widths
  are measured from each glyph's opaque columns at bake (`tools/phxpack/font.h`, shared with
  Phosphorus Studio's font editor).
- A `.fnt` is BMFont's text export (BMFont, Hiero, Littera...): each character's rect, offset
  and advance, from one page.
- TrueType is not rasterized: export a pixel font to `.fnt` (or draw one in the Studio) first.


Input: a PNG plus an optional sidecar describing slices/animations:

```json
{
  "image": "hero.png",
  "tile":  16,
  "palette": "auto",                 // build a shared 16-color palette (GBA)
  "animations": {
    "idle": { "frames": [0,1],       "fps": 4,  "loop": true },
    "run":  { "frames": [2,3,4,5],   "fps": 12, "loop": true },
    "jump": { "frames": [6],         "fps": 1,  "loop": false }
  },
  "transitions": [                   // the animation state machine (optional)
    { "from": "idle", "to": "run",  "on": "move" },
    { "from": "*",    "to": "jump", "on": "jump" },
    { "from": "jump", "to": "idle", "on": "land" }
  ]
}
```

(The line-based `.sprdef` says the same with `clip` and `trans <from|*> <to> <trigger>` lines;
tools/phxsprite/instructions.md.)

Output `.phxspr` blob: the atlas texture **per-target encoded** (shared `BundleWriter`
path, so `--target 0` emits 4bpp paletted tiles, `--target 1` swizzled RGBA8 — see §2 and
docs/06 §4) + a frame table + an animation table consumed directly by `engine/anim`, and
(when authored) a transitions trailer whose clip names were resolved to indices at bake time.

**Bake-time GBA palette quantization is implemented** (`tools/phxpack/tex_encode.h`): the
tier-0 encoder runs the same quantizer as the GBA PPU upload path (≤15 opaque colours per
16-colour palette, whole-texture palette when possible for OBJ use, per-tile palettes
otherwise) and its output composes the exact same frame (asserted by `make ppu`). Art the
tier can't express (a non-8px-aligned atlas, >15 colours in one 8×8 tile) is reported at
bake time and kept as RGBA8 — the PPU backend then applies its upload-time quantizer or
rejects it exactly as before, so nothing that would fail on hardware slips through
silently.

## 4. `phxtile` — tilemaps & collision

Reads the open **Tiled JSON export** (`.tmj` — so artists can use a mature editor) and
bakes:
- tile index layers (one per BG layer; GBA caps at 4),
- an optional per-layer Q16 parallax factor (imported from Tiled's `parallaxx`/`parallaxy`),
- object layer → entity spawn list (positions + prefab refs for `phxentity`).

**Collision comes from the map two ways.** The zero-setup convention is that **the last
tile layer IS the collision layer**: `engine/physics` treats any index `>= solid_from` in
that layer as solid (`docs/10-gameplay-systems.md`; both example games set
`solid_from = 1`). On top of that, **per-tile collision metadata** is now baked when the
tileset carries it: Tiled tileset per-tile boolean `properties` (`solid`, `oneway`,
`hazard`) or per-tile `class` strings of the same names become an optional per-tile flags
table in the Tilemap blob (`kTilemapHasTileFlags`, `phx/resource/bundle.h`), which
`TilemapView.tile_flags` serves straight into the physics `TileGrid`. That lets decorative
non-solid tiles, one-way platforms, and hazards share the gameplay layer — the thing the
bare "last layer wins" convention couldn't express. Maps without metadata behave exactly
as before. (`.tmx`/XML input remains unbuilt — export `.tmj` from Tiled.)

```
 Tiled .tmj                        .phxtmap
 ┌────────────────┐                ┌──────────────────────────────┐
 │ tileset        │                │ hdr: w,h,tilew,tileh,layers   │
 │  ├ tile props  │               │ layer[i]: u16 indices         │
 │ layer: bg      │  phxtile ──►  │ (+ optional per-layer parallax│
 │ layer: main    │               │    factor table)              │
 │ objects        │               │ (+ optional per-tile collision│
 │                │               │    flags: solid/oneway/hazard)│
 └────────────────┘                │ spawns: {type_hash, x,y,w,h}  │
                                   └──────────────────────────────┘
```

## 5. `phxsnd` — audio bake

| Target | Encoding (target design)          | Why                                  |
|--------|-----------------------------------|--------------------------------------|
| GBA    | 8-bit signed PCM, downsampled     | DirectSound DMA wants raw 8-bit; RAM |
| PSP    | ADPCM (4:1)                       | GE/audio-friendly, fits 32 MB        |
| PC     | 16-bit PCM (SFX), OGG ref (music) | quality, ample RAM                   |

> **As built:** all targets store mono 16-bit PCM; the per-target step implemented so
> far is the **tier-0 (GBA) bake-time downsample to the 18157 Hz vblank-locked device rate** (Q16
> linear, deterministic) — the runtime downmixes 16→8-bit at the DMA buffer. ADPCM
> and OGG music refs are future encoders behind the same `--target` switch.

Output `.phxsnd`: header (rate, frames) + samples. Music can be streamed by the
runtime instead of fully residing (`docs/06` §5).

**Synthesized sources.** Besides WAV, `phxsnd` bakes a **sound effect** (`.sfx`: an sfxr-style
parameter set) and a **song** (`.song`: a small pattern tracker with instruments, patterns and an
order list). `tools/phxpack/synth.h` renders them to PCM at 22050 Hz, and they then take exactly
the WAV path: the same Sound asset and the same tier-0 resample. Nothing is synthesized at
runtime. A song is one rendered loop, which the music bus loops, as Emberwing's baked theme does.
The renders are deterministic (an LFSR for noise, seeded presets). Phosphorus Studio's sound effect
and song editors play the exact PCM the bake produces.

## 6. `phxbin` — JSON → binary tables

Game data (item stats, dialogue, tuning) authored as **JSON only** (an XML input path was
part of the original design — "the same backend via a small XML→intermediate step" — but
was never built; `tools/phxbin/main.cpp` takes a `.json` argument, full stop), baked to a
flat binary the engine reads as a `BlobView` with a generated accessor struct:

```
 items.json ──► phxbin ──► items.phxbin   (array<ItemRecord>, fixed stride)
                              + items.gen.h (POD struct matching the layout)
```

No runtime JSON parser ships. The generated header guarantees the struct and the blob
agree (versioned).

Field types: `u8 i8 u16 i16 u32 i32 f32`, plus **`str8 str16 str32`** — an inline
NUL-terminated `char[N]` (overlong values truncate to N−1). A string column **names**
records, which is what turns a stats table into a **prefab schema**: the game hashes the
name (`fnv1a`) to match baked spawn types, and `phxtmap --prefabs table.json` reads the
same table as its placeable-entity vocabulary — one author file shared by both editors
and the bake.

## 7. `phxtmap` — Tilemap Editor (GUI)

A desktop tool built **on the engine itself**: the same App loop, SDL window and software renderer
the games use, with the tool widget kit (`tools/common/twk.h`) on top. That is dogfooding, with no
external UI toolkit. It is **Phosphorus Studio's map editor** (§10) in a window of its own: one panel
(`tools/phxstudio/ed_map.cpp`), two hosts. The document model (`tools/phxtmap/editor.h`) is
unit-tested headlessly. Usage and controls: `tools/phxtmap/instructions.md`.

```
 ┌───┬───────────────────────────────────────────┬──────────────────────┐
 │ ✎ │ [layer: main (gameplay) ▾] # ▦ ⚑ ◌ ≋  fit │ Tiles Layers Spawns Map│
 │ ⌫ │  ┌─────────────────────────────────────┐  │ ┌─┬─┬─┬─┬─┬─┬─┬─┐    │
 │ ▣ │  │  layers composited on the CPU, drawn │  │ ├─┼─┼─┼─┼─┼─┼─┼─┤    │
 │ ⊞ │  │  as one zoomed image each; collision │  │ └─┴─┴─┴─┴─┴─┴─┴─┘    │
 │ ⌖ │  │  overlay, spawns, stamp ghost        │  │ collision: none solid│
 │ ⚑ │  └─────────────────────────────────────┘  │  1-way hazard        │
 └───┴───────────────────────────────────────────┴──────────────────────┘
```

Built today:

- Multi-layer painting with the **real tileset art**. The tileset image round-trips as Tiled's
  `image`, so Tiled opens the map with its art too.
- **Brush / eraser / fill / rectangle / picker / select** tools and **multi-tile stamps** (drag
  across the palette or copy a selection).
- **Collision flags** per tile: solid / one-way / hazard, shown as an overlay on the gameplay
  layer and saved as Tiled per-tile properties.
- **Spawns**: place, select, drag and inspect name/type/position/size. Placeable types come from
  the prefab tables (§8), not hardcoded.
- **Layers**: add, delete, reorder, rename, parallax factors, and a parallax preview.
- **Map resize** with an anchor (spawns move with it).
- Bounded **undo/redo** per gesture, zoom and pan, and positioned load errors (`line L, col C`).

It saves Tiled-compatible `.tmj` and `phxtile` bakes it. Compatibility with Tiled means users
aren't locked into our editor.

## 8. `phxentity` — Entity / Prefab Editor (GUI)

**Phosphorus Studio's data-table editor** (§10, `tools/phxstudio/ed_table.cpp`) in a window of its
own. It is a spreadsheet over the phxbin author JSON (typed record tables).

- **Built today:**
  - **In-cell editing**: Enter/F2, typing, or double-click. Tab/Enter navigate.
  - Every value is **clamped or clipped to what the baked struct holds**: integers to their type,
    `f32` rounded to float, `strN` to N−1 characters. String cells are now fully editable.
  - **Schema editing**: add, rename, retype (values convert), reorder and delete fields. Insert,
    duplicate, reorder and delete records. Snapshot undo covers all of it.
  - A **record inspector**, and table checks for duplicate names, over-long text and the baked
    size.
  - `--new NAME --fields a:type,b:type` starts a fresh table.
  - Malformed input is refused with a positioned `line L, col C` parse error.
  - The document model (`tools/phxentity/editor.h`) is unit-tested headlessly, and its output is
    proven to re-bake through the real `phxbin` builder. See `tools/phxentity/instructions.md`.
- **Built today — the shared prefab schema:** string fields (`str8/str16/str32`, §6) let a
  record table carry a `type` name column. That one table is the seam between the tools:
  - the table editor edits the stats;
  - the map editor places the named types as spawns (every prefab table in the repo inside the
    Studio; `phxtmap --prefabs table.json` standalone);
  - the bake hashes the spawn type, and the game matches it against `fnv1a(record.type)` from the
    baked table.

  A third party defines a new entity kind by adding one JSON record, with no source edits.
- **Planned on top:** component schemas **introspected** from a reflection table
  (`PHX_REFLECT(Component, fields...)`) so prefabs become named component lists with
  defaults, and placing one in the map editor writes `{prefab_hash, x, y, overrides}`.
- Outputs `.json` → `phxbin` bakes it into tables → the game reads them zero-copy.

## 9. Pipeline guarantees

**True today, and asserted:**
- **Determinism, checked:** the bake path is a pure function of its inputs (no clock
  reads, no nondeterministic ordering) — same sources in, same bundle bytes out. The
  pipeline suite diffs two independent bakes of the same fixture byte-for-byte, and
  `make phxpack` proves an incremental rebake equals a `--full` rebake byte-for-byte.
- **Offline validation:** a broken prefab ref, a malformed `.tmj`/JSON, a missing
  referenced file, or GBA palette overflow in a single 8×8 tile (§3 — now caught by the
  tier-0 bake encoder) fails or warns at the *bake*, never surprises the *game*.
- **Round-trip tested:** the pipeline/resource suites (`make pipeline`, `make resource`,
  `make tools`) bake fixtures and assert the runtime `ResourceCache` reads back identical
  views for every type, on every target encoding (including PAL4/swizzled textures).
- **Incremental, via the lock file** (§2, `docs/06` §8): unchanged inputs are reused from
  the previous bundle; an unchanged input list skips the bake ("up to date"); the
  `<out>.lock`'s recorded output CRC32 lets CI flag a stale/hand-edited bundle; and
  `--upgrade` re-bakes a bundle from its own recorded source list. `--full` opts out.

## 10. `phxstudio` — Phosphorus Studio

The one editor for the engine, built on the engine like everything else here (the feasibility
study's Option A, `docs/gui-editor-feasibility.md`). One window, four views:

- **Editor**: an Explorer over the repo, tabs of documents, quick open, and New-asset templates.
  Each document opens in its own editor:
  - **code**: a syntax-highlighted text editor with find/replace, and compiler errors from the
    Run view in its gutter;
  - **sprite / pixel**: paint PNG sheets with GBA colour and 8×8-tile checks; frame grid, onion
    skin, named clips with a live preview;
  - **tilemap**: §7;
  - **data table**: §8.
- **Overview**: the module graph (`depcheck.py` layers + real `#include` edges) and the
  capability tiers (`caps.h`).
- **Assets**: every `.phxp`, validated as `ResourceCache::mount()` would, with previews. Textures
  are re-encoded per render tier by the bake's own `tex_encode.h` and sampled by the software
  golden renderer, so the GBA view is the real 4bpp/BGR555 result. Sprite clips play, tilemaps
  use the real parallax path, and sounds play on the real mixer.
- **Run**: games, editors, gates, every `make check` suite and the console builds as child
  processes, with live output.

The Studio works on **one game project at a time**: a folder with a `phxproject.json` (name,
code/asset folders, bundles, Run-view launches). While a project is open it writes only inside that
folder. It can read the engine's public API headers (`engine/*/include`) and docs, and opens nothing
else. This is `projectdoc.h: AccessPolicy`, applied to every open and save and asserted in
`make editors`. `--engine-dev` is the engine-maintenance mode: the whole checkout, plus the module
graph and the gates. Projects build with the engine's generic rules
(`make game | game-assets | play PROJECT=path`) against the public headers only. The asset step
(`tools/common/bake_project.py`) runs the converters above and then `phxpack`.

The editors write **author formats only** (`.png`, `.sprdef`/sprite `.json`, `.tmj`, phxbin
`.json`, source). The bake stays the single writer of engine blobs (§1). Every saved form is
re-read by the bake's own loaders in `make editors`.

The pieces are:

- the desktop-only platform seam extension `phx/platform/desktop.h` (keys, text, mouse
  buttons/wheel, clipboard, resizable window; `sdl` + a scripted `null`, no console backend);
- the widget kit `tools/common/twk.h`;
- headless document models: `textdoc.h`, `syntax.h`, `pixeldoc.h`, `project.h`,
  `tools/common/png_write.h`, and the map/table models;
- editor panels behind a `Host` interface (`host.h`), hosted by the Studio or by the standalone
  one-document shell (`solo.h`).

The models are asserted in `make editors` and `make pipeline`. Usage and controls:
`tools/phxstudio/instructions.md`; `make studio && ./build/phxstudio`.
