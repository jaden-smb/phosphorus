# phxpack — the bundle assembler

## What it is for

`phxpack` produces the **`.phxp` bundle** — the single packed asset file a Phosphorus game mounts
at runtime (zero-copy on PC/PSP, linked into the ROM on GBA). It is the final stage of the
two-stage pipeline (docs/08): the per-format converters (`phxsprite`/`phxtile`/`phxsnd`/
`phxbin`) bake author sources into intermediate `.phx*` files, and `phxpack` **merges** those
into one bundle. It can also **bake author sources directly** (it shares the same builder code),
which is convenient for small projects and tests.

## How it works

Every asset becomes a `[name-hash → typed blob]` entry in a sorted TOC. Names are FNV-1a hashes
of the asset name (games look assets up with `"hero"_hash`). With `--compress`, each blob is
LZSS-compressed independently and kept compressed **only where that shrinks it** — the runtime
reader decompresses once into its arena and serves uncompressed blobs zero-copy, so compression
never costs correctness or makes a bundle bigger. `--target` selects the capability tier and
drives per-target encoding (docs/06 §4): tier 0 resamples sounds to the GBA device rate and
bakes textures as 4bpp paletted tiles + palettes (the PPU's native layout); tier 1 bakes
textures in the PSP GU's swizzled block order; tier 2 keeps RGBA8/source-rate. Merging an
intermediate baked for a **different** target is refused (the blobs wouldn't decode).

Every bake also writes **`FILE.phxp.lock`** (tool + format versions, per-input content
hashes, per-input asset lists, output CRC32). On the next run, unchanged inputs are reused
from the previous bundle (per-asset **incremental** rebake — byte-identical to a `--full`
bake, asserted by `make phxpack`), and a fully unchanged input list skips the bake
("up to date"). CI can flag a stale bundle by checking the lock's recorded output CRC32.

## Build & run

```bash
make phxpack            # builds build/phxpack (and runs its CLI smoke)

./build/phxpack --out FILE.phxp [--target 0|1|2] [--compress|-z] [--manifest] [--full] <inputs...>
./build/phxpack --upgrade FILE.phxp [--target 0|1|2] [--compress]
```

| Flag | Meaning |
|------|---------|
| `--out FILE` | (required) the bundle to write |
| `--target N` | capability tier: 0 = GBA, 1 = PSP, 2 = PC (default 2) |
| `--compress`, `-z` | LZSS each blob, kept only where it shrinks |
| `--manifest` | also write `FILE.phxp.manifest.txt` — hash ↔ name ↔ source, for dev debugging |
| `--full` | ignore `FILE.phxp.lock`; re-bake everything from scratch |
| `--upgrade FILE` | re-bake FILE from the sources recorded in its lock (after a tool/format upgrade) |

## Accepted inputs (by extension)

| Kind | Extensions | Becomes |
|------|-----------|---------|
| sources (baked directly) | `.png` `.ppm` → Texture · `.tmj` (Tiled) → Tilemap + Spawns · `.tmcsv` → Tilemap · `.sprdef`/sprite `.json` → Texture + Sprite · `.wav` / `.sfx` / `.song` → Sound · `.font` / `.fnt` → Texture + Font · `.dlg` → Dialogue | engine assets |
| intermediates (merged) | `.phxspr` `.phxtmap` `.phxsnd` `.phxbin` `.phxp` | their contained assets, re-added by hash |

Asset names default to the input file's stem (e.g. `hero.sprdef` → `"hero"`).

## Examples

```bash
# canonical two-stage flow: merge converter outputs
./build/phxpack --out assets.phxp --target 2 hero.phxspr level.phxtmap tone.phxsnd items.phxbin

# direct bake, compressed, for a GBA ROM
./build/phxpack --out assets.phxp --target 0 -z tiles.png level.tmj jump.wav
```

Verified end to end by `make phxpack` (CLI smoke over every input type), `make pipeline`
(in-process), and `make tools` (real binaries, converters → merge).
