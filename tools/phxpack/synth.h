// tools/phxpack/synth.h — HOST-ONLY audio synthesis for the bake: SOUND EFFECTS from a handful of
// parameters (`.sfx`, an sfxr-style generator) and MUSIC from a small pattern tracker (`.song`).
// Both render to mono 16-bit PCM at kSynthRate and bake as ordinary Sound assets (build_sfx /
// build_song in builders.h), so the runtime needs nothing new: GameAudio::play() / play_music(), the
// flow table's `music` column, the tier-0 resample to the GBA device rate. Phosphorus Studio's SFX
// and song editors edit these documents and audition exactly what the bake renders.
//
// Deterministic: noise is an LFSR, presets/randomize/mutate take a seed. Never compiled into a
// game (STL allowed).
//
// .sfx (JSON; every key optional, defaults below):
//   { "sfx": 1, "wave": "square", "freq": 440, "freq_min": 0, "slide": 0, "dslide": 0,
//     "vib_depth": 0, "vib_speed": 0, "arp_mult": 1, "arp_time": 0, "duty": 0.5, "duty_sweep": 0,
//     "repeat": 0, "attack": 0, "sustain": 0.1, "punch": 0, "decay": 0.2, "lowpass": 0,
//     "highpass": 0, "volume": 0.5 }
//
// .song (JSON):
//   { "song": 1, "bpm": 120, "rows_per_beat": 4, "channels": 4,
//     "instruments": [ { "name": "lead", "wave": "square", "duty": 0.25, "attack": 0.01,
//                        "decay": 0.1, "sustain": 0.6, "release": 0.1, "volume": 0.7,
//                        "slide": 0, "vib_depth": 0, "vib_speed": 0 } ],
//     "patterns": [ { "name": "A", "rows": [ ["C-4 lead", "", "off", "E-2 bass"], ... ] } ],
//     "order": ["A", "A", "B"] }
//   A cell is "" (nothing new), "off" (release the channel's note) or "<note> [instrument]": a
//   note is C-4 / C#4 / ... B-7 (A-4 = 440 Hz); no instrument = the channel's last one. The song
//   plays the order list once, which the bake renders as one loop (play_music loops it).
#ifndef PHX_TOOLS_SYNTH_H
#define PHX_TOOLS_SYNTH_H

#include "json.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace phxtool {

constexpr uint32_t kSynthRate      = 22050;    // what .sfx/.song render at (tier 0 resamples it)
constexpr double   kSfxMaxSeconds  = 10.0;
constexpr double   kSongMaxSeconds = 300.0;
// A song's master gain: music leaves headroom for the sound effects mixed over it (the mixer
// saturates the sum), so four full channels peak around -6 dB.
constexpr double   kSongGain       = 0.2;

enum class Wave : uint8_t { Square, Saw, Triangle, Sine, Noise };

inline const std::vector<std::string>& wave_names() {
    static const std::vector<std::string> k = { "square", "saw", "triangle", "sine", "noise" };
    return k;
}
inline const char* wave_name(Wave w) { return wave_names()[size_t(w)].c_str(); }
inline Wave wave_from(const std::string& s, Wave d = Wave::Square) {
    for (size_t i = 0; i < wave_names().size(); ++i) if (wave_names()[i] == s) return Wave(i);
    return d;
}

// Deterministic 32-bit xorshift: presets, randomize and mutate are reproducible from a seed.
struct SynthRng {
    uint32_t s;
    explicit SynthRng(uint32_t seed) : s(seed ? seed : 0x9E3779B9u) {}
    uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    double unit() { return double(next() >> 8) / double(1u << 24); }          // [0, 1)
    double range(double a, double b) { return a + (b - a) * unit(); }
    bool chance(double p) { return unit() < p; }
};

// One oscillator + its noise source (shared by the SFX and song renderers).
struct Osc {
    double   phase = 0;          // cycles, fractional part used
    uint16_t lfsr = 0xACE1;
    double   held = 1.0;         // noise: the current random level
    long     noise_step = 0;
    double sample(Wave w, double duty) {
        const double p = phase - std::floor(phase);
        switch (w) {
        case Wave::Square:   return p < duty ? 1.0 : -1.0;
        case Wave::Saw:      return 2.0 * p - 1.0;
        case Wave::Triangle: return p < 0.5 ? 4.0 * p - 1.0 : 3.0 - 4.0 * p;
        case Wave::Sine:     return std::sin(p * 6.283185307179586);
        case Wave::Noise: {
            const long k = long(std::floor(phase * 2.0));        // a new level every half cycle
            while (noise_step < k) {
                const uint16_t bit = uint16_t(((lfsr >> 0) ^ (lfsr >> 2) ^ (lfsr >> 3) ^ (lfsr >> 5)) & 1u);
                lfsr = uint16_t((lfsr >> 1) | (bit << 15));
                held = double(int(lfsr & 0xFF) - 128) / 128.0;
                ++noise_step;
            }
            return held;
        }
        }
        return 0.0;
    }
    void advance(double freq, double rate) { phase += freq / rate; }
};

inline int16_t synth_sat16(double v) {
    const long x = std::lround(v * 32767.0);
    return int16_t(x > 32767 ? 32767 : (x < -32768 ? -32768 : x));
}

// ============================================================================================
// Sound effects
// ============================================================================================
struct SfxParams {
    Wave   wave       = Wave::Square;
    double freq       = 440;     // Hz at the start
    double freq_min   = 0;       // Hz: the sound ends when a falling slide passes it (0 = never)
    double slide      = 0;       // octaves per second (+ rises, - falls)
    double dslide     = 0;       // octaves per second, per second
    double vib_depth  = 0;       // semitones
    double vib_speed  = 0;       // Hz
    double arp_mult   = 1;       // pitch multiplier after arp_time (1 = off)
    double arp_time   = 0;       // seconds
    double duty       = 0.5;     // square: 0..1
    double duty_sweep = 0;       // per second
    double repeat     = 0;       // seconds: restart the pitch every `repeat` (0 = off)
    double attack     = 0;       // seconds
    double sustain    = 0.1;     // seconds
    double punch      = 0;       // 0..1: extra level at the start of the sustain
    double decay      = 0.2;     // seconds
    double lowpass    = 0;       // cutoff Hz (0 = off)
    double highpass   = 0;       // cutoff Hz (0 = off)
    double volume     = 0.5;     // 0..1

    double length() const { return std::min(kSfxMaxSeconds, std::max(0.0, attack) + std::max(0.0, sustain) + std::max(0.0, decay)); }
    bool operator==(const SfxParams& o) const {
        return wave == o.wave && freq == o.freq && freq_min == o.freq_min && slide == o.slide && dslide == o.dslide &&
               vib_depth == o.vib_depth && vib_speed == o.vib_speed && arp_mult == o.arp_mult && arp_time == o.arp_time &&
               duty == o.duty && duty_sweep == o.duty_sweep && repeat == o.repeat && attack == o.attack &&
               sustain == o.sustain && punch == o.punch && decay == o.decay && lowpass == o.lowpass &&
               highpass == o.highpass && volume == o.volume;
    }
    bool operator!=(const SfxParams& o) const { return !(*this == o); }
};

// The numeric parameters by name, in file/UI order (for load/save and the editor's sliders).
struct SfxField { const char* name; double SfxParams::*ptr; double lo, hi; const char* help; };
inline const std::vector<SfxField>& sfx_fields() {
    static const std::vector<SfxField> k = {
        { "freq",       &SfxParams::freq,       20, 4000, "Start pitch (Hz)" },
        { "freq_min",   &SfxParams::freq_min,   0, 2000,  "The sound ends when a falling pitch passes this (Hz; 0 = never)" },
        { "slide",      &SfxParams::slide,      -12, 12,  "Pitch slide (octaves per second; + rises)" },
        { "dslide",     &SfxParams::dslide,     -40, 40,  "Slide change (octaves per second, per second)" },
        { "vib_depth",  &SfxParams::vib_depth,  0, 12,    "Vibrato depth (semitones)" },
        { "vib_speed",  &SfxParams::vib_speed,  0, 40,    "Vibrato speed (Hz)" },
        { "arp_mult",   &SfxParams::arp_mult,   0.25, 4,  "Pitch jump after arp_time (x; 1 = off)" },
        { "arp_time",   &SfxParams::arp_time,   0, 1,     "When the pitch jumps (seconds)" },
        { "duty",       &SfxParams::duty,       0.05, 0.95, "Square wave duty (0.5 = square, 0.125 = thin)" },
        { "duty_sweep", &SfxParams::duty_sweep, -4, 4,    "Duty change per second" },
        { "repeat",     &SfxParams::repeat,     0, 1,     "Restart the pitch every N seconds (0 = off)" },
        { "attack",     &SfxParams::attack,     0, 2,     "Fade in (seconds)" },
        { "sustain",    &SfxParams::sustain,    0, 3,     "Hold (seconds)" },
        { "punch",      &SfxParams::punch,      0, 1,     "Extra loudness at the start of the hold" },
        { "decay",      &SfxParams::decay,      0, 4,     "Fade out (seconds)" },
        { "lowpass",    &SfxParams::lowpass,    0, 11000, "Low-pass cutoff (Hz; 0 = off)" },
        { "highpass",   &SfxParams::highpass,   0, 4000,  "High-pass cutoff (Hz; 0 = off)" },
        { "volume",     &SfxParams::volume,     0, 1,     "Loudness" },
    };
    return k;
}

inline std::vector<int16_t> render_sfx(const SfxParams& p, uint32_t rate = kSynthRate) {
    const double R = double(rate ? rate : kSynthRate);
    const double atk = std::max(0.0, p.attack), sus = std::max(0.0, p.sustain), dec = std::max(0.0, p.decay);
    const size_t n = size_t(p.length() * R);
    std::vector<int16_t> out;
    out.reserve(n);
    Osc osc;
    double lp = 0, hp_in = 0, hp = 0;
    const double lpa = p.lowpass > 0 ? 1.0 - std::exp(-6.283185307179586 * p.lowpass / R) : 1.0;
    const double hpa = p.highpass > 0 ? std::exp(-6.283185307179586 * p.highpass / R) : 0.0;
    const double nyq = R * 0.45;
    for (size_t i = 0; i < n; ++i) {
        const double t = double(i) / R;
        const double tr = p.repeat > 0 ? std::fmod(t, p.repeat) : t;
        double oct = p.slide * tr + 0.5 * p.dslide * tr * tr;
        double f = p.freq * std::pow(2.0, oct);
        if (p.freq_min > 0 && (p.slide < 0 || p.dslide < 0) && f < p.freq_min) break;
        if (p.arp_time > 0 && tr >= p.arp_time) f *= p.arp_mult;
        if (p.vib_depth > 0 && p.vib_speed > 0)
            f *= std::pow(2.0, p.vib_depth / 12.0 * std::sin(6.283185307179586 * p.vib_speed * t));
        f = std::min(nyq, std::max(1.0, f));
        double env;
        if (t < atk) env = t / atk;
        else if (t < atk + sus) env = 1.0 + p.punch * (1.0 - (t - atk) / std::max(1e-9, sus));
        else env = dec > 0 ? std::max(0.0, 1.0 - (t - atk - sus) / dec) : 0.0;
        const double duty = std::min(0.95, std::max(0.05, p.duty + p.duty_sweep * t));
        double x = osc.sample(p.wave, duty);
        osc.advance(f, R);
        if (p.lowpass > 0) { lp += lpa * (x - lp); x = lp; }
        if (p.highpass > 0) { hp = hpa * (hp + x - hp_in); hp_in = x; x = hp; }
        out.push_back(synth_sat16(x * env * p.volume * 0.8));
    }
    return out;
}

// The shortest decimal that reads back as exactly `v` (so a saved document renders the same PCM
// the editor played).
inline std::string synth_num(double v) {
    char b[40];
    for (int prec = 6; prec <= 17; ++prec) {
        std::snprintf(b, sizeof(b), "%.*g", prec, v);
        if (std::strtod(b, nullptr) == v) break;
    }
    return b;
}
// Round every parameter to 4 decimals (what presets / randomize / mutate / sliders produce), so
// saved files stay readable.
inline double synth_tidy(double v) { return std::round(v * 10000.0) / 10000.0; }

inline std::string sfx_to_json(const SfxParams& p) {
    std::string o = "{ \"sfx\": 1, \"wave\": \"" + std::string(wave_name(p.wave)) + "\"";
    for (const SfxField& f : sfx_fields()) o += ",\n  \"" + std::string(f.name) + "\": " + synth_num(p.*(f.ptr));
    return o + "\n}\n";
}

inline bool sfx_from_json(const std::string& text, SfxParams& out, std::string* err = nullptr) {
    JsonValue root;
    std::string jerr;
    if (!JsonParser::parse(text, root, &jerr)) { if (err) *err = "malformed JSON: " + jerr; return false; }
    if (!root.is_obj()) { if (err) *err = "top level is not a JSON object"; return false; }
    out = SfxParams{};
    const std::string w = root.str_at("wave");
    if (!w.empty()) {
        out.wave = wave_from(w, Wave(255));
        if (out.wave == Wave(255)) { if (err) *err = "unknown wave '" + w + "' (square, saw, triangle, sine, noise)"; return false; }
    }
    for (const SfxField& f : sfx_fields())
        if (const JsonValue* v = root.find(f.name)) out.*(f.ptr) = v->as_num(out.*(f.ptr));
    return true;
}

// Is this JSON text an .sfx / .song document? (For kind detection when the extension is .json.)
inline bool is_sfx_json(const std::string& head)  { return head.find("\"sfx\"") != std::string::npos; }
inline bool is_song_json(const std::string& head) { return head.find("\"song\"") != std::string::npos && head.find("\"patterns\"") != std::string::npos; }

inline SfxParams sfx_tidy(SfxParams p) {
    for (const SfxField& f : sfx_fields()) p.*(f.ptr) = synth_tidy(p.*(f.ptr));
    return p;
}

// Presets: a starting point per kind of sound, varied by `seed`.
inline const std::vector<std::string>& sfx_preset_names() {
    static const std::vector<std::string> k = { "pickup", "laser", "explosion", "powerup", "hurt", "jump", "blip" };
    return k;
}
inline SfxParams sfx_preset(const std::string& kind, uint32_t seed) {
    SynthRng r(seed * 2654435761u + uint32_t(kind.size()) * 97u + (kind.empty() ? 0u : uint32_t(uint8_t(kind[0]))));
    SfxParams p;
    p.attack = 0; p.punch = 0; p.volume = 0.5;
    if (kind == "pickup") {
        p.wave = Wave::Square; p.freq = r.range(800, 1400); p.duty = r.range(0.3, 0.5);
        p.arp_mult = r.range(1.3, 1.6); p.arp_time = r.range(0.04, 0.09);
        p.sustain = r.range(0.03, 0.08); p.punch = r.range(0.3, 0.6); p.decay = r.range(0.1, 0.25);
    } else if (kind == "laser") {
        p.wave = r.chance(0.5) ? Wave::Square : Wave::Saw; p.freq = r.range(800, 1800);
        p.slide = -r.range(3, 7); p.freq_min = r.range(80, 200); p.duty = r.range(0.2, 0.5);
        p.duty_sweep = r.range(-1, 1); p.sustain = r.range(0.05, 0.15); p.decay = r.range(0.05, 0.15);
    } else if (kind == "explosion") {
        p.wave = Wave::Noise; p.freq = r.range(300, 900); p.slide = -r.range(0.5, 2);
        p.sustain = r.range(0.1, 0.3); p.punch = r.range(0.3, 0.7); p.decay = r.range(0.3, 0.7);
        p.lowpass = r.chance(0.5) ? r.range(1500, 5000) : 0;
    } else if (kind == "powerup") {
        p.wave = r.chance(0.5) ? Wave::Square : Wave::Triangle; p.freq = r.range(250, 500);
        p.slide = r.range(1, 3); p.repeat = r.chance(0.5) ? r.range(0.08, 0.2) : 0;
        p.vib_depth = r.chance(0.5) ? r.range(0.2, 1) : 0; p.vib_speed = r.range(8, 20);
        p.sustain = r.range(0.2, 0.35); p.decay = r.range(0.1, 0.3);
    } else if (kind == "hurt") {
        p.wave = r.chance(0.5) ? Wave::Saw : Wave::Square; p.freq = r.range(200, 500);
        p.slide = -r.range(2, 5); p.sustain = r.range(0.02, 0.08); p.decay = r.range(0.1, 0.25);
        p.lowpass = r.range(2000, 6000);
    } else if (kind == "jump") {
        p.wave = Wave::Square; p.duty = r.range(0.2, 0.5); p.freq = r.range(250, 450);
        p.slide = r.range(1.5, 3.5); p.sustain = r.range(0.06, 0.14); p.decay = r.range(0.06, 0.15);
    } else {   // blip
        p.wave = r.chance(0.7) ? Wave::Square : Wave::Sine; p.duty = r.range(0.25, 0.5);
        p.freq = r.range(500, 1300); p.sustain = r.range(0.02, 0.06); p.decay = r.range(0.02, 0.06);
    }
    return sfx_tidy(p);
}
// Anything goes (still within sensible ranges).
inline SfxParams sfx_random(uint32_t seed) {
    SynthRng r(seed);
    SfxParams p;
    p.wave = Wave(r.next() % 5);
    p.freq = std::pow(2.0, r.range(6.5, 11.5));
    p.slide = r.chance(0.6) ? r.range(-6, 6) : 0;
    p.dslide = r.chance(0.2) ? r.range(-10, 10) : 0;
    p.vib_depth = r.chance(0.3) ? r.range(0, 2) : 0; p.vib_speed = r.range(2, 20);
    p.arp_mult = r.chance(0.3) ? r.range(0.5, 2) : 1; p.arp_time = r.range(0.02, 0.3);
    p.duty = r.range(0.1, 0.9); p.duty_sweep = r.chance(0.3) ? r.range(-1, 1) : 0;
    p.repeat = r.chance(0.2) ? r.range(0.05, 0.3) : 0;
    p.attack = r.chance(0.3) ? r.range(0, 0.2) : 0; p.sustain = r.range(0.02, 0.4);
    p.punch = r.chance(0.5) ? r.range(0, 0.8) : 0; p.decay = r.range(0.05, 0.6);
    p.lowpass = r.chance(0.3) ? r.range(800, 8000) : 0; p.highpass = r.chance(0.2) ? r.range(50, 800) : 0;
    p.volume = 0.5;
    return sfx_tidy(p);
}
// A small variation of `p` (the wave stays).
inline SfxParams sfx_mutate(const SfxParams& p, uint32_t seed) {
    SynthRng r(seed ^ 0x5bd1e995u);
    SfxParams q = p;
    for (const SfxField& f : sfx_fields()) {
        if (std::string(f.name) == "volume" || !r.chance(0.5)) continue;
        double& v = q.*(f.ptr);
        v += (f.hi - f.lo) * r.range(-0.05, 0.05);
        v = std::min(f.hi, std::max(f.lo, v));
    }
    return sfx_tidy(q);
}

// ============================================================================================
// Songs
// ============================================================================================
struct SongInstrument {
    std::string name = "lead";
    Wave   wave    = Wave::Square;
    double duty    = 0.5;
    double attack  = 0.005;   // seconds
    double decay   = 0.1;     // seconds, from 1 down to `sustain`
    double sustain = 0.6;     // level held while the note lasts
    double release = 0.08;    // seconds, after "off" or the next note
    double volume  = 0.6;
    double slide   = 0;       // octaves per second from the note's start (kicks: a fast fall)
    double vib_depth = 0;     // semitones
    double vib_speed = 0;     // Hz
    bool operator==(const SongInstrument& o) const {
        return name == o.name && wave == o.wave && duty == o.duty && attack == o.attack && decay == o.decay &&
               sustain == o.sustain && release == o.release && volume == o.volume && slide == o.slide &&
               vib_depth == o.vib_depth && vib_speed == o.vib_speed;
    }
};

struct InstField { const char* name; double SongInstrument::*ptr; double lo, hi; const char* help; };
inline const std::vector<InstField>& inst_fields() {
    static const std::vector<InstField> k = {
        { "duty",      &SongInstrument::duty,      0.05, 0.95, "Square wave duty (0.5 = square, 0.125 = thin)" },
        { "attack",    &SongInstrument::attack,    0, 1,      "Fade in (seconds)" },
        { "decay",     &SongInstrument::decay,     0, 2,      "Fall to the sustain level (seconds)" },
        { "sustain",   &SongInstrument::sustain,   0, 1,      "Level held while the note lasts" },
        { "release",   &SongInstrument::release,   0, 2,      "Fade out after 'off' or the next note (seconds)" },
        { "volume",    &SongInstrument::volume,    0, 1,      "Loudness" },
        { "slide",     &SongInstrument::slide,     -12, 12,   "Pitch slide from the note's start (octaves per second)" },
        { "vib_depth", &SongInstrument::vib_depth, 0, 2,      "Vibrato depth (semitones)" },
        { "vib_speed", &SongInstrument::vib_speed, 0, 20,     "Vibrato speed (Hz)" },
    };
    return k;
}

constexpr int kCellEmpty = -1;
constexpr int kCellOff   = -2;
constexpr int kMaxNote   = 95;     // B-7

struct SongCell {
    int note = kCellEmpty;          // kCellEmpty, kCellOff, or 0..95 (C-0 .. B-7; A-4 = 57)
    int inst = -1;                  // instrument index (-1 = the channel's last)
    bool operator==(const SongCell& o) const { return note == o.note && inst == o.inst; }
};

struct SongPattern {
    std::string name = "A";
    std::vector<std::vector<SongCell>> rows;      // rows[r][channel]
};

struct Song {
    int bpm = 120;
    int rows_per_beat = 4;
    int channels = 4;
    std::vector<SongInstrument> instruments;
    std::vector<SongPattern> patterns;
    std::vector<int> order;                        // pattern indices

    static constexpr int kMaxChannels = 8;
    static constexpr int kMaxRows = 128;

    double row_seconds() const { return 60.0 / (double(std::max(1, bpm)) * double(std::max(1, rows_per_beat))); }
    uint32_t row_frames(uint32_t rate = kSynthRate) const { return uint32_t(std::lround(row_seconds() * rate)); }
    int total_rows() const {
        int n = 0;
        for (int o : order) if (o >= 0 && o < int(patterns.size())) n += int(patterns[size_t(o)].rows.size());
        return n;
    }
    double seconds() const { return total_rows() * row_seconds(); }
    int find_instrument(const std::string& n) const {
        for (size_t i = 0; i < instruments.size(); ++i) if (instruments[i].name == n) return int(i);
        return -1;
    }
    int find_pattern(const std::string& n) const {
        for (size_t i = 0; i < patterns.size(); ++i) if (patterns[i].name == n) return int(i);
        return -1;
    }
    // Make every pattern row `channels` wide.
    void normalise() {
        channels = std::min(kMaxChannels, std::max(1, channels));
        for (SongPattern& p : patterns)
            for (auto& row : p.rows) row.resize(size_t(channels));
    }

    // ---- editing (Phosphorus Studio's tracker; every index stays consistent) ----
    std::string fresh_pattern_name() const {
        for (char c = 'A'; c <= 'Z'; ++c) if (find_pattern(std::string(1, c)) < 0) return std::string(1, c);
        for (int k = 1;; ++k) if (find_pattern("P" + std::to_string(k)) < 0) return "P" + std::to_string(k);
    }
    std::string fresh_instrument_name(const std::string& base) const {
        if (find_instrument(base) < 0) return base;
        for (int k = 2;; ++k) if (find_instrument(base + std::to_string(k)) < 0) return base + std::to_string(k);
    }
    int add_pattern(int rows = 16) {
        SongPattern p;
        p.name = fresh_pattern_name();
        p.rows.assign(size_t(std::min(kMaxRows, std::max(1, rows))), std::vector<SongCell>(size_t(channels)));
        patterns.push_back(p);
        return int(patterns.size()) - 1;
    }
    int duplicate_pattern(int i) {
        if (i < 0 || i >= int(patterns.size())) return -1;
        SongPattern p = patterns[size_t(i)];
        p.name = fresh_pattern_name();
        patterns.push_back(p);
        return int(patterns.size()) - 1;
    }
    // Removes the pattern and its places in the order list.
    void remove_pattern(int i) {
        if (i < 0 || i >= int(patterns.size())) return;
        patterns.erase(patterns.begin() + i);
        std::vector<int> o;
        for (int k : order) if (k != i) o.push_back(k > i ? k - 1 : k);
        order = o;
    }
    void resize_pattern(int i, int rows) {
        if (i < 0 || i >= int(patterns.size())) return;
        patterns[size_t(i)].rows.resize(size_t(std::min(kMaxRows, std::max(1, rows))), std::vector<SongCell>(size_t(channels)));
    }
    int add_instrument(SongInstrument in) {
        in.name = fresh_instrument_name(in.name.empty() ? "inst" : in.name);
        instruments.push_back(in);
        return int(instruments.size()) - 1;
    }
    // Removes the instrument; notes that named it fall back to the channel's previous instrument.
    void remove_instrument(int i) {
        if (i < 0 || i >= int(instruments.size())) return;
        instruments.erase(instruments.begin() + i);
        for (SongPattern& p : patterns)
            for (auto& row : p.rows)
                for (SongCell& c : row) { if (c.inst == i) c.inst = -1; else if (c.inst > i) --c.inst; }
    }
    // Renames instrument i when the name is free and valid (no spaces). False otherwise.
    bool rename_instrument(int i, const std::string& n) {
        if (i < 0 || i >= int(instruments.size()) || n.empty() || n.find_first_of(" \"") != std::string::npos) return false;
        if (instruments[size_t(i)].name != n && find_instrument(n) >= 0) return false;
        instruments[size_t(i)].name = n;
        return true;
    }
    bool rename_pattern(int i, const std::string& n) {
        if (i < 0 || i >= int(patterns.size()) || n.empty() || n.find('"') != std::string::npos) return false;
        if (patterns[size_t(i)].name != n && find_pattern(n) >= 0) return false;
        patterns[size_t(i)].name = n;
        return true;
    }
    void set_channels(int n) { channels = std::min(kMaxChannels, std::max(1, n)); normalise(); }
    // The pattern and row playing `seconds` into the order list (false past the end).
    bool position_at(double seconds, int& order_index, int& row) const {
        int r = int(seconds / row_seconds());
        for (size_t k = 0; k < order.size(); ++k) {
            const int o = order[k];
            if (o < 0 || o >= int(patterns.size())) continue;
            const int n = int(patterns[size_t(o)].rows.size());
            if (r < n) { order_index = int(k); row = r; return true; }
            r -= n;
        }
        return false;
    }
};

// "C-4" / "C#4" -> 48 / 49; -1 when it is not a note.
inline int note_from_text(const std::string& s) {
    static const char* kNames = "C-C#D-D#E-F-F#G-G#A-A#B-";
    if (s.size() != 3 || s[2] < '0' || s[2] > '7') return -1;
    for (int k = 0; k < 12; ++k)
        if (std::toupper(uint8_t(s[0])) == kNames[k * 2] && s[1] == kNames[k * 2 + 1]) return (s[2] - '0') * 12 + k;
    return -1;
}
inline std::string note_text(int n) {
    static const char* kNames = "C-C#D-D#E-F-F#G-G#A-A#B-";
    if (n < 0 || n > kMaxNote) return "---";
    std::string o;
    o += kNames[(n % 12) * 2];
    o += kNames[(n % 12) * 2 + 1];
    o += char('0' + n / 12);
    return o;
}
inline double note_freq(int n) { return 440.0 * std::pow(2.0, (n - 57) / 12.0); }

inline std::string cell_text(const Song& s, const SongCell& c) {
    if (c.note == kCellOff) return "off";
    if (c.note < 0) return "";
    std::string t = note_text(c.note);
    if (c.inst >= 0 && c.inst < int(s.instruments.size())) t += " " + s.instruments[size_t(c.inst)].name;
    return t;
}
inline bool cell_from_text(const Song& s, const std::string& text, SongCell& out, std::string* err = nullptr) {
    out = SongCell{};
    size_t a = text.find_first_not_of(' ');
    if (a == std::string::npos) return true;
    const size_t b = text.find(' ', a);
    const std::string head = text.substr(a, b == std::string::npos ? std::string::npos : b - a);
    if (head == "off") { out.note = kCellOff; return true; }
    out.note = note_from_text(head);
    if (out.note < 0) { if (err) *err = "'" + head + "' is not a note (C-4, C#4 ... B-7) or off"; return false; }
    if (b != std::string::npos) {
        const size_t c = text.find_first_not_of(' ', b);
        if (c != std::string::npos) {
            const std::string inst = text.substr(c, text.find(' ', c) == std::string::npos ? std::string::npos : text.find(' ', c) - c);
            out.inst = s.find_instrument(inst);
            if (out.inst < 0) { if (err) *err = "no instrument '" + inst + "'"; return false; }
        }
    }
    return true;
}

inline std::string json_str(const std::string& s) {
    std::string o = "\"";
    for (char c : s) { if (c == '"' || c == '\\') o += '\\'; o += c; }
    return o + "\"";
}

inline std::string song_to_json(const Song& s) {
    std::string o = "{ \"song\": 1, \"bpm\": " + std::to_string(s.bpm) + ", \"rows_per_beat\": " +
                    std::to_string(s.rows_per_beat) + ", \"channels\": " + std::to_string(s.channels) + ",\n";
    o += "  \"instruments\": [";
    for (size_t i = 0; i < s.instruments.size(); ++i) {
        const SongInstrument& in = s.instruments[i];
        o += i ? ",\n    " : "\n    ";
        o += "{ \"name\": " + json_str(in.name) + ", \"wave\": \"" + wave_name(in.wave) + "\"";
        for (const InstField& f : inst_fields()) o += ", \"" + std::string(f.name) + "\": " + synth_num(in.*(f.ptr));
        o += " }";
    }
    o += s.instruments.empty() ? "],\n" : "\n  ],\n";
    o += "  \"patterns\": [";
    for (size_t i = 0; i < s.patterns.size(); ++i) {
        const SongPattern& p = s.patterns[i];
        o += i ? ",\n    " : "\n    ";
        o += "{ \"name\": " + json_str(p.name) + ", \"rows\": [";
        for (size_t r = 0; r < p.rows.size(); ++r) {
            o += r ? ",\n        [" : "\n        [";
            for (size_t c = 0; c < p.rows[r].size(); ++c) { if (c) o += ", "; o += json_str(cell_text(s, p.rows[r][c])); }
            o += "]";
        }
        o += p.rows.empty() ? "] }" : "\n      ] }";
    }
    o += s.patterns.empty() ? "],\n" : "\n  ],\n";
    o += "  \"order\": [";
    for (size_t i = 0; i < s.order.size(); ++i) {
        if (i) o += ", ";
        const int k = s.order[i];
        o += json_str(k >= 0 && k < int(s.patterns.size()) ? s.patterns[size_t(k)].name : std::string());
    }
    return o + "]\n}\n";
}

inline bool song_from_json(const std::string& text, Song& out, std::string* err = nullptr) {
    auto fail = [&](const std::string& why) { if (err) *err = why; return false; };
    JsonValue root;
    std::string jerr;
    if (!JsonParser::parse(text, root, &jerr)) return fail("malformed JSON: " + jerr);
    if (!root.is_obj()) return fail("top level is not a JSON object");
    out = Song{};
    out.bpm = std::min(999, std::max(1, root.int_at("bpm", 120)));
    out.rows_per_beat = std::min(16, std::max(1, root.int_at("rows_per_beat", 4)));
    out.channels = std::min(Song::kMaxChannels, std::max(1, root.int_at("channels", 4)));
    if (const JsonValue* ins = root.find("instruments"); ins && ins->is_arr())
        for (const JsonValue& j : ins->arr) {
            SongInstrument in;
            in.name = j.str_at("name");
            if (in.name.empty() || in.name.find(' ') != std::string::npos)
                return fail("instrument names must be non-empty, without spaces ('" + in.name + "')");
            if (out.find_instrument(in.name) >= 0) return fail("two instruments are named '" + in.name + "'");
            const std::string w = j.str_at("wave");
            in.wave = wave_from(w.empty() ? "square" : w, Wave(255));
            if (in.wave == Wave(255)) return fail("instrument '" + in.name + "': unknown wave '" + w + "'");
            for (const InstField& f : inst_fields())
                if (const JsonValue* v = j.find(f.name)) in.*(f.ptr) = v->as_num(in.*(f.ptr));
            out.instruments.push_back(in);
        }
    if (const JsonValue* ps = root.find("patterns"); ps && ps->is_arr())
        for (const JsonValue& j : ps->arr) {
            SongPattern p;
            p.name = j.str_at("name");
            if (p.name.empty()) return fail("a pattern has no name");
            if (out.find_pattern(p.name) >= 0) return fail("two patterns are named '" + p.name + "'");
            if (const JsonValue* rows = j.find("rows"); rows && rows->is_arr()) {
                if (int(rows->arr.size()) > Song::kMaxRows) return fail("pattern '" + p.name + "' has more than 128 rows");
                for (const JsonValue& row : rows->arr) {
                    std::vector<SongCell> cells(size_t(out.channels));
                    if (row.is_arr())
                        for (size_t c = 0; c < row.arr.size() && c < cells.size(); ++c) {
                            std::string cerr;
                            if (!cell_from_text(out, row.arr[c].as_str(), cells[c], &cerr))
                                return fail("pattern '" + p.name + "' row " + std::to_string(p.rows.size()) + ": " + cerr);
                        }
                    p.rows.push_back(cells);
                }
            }
            out.patterns.push_back(p);
        }
    if (const JsonValue* od = root.find("order"); od && od->is_arr())
        for (const JsonValue& j : od->arr) {
            const int k = out.find_pattern(j.as_str());
            if (k < 0) return fail("the order names an unknown pattern '" + j.as_str() + "'");
            out.order.push_back(k);
        }
    return true;
}

// Renders the order list once. Rows are a whole number of frames, so a pattern's length in samples
// is exact and the loop point (the end) lands on a row boundary.
inline std::vector<int16_t> render_song(const Song& s, uint32_t rate = kSynthRate) {
    const double R = double(rate ? rate : kSynthRate);
    const uint32_t rf = std::max<uint32_t>(1, s.row_frames(rate ? rate : kSynthRate));
    const int ch = std::min(Song::kMaxChannels, std::max(1, s.channels));
    struct Voice {
        bool on = false, released = false;
        int inst = -1, last_inst = 0;
        double freq = 0, t = 0, rel_t = 0, rel_level = 0;
        Osc osc;
    };
    std::vector<Voice> v(static_cast<size_t>(ch));
    for (int c = 0; c < ch; ++c) { v[size_t(c)].osc.lfsr = uint16_t(0xACE1 + c * 0x1357); v[size_t(c)].last_inst = 0; }
    auto level = [](const SongInstrument& in, double t) {
        if (t < in.attack) return in.attack > 0 ? t / in.attack : 1.0;
        const double d = t - in.attack;
        if (in.decay > 0 && d < in.decay) return 1.0 - (1.0 - in.sustain) * (d / in.decay);
        return in.sustain;
    };
    std::vector<int16_t> out;
    const size_t max_frames = size_t(kSongMaxSeconds * R);
    out.reserve(std::min(max_frames, size_t(s.total_rows()) * rf));
    for (int o : s.order) {
        if (o < 0 || o >= int(s.patterns.size())) continue;
        for (const auto& row : s.patterns[size_t(o)].rows) {
            for (int c = 0; c < ch && c < int(row.size()); ++c) {
                const SongCell& cell = row[size_t(c)];
                Voice& vo = v[size_t(c)];
                if (cell.note == kCellOff) {
                    if (vo.on && !vo.released && vo.inst >= 0 && vo.inst < int(s.instruments.size())) {
                        vo.released = true; vo.rel_t = 0; vo.rel_level = level(s.instruments[size_t(vo.inst)], vo.t);
                    }
                } else if (cell.note >= 0) {
                    const int inst = cell.inst >= 0 ? cell.inst : vo.last_inst;
                    if (inst >= 0 && inst < int(s.instruments.size())) {
                        vo.on = true; vo.released = false; vo.inst = inst; vo.last_inst = inst;
                        vo.freq = note_freq(cell.note); vo.t = 0;
                    }
                }
            }
            for (uint32_t f = 0; f < rf; ++f) {
                if (out.size() >= max_frames) return out;
                double mix = 0;
                for (int c = 0; c < ch; ++c) {
                    Voice& vo = v[size_t(c)];
                    if (!vo.on) continue;
                    const SongInstrument& in = s.instruments[size_t(vo.inst)];
                    double env;
                    if (vo.released) {
                        env = in.release > 0 ? vo.rel_level * (1.0 - vo.rel_t / in.release) : 0.0;
                        if (env <= 0) { vo.on = false; continue; }
                        vo.rel_t += 1.0 / R;
                    } else {
                        env = level(in, vo.t);
                    }
                    double fr = in.slide != 0 ? vo.freq * std::pow(2.0, in.slide * vo.t) : vo.freq;
                    if (in.vib_depth > 0 && in.vib_speed > 0)
                        fr *= std::pow(2.0, in.vib_depth / 12.0 * std::sin(6.283185307179586 * in.vib_speed * vo.t));
                    fr = std::min(R * 0.45, std::max(1.0, fr));
                    mix += vo.osc.sample(in.wave, in.duty) * env * in.volume;
                    vo.osc.advance(fr, R);
                    vo.t += 1.0 / R;
                }
                out.push_back(synth_sat16(mix * kSongGain));
            }
        }
    }
    return out;
}

// One note of an instrument, for auditioning it (held `hold` seconds, then released).
inline std::vector<int16_t> render_note(const SongInstrument& in, int note, double hold = 0.25, uint32_t rate = kSynthRate) {
    Song s;
    s.bpm = 60; s.rows_per_beat = 1; s.channels = 1;
    s.instruments = { in };
    // one row = one second at 60 bpm; scale by picking rows_per_beat so a row is ~hold seconds
    s.rows_per_beat = std::max(1, int(std::lround(1.0 / std::max(0.05, hold))));
    const int tail = std::max(1, int(std::ceil(in.release / s.row_seconds())) + 1);
    SongPattern p;
    p.name = "n";
    p.rows.assign(size_t(1 + tail), std::vector<SongCell>(1));
    p.rows[0][0] = SongCell{ std::min(kMaxNote, std::max(0, note)), 0 };
    p.rows[1][0] = SongCell{ kCellOff, -1 };
    s.patterns = { p };
    s.order = { 0 };
    return render_song(s, rate);
}

// Problems a song has that the bake would reject or that play nothing, as human-readable lines.
inline std::vector<std::string> song_problems(const Song& s) {
    std::vector<std::string> out;
    if (s.instruments.empty()) out.push_back("no instruments");
    if (s.order.empty()) out.push_back("the order list is empty (the song plays nothing)");
    if (s.seconds() > kSongMaxSeconds) out.push_back("longer than 300 s (the bake stops there)");
    for (const SongPattern& p : s.patterns) if (p.rows.empty()) out.push_back("pattern '" + p.name + "' has no rows");
    return out;
}

// A small song to start from: 4 channels (lead, bass, drums), two patterns.
inline Song song_starter() {
    Song s;
    s.bpm = 120; s.rows_per_beat = 4; s.channels = 4;
    SongInstrument lead;  lead.name = "lead";  lead.wave = Wave::Square;   lead.duty = 0.25; lead.decay = 0.15; lead.sustain = 0.5; lead.volume = 0.5;
    SongInstrument bass;  bass.name = "bass";  bass.wave = Wave::Triangle; bass.decay = 0.2; bass.sustain = 0.7; bass.volume = 0.8;
    SongInstrument kick;  kick.name = "kick";  kick.wave = Wave::Sine; kick.attack = 0; kick.decay = 0.12; kick.sustain = 0; kick.release = 0.02; kick.volume = 0.9; kick.slide = -6;
    SongInstrument hat;   hat.name = "hat";    hat.wave = Wave::Noise; hat.attack = 0; hat.decay = 0.04; hat.sustain = 0; hat.release = 0.01; hat.volume = 0.35;
    s.instruments = { lead, bass, kick, hat };
    auto pat = [&](const std::string& name, const std::vector<int>& melody, int root) {
        SongPattern p; p.name = name;
        p.rows.assign(16, std::vector<SongCell>(4));
        for (int r = 0; r < 16; ++r) {
            if (melody[size_t(r)] >= 0) p.rows[size_t(r)][0] = SongCell{ melody[size_t(r)], r == 0 ? 0 : -1 };
            if (r % 4 == 0) p.rows[size_t(r)][1] = SongCell{ root + (r == 8 ? 7 : 0), r == 0 ? 1 : -1 };
            if (r % 8 == 0) p.rows[size_t(r)][2] = SongCell{ 36, 2 };
            if (r % 4 == 2) p.rows[size_t(r)][3] = SongCell{ 84, 3 };
        }
        return p;
    };
    const int C4 = 48, E4 = 52, G4 = 55, A4 = 57, F4 = 53, D4 = 50;
    s.patterns = { pat("A", { C4, -1, E4, -1, G4, -1, E4, -1, A4, -1, G4, -1, E4, -1, D4, -1 }, 24),
                   pat("B", { F4, -1, A4, -1, 60, -1, A4, -1, G4, -1, E4, -1, D4, -1, C4, -1 }, 29) };
    s.order = { 0, 1 };
    return s;
}

} // namespace phxtool
#endif // PHX_TOOLS_SYNTH_H
