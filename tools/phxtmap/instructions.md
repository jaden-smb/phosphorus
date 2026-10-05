# phxtmap — the tilemap editor

## What it is for

Level layout for Phosphorus games: tile layers drawn with the **real tileset art**, per-tile
**collision flags**, **entity spawns**, **parallax** factors and map size. Maps are saved as the
open **Tiled `.tmj`** author format, never as engine blobs; `phxtile`/`phxpack` bake what it saves
(docs/08 §1). Anything it writes opens in the full Tiled editor, art included, and the reverse is
true too.

`phxtmap` is **Phosphorus Studio's map editor in a window of its own**. It hosts the Studio's map
panel (`tools/phxstudio/ed_map.cpp`) in a one-document shell (`tools/phxstudio/solo.h`), so the
tools, keys and file handling are identical in both. Inside the Studio, open any `.tmj` from the
Explorer instead ([Studio guide](../phxstudio/instructions.md#tilemap-editor)).

## How it works

It **dogfoods the engine**: the same App loop, SDL window, software renderer and tool widget kit
(`tools/common/twk.h`) as the Studio and the games. Each tile layer is composited on the CPU into
one RGBA image that updates cell by cell as you paint, then drawn as one clipped, zoomed sprite,
so large maps stay cheap. The document model (`editor.h: TmapDoc`) is separate from the GUI and
unit-tested headlessly in the pipeline and editors suites. Those tests cover load → edit → save →
re-import through the bake's own `tiled_load`, including parallax, spawns, collision flags, the
tileset image, layer edits, resize, stamps and undo.

The tileset image is found from the map's tileset `image` (relative to the `.tmj`, as in Tiled).
Failing that, it looks for `<tileset>.png` next to the map, then any PNG in the repo with that
name. Without one, tiles show as coloured swatches; the baked game still uses the real art.

## Build & run

Needs SDL2 and a display (not part of `make check`).

```bash
make tmap                                   # -> build/phxtmap

./build/phxtmap level.tmj                   # edit a map (Ctrl+S saves in place)
./build/phxtmap --out copy.tmj level.tmj    # edit one file, save to another
./build/phxtmap --out new.tmj --size 32x20  # a NEW 32x20 map (saved on the first Ctrl+S)
./build/phxtmap --out new.tmj --size 64x18 --tile 8x8 --tileset art/tiles.png
./build/phxtmap --prefabs prefabs.json …    # place the prefab table's types as spawns
./build/phxtmap --types door,key …          # extra spawn types
./build/phxtmap --scale 3 …                 # UI scale (the window is resizable)
```

**Spawn types are never hardcoded into your workflow.** The placeable list is:

- the `--prefabs` table's string `type`/`name` column (the shared prefab schema: one phxbin table
  defines the game's entity kinds and stats, both editors read it, and the game matches spawns
  against `fnv1a(record.type)`),
- `--types`,
- player/coin/enemy/spike,
- every type the map already uses.

In the Studio, the list is every prefab table in the repo.

`PHX_MAX_FRAMES=120 ./build/phxtmap …` runs a bounded smoke (boots, runs, exits).
`--shot out.ppm` writes the window after 45 frames and quits.

## Controls

| Input | Action |
|---|---|
| **B** brush | paint the selected tile, or a multi-tile **stamp** (drag across the palette, or Ctrl+C a selection). Drags paint cell by cell |
| **E** eraser | clear cells. **Right-drag** erases with any tool |
| **G** fill | flood-fill the connected area of the clicked tile |
| **R** rectangle | drag a box; releasing fills it |
| **I** picker | click a cell for its tile; **drag a box** to pick a stamp |
| **S** select | drag a box. **Ctrl+C** makes it the brush stamp; **Delete** clears it |
| **T** spawns | click empty space to place the selected type (snapped to the grid; **Shift** = free), click a spawn to select it, drag to move, **Delete** removes |
| **H** or middle-drag | pan. **Ctrl+wheel** zooms about the pointer, the wheel scrolls, **F** fits |
| **Tab** / Shift+Tab | next / previous layer |
| **V** | cycle the brush tile's collision: none → solid → one-way → hazard |
| **Ctrl+Z** / Ctrl+Y | undo / redo (one step per stroke or edit; resize included) |
| **Ctrl+S** | save. Closing the window with unsaved edits asks first |

The side panel has four tabs:

- **Tiles**: the tileset palette. Click for a tile and drag for a stamp. Right-click a tile to set
  its collision (or use the none / solid / one-way / hazard buttons for the brush tile, or **V**).
  A map without a tileset image gets **create tileset…**, which writes a PNG of tiles next to the
  map and links it. Painting the tiles themselves happens in Phosphorus Studio: double-click a tile
  there to open the tileset zoomed on it, and the map redraws when the PNG is saved
  ([Studio guide](../phxstudio/instructions.md#tilemap-editor)).
- **Layers**: visibility (editor-only), add, delete, reorder, rename, and horizontal/vertical
  parallax (1 = moves with the world, 0 = fixed to the screen).
- **Spawns**: the type to place, the spawn list, and an inspector (name, type, x/y, w/h, delete),
  plus **per-spawn properties**: name, type (int/float/bool/string) and value, saved as Tiled
  custom properties and baked for the game. One named like a prefab column overrides it for that
  spawn.
- **Map**: size (the **resize** dialog anchors the old map, and spawns move with it), the tileset
  name, and the tileset image.

The toolbar toggles the grid, the collision overlay, spawns, dimming the other layers, and the
**parallax preview** (each layer scrolls by its factor as you pan; painting is paused while it
is on). The status line shows the map size, layer, spawn count, zoom and the cell under the
pointer.

## Conventions it preserves

- The **last tile layer is the gameplay layer** that the games' physics reads. Earlier layers are
  backdrops and may carry `parallaxx`/`parallaxy`. Names and factors survive load → save.
- Spawn objects keep their `name`, `type` (baked to the type hash the game switches on), `x`, `y`,
  `width` and `height`.
- **Collision flags** live on the tileset as per-tile boolean `properties` (`solid`, `oneway`,
  `hazard`). Tiled's per-tile `class` strings of the same names import too. The bake turns them
  into the per-tile collision table (`TileGrid.flags`). A map *without* flags falls back to
  "every non-empty tile on the gameplay layer is solid", which the overlay shows.
- The tileset's `image`, `columns`, `tilecount` and image size are written when known, so Tiled can
  open the map with its art. The **tileset name** (= the PNG's file stem) is what the bake uses to
  find the texture.

## Typical workflow

```bash
make tmap
./build/phxtmap --out mylevel.tmj --size 32x20 --tileset tiles.png
#   paint ground on the last layer, T -> place player/coins/enemies, Ctrl+S
./build/phxtile --out mylevel.phxtmap mylevel.tmj   # bake
./build/phxpack --out assets.phxp tiles.png mylevel.phxtmap
```
