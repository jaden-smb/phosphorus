# phxsnd — the audio converter

## What it is for

Bakes a **WAV file**, a **sound effect** (`.sfx`) or a **song** (`.song`) into a `.phxsnd`
intermediate holding one Sound asset — mono 16-bit PCM the runtime wraps as a `SoundView` and
plays through the software mixer. `phxpack` merges the `.phxsnd` into the final bundle.

`.sfx` and `.song` are synthesized first, by `tools/phxpack/synth.h` (the same code Phosphorus
Studio's sound effect and song editors play), at 22050 Hz:

- **`.sfx`** is JSON: the parameters of an sfxr-style generator. It holds `wave` (square, saw,
  triangle, sine, noise) plus pitch, slide, vibrato, arpeggio, duty, envelope, filter and volume
  values; every key is optional. See the header comment in `synth.h` for the full list.
- **`.song`** is JSON for a pattern tracker. It holds `bpm`, `rows_per_beat`, `channels`,
  `instruments` (a wave and an envelope each), `patterns` (rows of cells such as `"C-4 lead"`,
  `"off"` or `""`) and an `order` of pattern names. The song renders once through its order
  list; `play_music` loops it.

## How it works

Decodes RIFF/PCM WAV (8- or 16-bit, mono or stereo) with the pipeline's own decoder; stereo is
downmixed to mono, 8-bit is converted to signed 16. Then the **per-target encode** step runs:

- `--target 0` (GBA): the PCM is **resampled down to 18157 Hz at bake time** — the vblank-locked
  GBA Direct Sound device rate (924 CPU cycles/sample; 280896 CPU cycles/video frame ÷ 924 =
  exactly 304 samples/frame, so the DMA double-buffer swap never drifts against vblank; a
  non-locked rate like 16384 Hz gives a fractional 274.3125 samples/frame instead — see
  `tools/phxpack/bundle_writer.h`'s `kTier0Rate` comment). The cartridge carries ~2.4× less
  sample data (for 44.1 kHz sources) and the 16 MHz CPU mixes 1:1 instead of resampling every
  voice. Q16 linear interpolation, all-integer, deterministic.
- `--target 1|2` (PSP/PC, default 2): the source rate is kept — those mixers run at 44.1 kHz
  and resample cheaply at runtime.

## Build & run

```bash
make check              # builds build/phxsnd along the way (or: make tools)

./build/phxsnd --out FILE.phxsnd [--name N] [--target 0|1|2] <sound.wav | effect.sfx | music.song>
```

The asset name defaults to the input's stem; the game looks it up by that name
(`res->sound("jump"_hash)`) and plays it via the mixer / command queue.

## Example

```bash
./build/phxsnd  --out jump.phxsnd --name jump --target 0 jump.wav   # GBA-rate encode
./build/phxpack --out assets.phxp jump.phxsnd
```

Try it on the fixture `make check` drops: `./build/phxsnd --out /tmp/t.phxsnd build/p_tone.wav`.
Verified by `make audio` (decode → bake → mount → mix) and `make pipeline` (incl. the tier-0
resample: rate 18157, frame count scaled, samples intact).
