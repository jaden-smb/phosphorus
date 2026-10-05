# phxentity — the data table editor

## What it is for

A spreadsheet for the **phxbin author JSON**: the typed record tables games use for entity/prefab
parameters, item stats, tuning values and similar data. It edits the author format, never engine
blobs (docs/08 §1). `phxbin`/`phxpack` bake what it saves, and the editors suite proves an edited
table still bakes through the real `phxbin` builder.

`phxentity` is **Phosphorus Studio's table editor in a window of its own**. It hosts the Studio's
table panel (`tools/phxstudio/ed_table.cpp`) in a one-document shell (`tools/phxstudio/solo.h`).
Inside the Studio, open any phxbin `.json` from the Explorer instead
([Studio guide](../phxstudio/instructions.md#data-table-editor)).

## How it works

It **dogfoods the engine**: the same App loop, SDL window, software renderer and tool widget kit
as the Studio. The document model (`editor.h: BinDoc`) is separate from the GUI and unit-tested
headlessly. Tests cover load, typed cell edits, clamping, schema edits, undo, save, reload, and a
bake through `build_bin`.

**Every value you can enter is one the baked struct can hold**:

| Field type | Baked as | What the editor enforces |
|---|---|---|
| `u8 i8 u16 i16 u32 i32` | integers | typed values (decimal or `0x` hex) **clamp** to the type's range, and the Studio tells you |
| `f32` | `float` | rounded to float and saved with the shortest text that round-trips |
| `str8 str16 str32 str64` | NUL-terminated `char[N]` | clipped to N−1 characters. Text that is already too long (from hand-written JSON) shows red |

Field names and the struct name must be C identifiers, because they become the generated header's
struct and members.

## Build & run

Needs SDL2 and a display (not part of `make check`).

```bash
make entity                                # -> build/phxentity

./build/phxentity items.json               # edit (Ctrl+S saves in place)
./build/phxentity --out copy.json items.json
./build/phxentity --new Enemy --fields type:str16,hp:u16,atk:i8,speed:f32 --out enemies.json
```

`--new NAME --fields a:type,b:type` starts a **fresh table** from a schema. It opens with one
zeroed record and is written on the first Ctrl+S. The types are
`u8 i8 u16 i16 u32 i32 f32 str8 str16 str32 str64`.

**Prefab schemas:** give a table a string `type` (or `name`) column and it becomes the game's
prefab vocabulary. The map editor (`phxtmap --prefabs table.json`, or any map in the Studio)
places those types as spawns, and the game matches `fnv1a(record.type)` against the baked
spawn-type hash. One author file serves both editors. An existing input must have the phxbin shape
(`struct` + `fields` + `records`); a malformed file is refused with a `line L, col C` parse error:

```json
{ "struct": "ItemRecord",
  "fields": [ {"name":"id","type":"u16"}, {"name":"price","type":"u32"}, {"name":"weight","type":"f32"} ],
  "records": [ {"id":1,"price":100,"weight":2.5} ] }
```

`PHX_MAX_FRAMES=60 ./build/phxentity …` runs a bounded smoke. `--shot out.ppm` writes the window
after 45 frames and quits.

## Controls

| Input | Action |
|---|---|
| **arrows**, **Tab** / Shift+Tab, Home/End, Ctrl+Home/End, PgUp/PgDn | move the cell cursor |
| **Enter**, **F2** or **double-click** | edit the cell (the caret starts at the end) |
| **typing** | start editing, replacing the cell with what you type |
| while editing: **Enter** / **Tab** / **Esc** | commit and move down / commit and move right / cancel |
| **+** / **−** (Shift: ±10) | step a number |
| **Delete** | clear the cell (0 or empty) |
| **Ctrl+D** / **Ctrl+Enter** | duplicate the record / insert a zeroed record above |
| **right-click a column header** | rename, change type (values convert), move left/right, add field, delete field |
| **right-click a row number** | insert above/below, duplicate, move up/down, delete |
| toolbar | struct name, + record, insert, duplicate, delete, move up/down, + field |
| **Ctrl+Z** / Ctrl+Y | undo / redo (every edit, schema changes included) |
| **Ctrl+S** | save. Closing with unsaved edits asks first |

The grid scrolls both ways and numbers are right-aligned. The name column is green, and its header
says `name`. The **Record** panel edits the selected record with typed fields: drag or wheel a
number to scrub it, or click to type. The **Table** panel lists the prefab types and flags
duplicate names, over-long text, and the baked size per record.

## Typical workflow

```bash
make entity
./build/phxentity items.json                        # tune values, Ctrl+D to add records, Ctrl+S
./build/phxbin  --out items.phxbin --header src/items.gen.h items.json
./build/phxpack --out assets.phxp items.phxbin
```
