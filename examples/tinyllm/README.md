# tinyllm — a language model that runs on a Game Boy Advance

A 260K-parameter decoder-only transformer (llama2 architecture: RMSNorm, RoPE, grouped-query
attention, SwiGLU) generating English text on a **16.78 MHz ARM7TDMI with 256 KB of RAM and no
FPU** — the same inference code that runs on a desktop, producing the same tokens bit-for-bit.

```
+------------------------------+
|2.1 t/s  54tok  58/153       /|   <- amber status: throughput, tokens, context, busy marker
|Once upon a time, there was a |
|little girl named Lily. She   |
|loved to play with her toys   |
|and her friends. One day,     |
|Lily's mommy told her that she|
|was going to the park to play |
|with her friends              |
|                              |
|A run  B reset  START prompt  |
+------------------------------+
```

That is a real frame, decoded from the GBA's VRAM while the ROM was running under mGBA — not a
mock-up. Because the console is a tilemap, the screen reads back as text straight out of the
screenblock; the recipe is at the bottom of this file.

## The idea

The GBA has ~1/17,000th the RAM of the machine this model was trained on, no floating-point
unit, and no divide instruction. Three decisions make it work, and they are the whole project:

1. **The weights never enter RAM.** The quantized model is an opaque blob inside the `.phxp`
   bundle, which `bin2s` links into **cartridge ROM**. The inference core reads int8 weights
   *in place* off the cart bus. Nothing is decompressed, unpacked, or copied — the 316 KB model
   costs zero bytes of the console's 256 KB of EWRAM.
2. **Every number is an integer.** Activations are Q16.16, weights are int8 with per-group Q30
   scales, the KV cache is int16. There is no `float` anywhere on the device path — which is
   also what lets `make determinism` prove the PC (float `phx::scalar`) and GBA (fixed16) builds
   emit identical tokens.
3. **A token is not a frame.** A token takes ~380 ms on the console, so generation is a
   **resumable state machine**: `llm_advance()` performs one bounded transformer stage and
   returns, and the front end runs a couple of stages per frame. The console keeps redrawing
   while a token is in flight instead of freezing for half a second per word.

## Measured results (mGBA 0.10.5, emulating at 60.0 fps)

| | |
|---|---|
| **Throughput** | **2.17 tokens/sec** wall-clock (11.9 fps display, 2 stages/frame) |
| Inference-bound ceiling | 2.62 tokens/sec (381 ms/token, one whole token per frame) |
| Acceptance target | ≥ 1 tok/s — **met**. Stretch ≥ 5 tok/s — **not met**, see below |
| Context window | 153 tokens (derived from a 96 KB KV budget, not hardcoded) |
| The same code on x86-64 | ~3600 tok/s (0.28 ms/token) — the console is ~1600× slower |

The on-screen readout and the external wall-clock measurement agree (2.1 vs 2.17 tok/s); see
*Measuring time on a GBA* below for why that took work.

**Why not 5 tok/s.** Profiling (below) says the cost is where you would expect: streaming 260K
int8 weights off a 16-bit cartridge bus that has no data cache. The GBA's prefetch buffer only
serves *instruction* fetches, so every weight byte pays wait states. The one big win available —
pulling four weights per aligned 32-bit load instead of four byte loads — is implemented and
measured at **+8%** (413 → 381 ms/token). Getting to 5 tok/s would need a fundamentally cheaper
inner loop (hand-written ARM assembly with more registers live, or a 4-bit weight format), not
more tuning of this one.

## Memory budget

**Cartridge ROM (416.7 KB of the 32 MB cap; `make size-gate-tinyllm` gates it at 2 MB):**

| | bytes | |
|---|---:|---|
| model blob (`.phxllm`) | 323,132 | int8 weights + Q30 scales + RoPE/exp/SiLU tables + tokenizer |
| font tiles + bundle TOC | 6,448 | 4bpp paletted, per-target encoded at bake time |
| engine + example code | 97,148 | |
| **ROM total** | **426,728** | |

**EWRAM (the scarce resource: 256 KB total; the ROM asks the engine for a 200 KB arena):**

| | bytes | |
|---|---:|---|
| **KV cache** | 97,920 | 5 layers × 2 (K,V) × 153 positions × 32 kv_dim × int16 |
| logits | 2,048 | 512 vocab × Q16.16 |
| FFN activations (`hb`,`hb2`) | 1,376 | 2 × 172 × Q16.16 |
| top-p candidate index | 1,024 | |
| residual stream (`x`,`xb`,`xb2`) | 768 | 3 × 64 × Q16.16 |
| layer weight-pointer table | 880 | resolved once at init; points into ROM |
| attention scores, q/k/v, prompt, xq, scratch | 1,664 | |
| **inference core total** | **105,696** | reported by `llm_required_arena_bytes()` |
| console tile buffer | 1,200 | 30 × 20 × uint16 |

**IWRAM: 13.5 KB static of the 28 KB budget** (32 KB total). The four measured hot functions —
the int8 matvec, attention, RMSNorm, and the activation quantizer — run from IWRAM as ARM code
via the engine's `PHX_HOT_CODE`.

The KV cache is the only thing that scales with context, at **640 bytes per token**. `max_seq`
is *derived* at load time from `kv_budget_bytes` and the model header, so a bigger checkpoint
simply gets a shorter window instead of overflowing EWRAM.

## Build & run

```bash
make tinyllm              # headless: fetch + bake the model, generate, print to stdout
make tinyllm-sdl          # a real window rendering exactly what the GBA renders
make gba-tinyllm-ppu      # devkitARM -> build/gba/phx-tinyllm.gba   (the shipping ROM)
make size-gate-tinyllm    # ROM / IWRAM / EWRAM budget gate
make tinyllm-test         # the headless suite (also part of `make check` and `make determinism`)
```

`make tinyllm` fetches the 260K TinyStories checkpoint once (this tree tracks no binaries) and
runs the exporter over it. **With no network and no numpy it still works** — every target falls
back to a tiny generated fixture model, so a fresh offline clone builds and runs. To retry the
fetch: `rm -f build/tinyllm.phxllm build/stories260K.bin && make tinyllm-model`.

Environment knobs for the headless runner: `TINYLLM_MODEL` (a `.phxllm` path), `TINYLLM_PROMPT`
(0–3), `TINYLLM_TOKENS`, `TINYLLM_GREEDY=1`, `TINYLLM_SEED`.

### Controls (ROM and window)

| Button | Action |
|--------|--------|
| A      | run / pause generation (restarts once a generation has finished) |
| B      | reset the context and re-run the current prompt |
| START  | cycle to the next baked-in prompt |
| SELECT | toggle the frame-profiler overlay (matches the platformer's convention) |

## The exporter — `tools/export_model.py`

Full reference: [`tools/instructions.md`](tools/instructions.md) (usage, options, the complete
`.phxllm` layout), matching the convention every tool folder here follows. In short — the host
half. It does every expensive, fallible thing offline (Phosphorus's pipeline philosophy,
`docs/08`) so the console only streams int8.

```bash
# bake a checkpoint into a .phxllm
python3 examples/tinyllm/tools/export_model.py export \
        --checkpoint build/stories260K.bin --tokenizer build/tok512.bin \
        --out build/tinyllm.phxllm [--group-size 32]

# ...and record golden tokens for a fixed prompt
        --golden tests/fixtures/x.txt --prompt "Once upon a time" --steps 24

# how much did quantization cost?
python3 examples/tinyllm/tools/export_model.py eval --checkpoint ... --tokenizer ...

# regenerate the committed test golden from the C++-built fixture blob
python3 examples/tinyllm/tools/export_model.py golden --blob build/tinyllm_fixture.phxllm \
        --golden tests/fixtures/tinyllm_golden.txt --prompt "the cat sat on the mat" --steps 16
```

Inputs: the llama2.c legacy `.bin` export, or a PyTorch `state_dict` (`--checkpoint x.pt`, needs
torch; the `.bin` path needs only numpy).

**Swapping in a different checkpoint** needs no code change. Every shape — `dim`, `n_layers`,
`n_heads`, `n_kv_heads`, `vocab_size`, `seq_len`, `group_size`, `head_size`, `kv_dim` — lives in
the blob header and is validated at load; the runtime derives its context window and arena size
from them. Export it, point `TINYLLM_MODEL` at it (or drop it at `build/tinyllm.phxllm` and
rebuild the ROM), and check the arena fits: `llm_required_arena_bytes()` is printed at boot.

### Quantization scheme

- **int8, symmetric, group-wise** over the last (input) axis. Rows are zero-padded up to a whole
  group so the device kernel never has a ragged tail; `stride` (padded) and `cols` (logical) are
  both in the tensor directory.
- **Per-group scales are Q30, not Q16.16.** Measured on stories260K, group scales run
  5.9e-4 … 1.5e-2, and Q16.16's 1.5e-5 step would round the smallest of those to **0.9%
  relative error**. Q30's step is 9.3e-10. Activations stay Q16.16 — that is the domain the
  engine's own fixed-point math uses.
- **Activation quantization is per-vector, not per-group.** The vectors quantized here are always
  freshly RMSNorm'd or a SwiGLU product, so their within-vector range is narrow, and hoisting the
  scale out of the matvec removes an int64 multiply per group per output row.
- RMSNorm gains (Q16.16), the RoPE tables (Q15), and the tokenizer stay in higher precision —
  together they are ~2% of the blob and quantizing them costs real quality.

**Cost, measured, not assumed** (`export_model.py eval`, 96 held-out tokens):

```
  float32 perplexity        2.2501
  int8 fixed perplexity     2.2719
  delta                    +0.0218  (+0.97%)
```

Per-tensor error is 0.44–0.62% relative RMS. Greedy generations from the float and fixed paths
agree for the first 16 tokens on the reference prompt before diverging — both stay coherent.

### The `.phxllm` format

Normative definition: `src/llm_format.h` (the exporter mirrors it; struct sizes are
`static_assert`ed and pinned again by `tests/unit/test_tinyllm_format.cpp`).

```
[LlmHeader 64 B][LlmTensor directory, 32 B each][4-byte-aligned payloads ...]
```

The header carries magic/version, the full config, `kv_shift`, the RMSNorm epsilon, a CRC32, and
the tokenizer's BOS/EOS/max-piece-length. Each directory entry is `{name hash, kind, dtype,
layer, rows, cols, stride, data_off, scale_off, nbytes}`. Tensor kinds cover the weights, the
per-layer norms, the precomputed RoPE cos/sin tables, the exp and SiLU lookup tables, and the
tokenizer (concatenated pieces + offset index + a memcmp-sorted id table for binary search +
Q16.16 merge scores).

`llm_validate()` is the only thing between a truncated ROM image and an out-of-bounds read on a
console with no MMU, so it checks everything: size floor, magic, major version, header
self-consistency, `blob_size` against the buffer it was handed, directory bounds, and for every
tensor that its payload *and* its scale table lie inside the blob at 4-byte-aligned offsets with
a byte length matching `rows × stride × sizeof(element)`. Base-pointer alignment is a
correctness check, not a nicety: a misaligned 32-bit load on ARM7TDMI silently *rotates* instead
of faulting.

## The inference core

`src/llm.h` / `src/llm.cpp` / `src/fixedmath.h`. C++17, **no STL, no exceptions, no RTTI, no
heap** — all working memory is carved once from a caller-supplied `phx::ArenaAllocator`. No
platform or SDK headers; the only target-specific thing is `PHX_HOT_CODE`, the engine's single
isolated code-placement macro.

**ARM7TDMI has no divide instruction**, so `fixedmath.h` builds every reciprocal from multiplies
and shifts: a Newton–Raphson `q_recip()` (4 iterations, measured worst case 2 ULP of a 31-bit
mantissa = 9.3e-10 relative over a 20 M-divisor sweep), `q_div()`, and a bit-by-bit integer
`q_isqrt64()` feeding `q_rsqrt()`. `exp` (softmax) and SiLU are 513-entry Q16.16 tables **in the
blob** — power-of-two stepped, so the index/fraction split is a shift; interpolation error is
held under 1.3e-4 and 3.0e-4 respectively by unit tests.

Softmax does proper max-subtraction range reduction, then renormalizes the weights to Q15 so the
value-weighted sum accumulates in int32 with a bit of headroom instead of needing int64 per term.

The KV cache is int16 at `Q(kv_shift)`, with the shift **calibrated by the exporter** from the
K/V magnitudes a real prompt produces (stories260K: max 26.3 → `kv_shift = 9`, 2.4× headroom)
rather than hardcoded. Its layout is `[layer][K|V][kv_head][t][head_size]`, chosen so both
attention inner loops walk memory strictly forward within a head.

### API

```cpp
uint32_t llm_required_arena_bytes(const LlmHeader*, uint32_t kv_budget_bytes);
int32_t  llm_max_seq(const LlmHeader*, uint32_t kv_budget_bytes);
bool     llm_init(LlmContext&, const void* blob, uint32_t size, ArenaAllocator&, uint32_t kv_budget);
void     llm_reset(LlmContext&);
bool     llm_prefill(LlmContext&, const uint16_t* ids, int32_t n);
LlmStep  llm_advance(LlmContext&);      // ONE bounded stage: Working | Token | Idle | Full
LlmStep  llm_run_token(LlmContext&);    // blocking convenience, for tests and headless runs
int32_t  llm_encode(const LlmContext&, const char* text, bool add_bos, uint16_t* out, int32_t cap);
int32_t  llm_decode(const LlmContext&, uint16_t prev, uint16_t token, char* out, int32_t cap);
```

**Resumability is option (a) from the brief, and it is exact**: a token sliced across
`llm_stages_per_token()` separate `llm_advance()` calls produces byte-identically the same token
as running it in one go. The suite asserts that equivalence directly — it is what lets the ROM
interleave inference with the frame loop without changing a single output token. Stages are two
per layer (attention block, feed-forward block) plus one for the final norm, output head and
sampling: 11 for this model.

Sampling is greedy/argmax, or fixed-point temperature + top-p nucleus filtering driven by a
seeded xorshift32, so a (prompt, seed) pair reproduces exactly on either scalar tier.

The tokenizer is llama2.c's BPE, allocation-free: binary search over the blob's sorted piece
table, greedy highest-score pair merging in the caller's buffer, and both display rules on decode
(strip the dummy leading space after BOS; expand `<0xNN>` back to a raw byte).

## Rendering

The screen is **one Mode-0 BG tilemap used as a 30×20 text console** — the cell value *is* the
glyph index into the baked font atlas. That is the native way to put 600 characters on a GBA:
OBJ sprites cap at 128, so `ui.text` could not draw a screen of prose.

The font (`src/text_font.h`) is a 5×7 bitmap covering **all** printable ASCII including
lowercase — the shared `tools/common/debug_font.h` is uppercase-and-digits only, which cannot
render what a language model emits. It bakes into a 128×96 atlas holding the 96 glyphs twice,
white and amber, because the PPU cannot tint a background tile: the console picks a colour by
picking a tile index. Three colours total, so the tier-0 bake encodes it as 4bpp paletted tiles.

Words are soft-wrapped, and the partially-typed word is drawn *tentatively* each frame so the
newest text appears the instant a token lands rather than waiting for the space that ends it.

## Testing

| | |
|---|---|
| `tests/unit/test_tinyllm_math.cpp` | reciprocal/div/isqrt/rsqrt against exact rational arithmetic; exp and SiLU LUT error bounds; int8 quantizer round-trip, symmetry, padding, and all-zero handling |
| `tests/unit/test_tinyllm_format.cpp` | struct layout; ~30 targeted corruptions each asserted to be *refused*; arena sizing derived from the header; init rejecting an undersized arena, a useless window, and a shape-mismatched directory |
| `tests/unit/test_tinyllm_tokenizer.cpp` | encode/decode round-trip, merges firing, byte fallback, sorted-table invariants, overflow and output-cap boundaries |
| `tests/unit/test_tinyllm_font.cpp` | every glyph's cell value against the actual baked atlas pixels |
| `tests/suites/tinyllm_test.cpp` | end-to-end golden tokens, the resumable state machine, KV-budget independence, and the whole example under the real App loop with a mounted bundle |

**The golden token file is the load-bearing assertion.** `tests/fixtures/tinyllm_golden.txt` was
produced by a *completely independent* implementation of the same integer arithmetic — the Python
reference in the exporter — reading the exact fixture blob the suite builds. Two implementations
agreeing bit-for-bit is the only practical way to catch a fixed-point bug before it reaches a ROM.
It pins greedy tokens, temperature+top-p tokens, and the prompt's encoding.

The fixture model is **generated, not committed** (this tree tracks zero binary files): the C++
builder in `src/llm_build.h` makes a deterministic ~11 K-parameter model from an integer PRNG,
and the golden records the blob's size and CRC32 so a changed fixture reports as a changed
fixture instead of as a mysterious token mismatch. Regenerate both with `make tinyllm-fixture`
(needs numpy; `make check` does not).

`make determinism` runs the whole suite on both scalar tiers and diffs it byte-for-byte.

## Notes and gotchas

- **Measuring time on a GBA.** The platform's `clock_ns()` is a *virtual* clock (one microsecond
  per read, one sim step per vblank), so a "run stages until 12 ms are gone" budget silently
  never expires there — it swallowed a whole token per frame. Worse, `gba_pump_events()` falls
  back to one step per loop iteration when nothing is counting vblanks (the VBlank ISR is
  installed by the *audio* path, which this ROM does not use), so elapsed sim steps stopped
  meaning elapsed time and the status line read ~3× high. Fixed by adding
  `phx_gba_vblank_clock_start()` to the GBA backend — the same IRQ install without the sound
  hardware — after which the on-screen number matches the wall clock. Any compute-heavy GBA ROM
  wants this.
- **Pick the stage count so a frame stays under the platform's 5-vblank catch-up clamp**,
  or elapsed steps under-count again. Two stages/frame keeps a frame at ~4 vblanks.
- **Tilemap cell values are 1-based**: value 0 means EMPTY and value *v* selects tileset tile
  *v−1*, identically in the software backend and the PPU. Returning the raw glyph index rendered
  every character as the one before it — a bug invisible to a test that only counts non-empty
  cells. `test_tinyllm_font.cpp` now pins the mapping against the atlas.
- **Mount the ROM bundle with `verify_checksum=false`**: the default CRC32 runs over the whole
  ~322 KB bundle, which is a visible pause on a 16 MHz CPU. It is self-baked and ROM-resident;
  structural checks still run, and the model carries its own CRC in its header.
- **The model is never LZSS-compressed.** It must be readable in place; decompressing 316 KB into
  EWRAM would consume the entire memory budget the design exists to protect.
- **mGBA writes `<rom-basename>.ss0`** (note: *without* the `.gba`) and silently resumes from it,
  so after rebuilding a ROM you can be measuring the previous build. `rm -f build/gba/phx-tinyllm.ss0`.

### Reading the screen off emulated hardware

The console is a tilemap, so the screen can be read back as *text* — no pixel reconstruction
needed. With the ROM running under `QT_QPA_PLATFORM=offscreen mgba-qt -g rom.gba`:

```bash
arm-none-eabi-gdb -batch -ex "target remote :2345" \
  -ex "dump binary memory sb.bin 0x0600C000 0x0600C800" build/gba/tinyllm.elf
# screen entry & 0x3FF == the cell value (PPU tile base is 1); character = cell - 1 + 32
```

The ROM also publishes `phx_tinyllm_{ready,tokens,tps_x10,steps,ctx,ctx_max,arena,text}` as
C-linkage globals for the GDB stub, which is how the throughput numbers above were measured.
