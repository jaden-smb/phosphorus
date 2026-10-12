// tools/phxpack/bundle_writer.h — host-side `.phxp` assembler. STL is fine here (tools
// never ship to console). Accumulates assets, then writes header + sorted TOC + blobs.
// Shares the format definition (phx/resource/bundle.h) with the runtime reader.
#ifndef PHX_TOOLS_BUNDLE_WRITER_H
#define PHX_TOOLS_BUNDLE_WRITER_H

#include "phx/resource/bundle.h"
#include "phx/core/types.h"
#include "phx/core/pixel.h"
#include "phx/core/crc32.h"
#include "lz_encode.h"
#include "tex_encode.h"

#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace phxtool {

class BundleWriter {
public:
    // target_tier: 0=GBA PPU, 1=PSP GU, 2=PC GL/soft (phx/resource/bundle.h). Anything else is
    // an authoring mistake (a made-up tier the runtime's target guard could never match) —
    // fail loudly at construction rather than silently baking an unloadable bundle.
    explicit BundleWriter(uint8_t target_tier = 2) : target_(target_tier) {
        if (target_tier > 2) {
            std::fprintf(stderr, "BundleWriter: invalid target tier %u (want 0=GBA/1=PSP/2=PC)\n",
                         unsigned(target_tier));
            std::abort();
        }
    }

    // Enable LZSS compression of blobs. Each asset is compressed independently at write()
    // time and stored compressed ONLY if that actually shrinks it (else stored verbatim), so
    // turning this on never makes a bundle bigger and the reader handles a mix transparently.
    void set_compression(bool on) { compress_ = on; }

    // PER-TARGET ENCODE (docs/06 §4, docs/08 §2), the texture counterpart of add_sound's
    // tier-0 resample: tier 0 bakes the PPU's native PAL4_TILES (4bpp tiles + palettes,
    // ~8× less texel data in the ROM, upload = claim banks + remap nibbles instead of a
    // full quantize), tier 1 bakes the GU's swizzled RGBA8 (pure reorder, sampled zero-copy
    // with the swizzle bit). Sources a tier can't express stay RGBA8 — the runtime then
    // behaves exactly as it did for v1 bundles (upload-time quantize on GBA, linear on PSP).
    void add_texture(const std::string& name, const uint32_t* rgba,
                     uint16_t w, uint16_t h) {
        phx::TextureBlobHeader th{};
        th.width = w; th.height = h; th.format = uint8_t(phx::PixelFormat::RGBA8);

        std::vector<uint8_t> payload;
        if (target_ == 0 && pal4_encode(rgba, w, h, payload)) {
            th.format = uint8_t(phx::PixelFormat::PAL4_TILES);
            std::printf("  ~ texture %-12s tier-0 encode: RGBA8 -> 4bpp tiles (%u -> %u bytes)\n",
                        name.c_str(), unsigned(uint32_t(w) * h * 4), unsigned(payload.size()));
        } else if (target_ == 1 && swz_encode(rgba, w, h, payload)) {
            th.format = uint8_t(phx::PixelFormat::RGBA8_SWZ);
            std::printf("  ~ texture %-12s tier-1 encode: RGBA8 swizzled (GU block order)\n",
                        name.c_str());
        } else {
            if (target_ != 2)
                std::printf("  ~ texture %-12s kept RGBA8 (source not tier-%u encodable)\n",
                            name.c_str(), unsigned(target_));
            payload.resize(size_t(w) * h * 4);
            std::memcpy(payload.data(), rgba, payload.size());
        }

        std::vector<uint8_t> blob(sizeof(th) + payload.size());
        std::memcpy(blob.data(), &th, sizeof(th));
        std::memcpy(blob.data() + sizeof(th), payload.data(), payload.size());
        push(name, phx::AssetType::Texture, std::move(blob));
    }

    // `parallax`: optional per-layer camera factors (one {fx,fy} pair per layer, 1.0 = moves
    // with the world), e.g. from Tiled's parallaxx/parallaxy. The Q16 table is appended — and
    // the header flag set — only when some factor differs from 1, so bundles without parallax
    // stay byte-identical to the old format.
    // `tile_flags`: optional per-tile collision flags (kTileFlag*, indexed by tile index),
    // e.g. from Tiled tileset per-tile properties. Same rule: appended — 4-aligned, a uint32
    // count then the bytes — and flagged only when some flag is non-zero.
    void add_tilemap(const std::string& name, const uint16_t* indices,
                     uint16_t w, uint16_t h, uint8_t layers,
                     uint8_t tile_w, uint8_t tile_h, const std::string& tileset,
                     const std::vector<std::pair<double,double>>* parallax = nullptr,
                     const std::vector<uint8_t>* tile_flags = nullptr) {
        phx::TilemapBlobHeader mh{};
        mh.width = w; mh.height = h; mh.layers = layers;
        mh.tile_w = tile_w; mh.tile_h = tile_h;
        mh.tileset = phx::fnv1a(tileset.c_str());
        bool want_par = false;
        if (parallax)
            for (auto& p : *parallax) if (p.first != 1.0 || p.second != 1.0) want_par = true;
        bool want_flags = false;
        if (tile_flags)
            for (uint8_t f : *tile_flags) if (f) want_flags = true;
        if (want_par)   mh.flags |= phx::kTilemapHasParallax;
        if (want_flags) mh.flags |= phx::kTilemapHasTileFlags;

        const size_t n = size_t(w) * h * layers;
        size_t end = sizeof(mh) + n * sizeof(uint16_t);
        const size_t par_off = (end + 3) & ~size_t(3);                        // 4-align
        if (want_par) end = par_off + size_t(layers) * 2 * sizeof(int32_t);
        const size_t flg_off = (end + 3) & ~size_t(3);                        // 4-align
        if (want_flags) end = flg_off + sizeof(uint32_t) + tile_flags->size();

        std::vector<uint8_t> blob(end, 0);
        std::memcpy(blob.data(), &mh, sizeof(mh));
        std::memcpy(blob.data() + sizeof(mh), indices, n * sizeof(uint16_t));
        if (want_par) {
            int32_t* pq = reinterpret_cast<int32_t*>(blob.data() + par_off);
            for (uint8_t l = 0; l < layers; ++l) {
                const auto pr = l < parallax->size() ? (*parallax)[l] : std::make_pair(1.0, 1.0);
                pq[l * 2 + 0] = int32_t(pr.first  * 65536.0 + (pr.first  < 0 ? -0.5 : 0.5));
                pq[l * 2 + 1] = int32_t(pr.second * 65536.0 + (pr.second < 0 ? -0.5 : 0.5));
            }
        }
        if (want_flags) {
            const uint32_t count = uint32_t(tile_flags->size());
            std::memcpy(blob.data() + flg_off, &count, sizeof(count));
            std::memcpy(blob.data() + flg_off + sizeof(count), tile_flags->data(), count);
        }
        push(name, phx::AssetType::Tilemap, std::move(blob));
    }

    // Sprite-sheet metadata: a frame grid over a (separately added) texture + named clips, and
    // (optional) the clips' transitions.
    void add_sprite(const std::string& name, const std::string& texture,
                    uint16_t frame_w, uint16_t frame_h, uint16_t cols,
                    const std::vector<phx::SpriteClipDef>& clips,
                    const std::vector<phx::SpriteTransDef>& trans = {}) {
        phx::SpriteBlobHeader sh{};
        sh.texture = phx::fnv1a(texture.c_str());
        sh.frame_w = frame_w; sh.frame_h = frame_h; sh.cols = cols;
        sh.clip_count = uint16_t(clips.size());
        const size_t clips_end = sizeof(sh) + clips.size() * sizeof(phx::SpriteClipDef);
        const size_t trans_bytes = trans.empty() ? 0 : 8 + trans.size() * sizeof(phx::SpriteTransDef);
        std::vector<uint8_t> blob(clips_end + trans_bytes);
        std::memcpy(blob.data(), &sh, sizeof(sh));
        if (!clips.empty())
            std::memcpy(blob.data() + sizeof(sh), clips.data(), clips.size() * sizeof(phx::SpriteClipDef));
        if (!trans.empty()) {        // the transitions trailer (bundle.h); clips_end is 4-aligned
            const uint32_t hdr[2] = { phx::kSpriteTransMagic, uint32_t(trans.size()) };
            std::memcpy(blob.data() + clips_end, hdr, sizeof(hdr));
            std::memcpy(blob.data() + clips_end + 8, trans.data(), trans.size() * sizeof(phx::SpriteTransDef));
        }
        push(name, phx::AssetType::Sprite, std::move(blob));
    }

    // A compiled dialogue (tools/phxpack/dialogue.h DlgCompiled::blob()).
    void add_dialogue(const std::string& name, std::vector<uint8_t> blob) {
        push(name, phx::AssetType::Dialogue, std::move(blob));
    }

    // A font's glyph table over a (separately added) atlas texture (bundle.h FontBlobHeader).
    void add_font(const std::string& name, const phx::FontBlobHeader& hdr, const std::vector<phx::FontGlyphDef>& glyphs) {
        phx::FontBlobHeader h = hdr;
        h.glyph_count = uint16_t(glyphs.size());
        std::vector<uint8_t> blob(sizeof(h) + glyphs.size() * sizeof(phx::FontGlyphDef));
        std::memcpy(blob.data(), &h, sizeof(h));
        if (!glyphs.empty()) std::memcpy(blob.data() + sizeof(h), glyphs.data(), glyphs.size() * sizeof(phx::FontGlyphDef));
        push(name, phx::AssetType::Font, std::move(blob));
    }

    // Mono 16-bit PCM sound at `rate` Hz (e.g. decoded from a WAV). PER-TARGET ENCODE
    // (docs/06): on tier 0 (GBA) the PCM is resampled down to the GBA device rate at BAKE
    // time — the ROM carries ~2.4× less sample data (for 44.1 kHz sources) and the 16 MHz
    // CPU mixes 1:1 instead of resampling every voice. Q16 linear interpolation, all-integer
    // (deterministic).
    // Tiers 1/2 keep the source rate: their mixers run at 44.1 kHz and resample cheaply.
    // == the GBA Direct Sound device rate. 18157 Hz is the vblank-locked rate (924 CPU
    // cycles/sample; exactly 304 samples per 280896-cycle video frame) — the GBA backend's
    // per-frame double-buffer pump needs integer samples/frame (16384 Hz gave 274.3125,
    // i.e. a discontinuity every frame). Must match kGbaAudioRate in the GBA game entries.
    static constexpr uint32_t kTier0Rate = 18157;
    void add_sound(const std::string& name, const int16_t* samples, uint32_t frames, uint32_t rate) {
        std::vector<int16_t> enc;
        if (target_ == 0 && rate > kTier0Rate && frames) {
            const uint64_t step = (uint64_t(rate) << 16) / kTier0Rate;   // src frames per out, Q16
            const uint32_t out_n = uint32_t((uint64_t(frames) << 16) / step);
            enc.resize(out_n);
            uint64_t pos = 0;
            for (uint32_t i = 0; i < out_n; ++i, pos += step) {
                const uint32_t i0 = uint32_t(pos >> 16);
                const uint32_t i1 = i0 + 1 < frames ? i0 + 1 : i0;
                const int32_t  f  = int32_t(pos & 0xFFFF);
                enc[i] = int16_t((int32_t(samples[i0]) * (65536 - f) + int32_t(samples[i1]) * f) >> 16);
            }
            std::printf("  ~ sound   %-12s tier-0 encode: %u Hz -> %u Hz (%u -> %u frames)\n",
                        name.c_str(), rate, kTier0Rate, frames, out_n);
            samples = enc.data(); frames = out_n; rate = kTier0Rate;
        }
        phx::SoundBlobHeader sh{}; sh.frames = frames; sh.rate = rate;
        std::vector<uint8_t> blob(sizeof(sh) + size_t(frames) * sizeof(int16_t));
        std::memcpy(blob.data(), &sh, sizeof(sh));
        if (frames) std::memcpy(blob.data() + sizeof(sh), samples, size_t(frames) * sizeof(int16_t));
        push(name, phx::AssetType::Sound, std::move(blob));
    }

    // Object-layer spawn points (e.g. from a Tiled object group), plus the optional extension
    // (names + per-instance properties, already laid out; see phx/resource/bundle.h).
    void add_spawns(const std::string& name, const std::vector<phx::SpawnDef>& spawns,
                    const std::vector<uint8_t>& ext = {}) {
        phx::SpawnBlobHeader sh{}; sh.count = uint32_t(spawns.size());
        std::vector<uint8_t> blob(sizeof(sh) + spawns.size() * sizeof(phx::SpawnDef));
        std::memcpy(blob.data(), &sh, sizeof(sh));
        if (!spawns.empty())
            std::memcpy(blob.data() + sizeof(sh), spawns.data(), spawns.size() * sizeof(phx::SpawnDef));
        if (!ext.empty()) {
            blob.resize((blob.size() + 3) & ~size_t(3), 0);
            blob.insert(blob.end(), ext.begin(), ext.end());
        }
        push(name, phx::AssetType::Spawns, std::move(blob));
    }

    void add_blob(const std::string& name, const void* data, uint32_t size) {
        std::vector<uint8_t> blob(size);
        std::memcpy(blob.data(), data, size);
        push(name, phx::AssetType::Blob, std::move(blob));
    }

    // Add an already-baked asset with its name hash intact (used to MERGE a pre-baked bundle —
    // e.g. converter output — whose TOC stores hashes, not the original asset names).
    void add_raw(phx::NameHash hash, phx::AssetType type, std::vector<uint8_t> blob) {
        entries_.push_back(Entry{ hash, type, std::move(blob), std::string(), source_ });
    }

    // Author-source attribution for the assets added AFTER this call (the phxpack assembler
    // sets it per input). Feeds the manifest sidecar and the lock file's per-input asset map.
    void set_source(const std::string& src) { source_ = src; }

    // Introspection for the manifest/lock writers (tools/phxpack/lock.h). Note: write()
    // sorts entries by name hash, so index/order differs before and after write().
    uint32_t         entry_count() const        { return uint32_t(entries_.size()); }
    phx::NameHash    entry_hash(uint32_t i) const   { return entries_[i].hash; }
    phx::AssetType   entry_type(uint32_t i) const   { return entries_[i].type; }
    const std::string& entry_name(uint32_t i) const { return entries_[i].name; }    // "" if merged raw
    const std::string& entry_source(uint32_t i) const { return entries_[i].source; }

    static const char* type_name(phx::AssetType type) {
        switch (type) {
        case phx::AssetType::Texture: return "Texture";
        case phx::AssetType::Tilemap: return "Tilemap";
        case phx::AssetType::Sound: return "Sound";
        case phx::AssetType::Font: return "Font";
        case phx::AssetType::Blob: return "Blob";
        case phx::AssetType::Sprite: return "Sprite";
        case phx::AssetType::Spawns: return "Spawns";
        case phx::AssetType::Dialogue: return "Dialogue";
        }
        return "Unknown";
    }

    // A logical identifier is (type, hash), never just a stem. Sprites and fonts
    // intentionally carry their sheets; only byte-identical Texture entries may
    // be coalesced. Every other duplicate is ambiguous even if its bytes match.
    bool validate_inputs(bool references = false) {
        for (size_t i = 0; i < entries_.size(); ++i) {
            const Entry& a = entries_[i];
            for (size_t j = 0; j < i; ++j) {
                const Entry& b = entries_[j];
                if (a.hash != b.hash || a.type != b.type) continue;
                if (a.type == phx::AssetType::Texture && a.blob == b.blob) continue;
                std::fprintf(stderr, "phxpack: '%s' [%s]: duplicate asset identifier 0x%08x%s%s; also defined by '%s'\n",
                             a.source.c_str(), type_name(a.type), unsigned(a.hash),
                             a.name.empty() ? "" : " name=", a.name.c_str(), b.source.c_str());
                return false;
            }
            if (!references) continue;
            phx::NameHash ref = 0;
            size_t offset = 0, required = 0;
            if (a.type == phx::AssetType::Sprite) {
                offset = offsetof(phx::SpriteBlobHeader, texture); required = sizeof(phx::SpriteBlobHeader);
            } else if (a.type == phx::AssetType::Font) {
                offset = offsetof(phx::FontBlobHeader, texture); required = sizeof(phx::FontBlobHeader);
            } else if (a.type == phx::AssetType::Tilemap) {
                offset = offsetof(phx::TilemapBlobHeader, tileset); required = sizeof(phx::TilemapBlobHeader);
            }
            if (!required) continue;
            if (a.blob.size() < required) {
                std::fprintf(stderr, "phxpack: '%s' [%s]: truncated asset header\n", a.source.c_str(), type_name(a.type));
                return false;
            }
            std::memcpy(&ref, a.blob.data() + offset, sizeof(ref));
            const bool found = std::any_of(entries_.begin(), entries_.end(), [&](const Entry& e) {
                return e.type == phx::AssetType::Texture && e.hash == ref;
            });
            if (!found) {
                std::fprintf(stderr, "phxpack: '%s' [%s]: missing Texture reference 0x%08x\n",
                             a.source.c_str(), type_name(a.type), unsigned(ref));
                return false;
            }
        }
        return true;
    }

    // CRC32 of the COMPLETE file written by the last successful write() (header included).
    // The lock file records it so a later invocation can prove the output on disk is exactly
    // the bundle this bake produced (not stale, not hand-edited).
    uint32_t file_crc32() const { return file_crc32_; }

    // Assemble and write. Returns false on I/O error.
    bool write(const std::string& path) {
        // sort by name hash so the runtime can binary-search the TOC
        std::sort(entries_.begin(), entries_.end(),
                  [](const Entry& a, const Entry& b) { return a.hash != b.hash ? a.hash < b.hash : a.type < b.type; });

        entries_.erase(std::unique(entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b) {
            return a.hash == b.hash && a.type == phx::AssetType::Texture && b.type == a.type && a.blob == b.blob;
        }), entries_.end());

        const uint32_t count    = uint32_t(entries_.size());
        const uint32_t toc_off  = uint32_t(sizeof(phx::BundleHeader));
        const uint32_t blob_off = toc_off + count * uint32_t(sizeof(phx::TocEntry));

        // Pick each blob's STORED bytes (compressed if smaller, else the raw blob). The TOC
        // records `usize` (decompressed) and `size` (stored) so the reader knows both.
        std::vector<const std::vector<uint8_t>*> stored(count);
        std::vector<std::vector<uint8_t>> packed(count);
        std::vector<uint16_t> flags(count, phx::kTocNone);
        for (uint32_t i = 0; i < count; ++i) {
            const std::vector<uint8_t>& raw = entries_[i].blob;
            stored[i] = &raw;
            if (compress_ && !raw.empty()) {
                lz_encode(raw.data(), uint32_t(raw.size()), packed[i]);
                if (packed[i].size() < raw.size()) { stored[i] = &packed[i]; flags[i] = phx::kTocLZ; }
            }
        }

        std::vector<phx::TocEntry> toc(count);
        uint32_t cursor = blob_off;
        for (uint32_t i = 0; i < count; ++i) {
            cursor = align16(cursor);
            toc[i].name_hash = entries_[i].hash;
            toc[i].type      = uint16_t(entries_[i].type);
            toc[i].flags     = flags[i];
            toc[i].offset    = cursor;
            toc[i].size      = uint32_t(stored[i]->size());        // stored (maybe compressed)
            toc[i].usize     = uint32_t(entries_[i].blob.size());  // decompressed
            cursor += uint32_t(stored[i]->size());
        }
        const uint32_t total = cursor;

        // Assemble the whole file in memory first (TOC + blobs; zero-filled alignment padding
        // comes for free from vector's default-init) so the blob-region CRC32 can be computed
        // before the header is written — a bundle is small enough on any target's dev machine
        // that this costs nothing, and it means the header is ALWAYS written with the real
        // checksum of the bytes that follow it, never patched after the fact on disk.
        std::vector<uint8_t> buf(total - toc_off, 0);
        std::memcpy(buf.data(), toc.data(), count * sizeof(phx::TocEntry));
        for (uint32_t i = 0; i < count; ++i)
            std::memcpy(buf.data() + (toc[i].offset - toc_off), stored[i]->data(), stored[i]->size());

        phx::BundleHeader h{};
        h.magic = phx::kBundleMagic; h.version = phx::kBundleVersion;
        h.target = target_; h.flags = 0;
        h.asset_count = count; h.toc_offset = toc_off; h.total_size = total;
        h.blob_crc32 = phx::crc32_of(buf.data(), buf.size());   // covers TOC + every blob

        FILE* fp = std::fopen(path.c_str(), "wb");
        if (!fp) return false;
        const bool wrote_header = std::fwrite(&h, sizeof(h), 1, fp) == 1;
        const bool wrote_body = std::fwrite(buf.data(), 1, buf.size(), fp) == buf.size();
        const bool closed = std::fclose(fp) == 0;
        if (!wrote_header || !wrote_body || !closed) return false;
        file_crc32_ = phx::crc32_finish(phx::crc32(buf.data(), buf.size(),
                                                   phx::crc32(&h, sizeof(h))));
        return true;
    }

    uint32_t count() const { return uint32_t(entries_.size()); }

private:
    // name/source are host-side metadata only (manifest + lock); they never reach the bundle.
    struct Entry {
        phx::NameHash hash; phx::AssetType type; std::vector<uint8_t> blob;
        std::string name, source;
    };

    static uint32_t align16(uint32_t v) { return (v + 15u) & ~15u; }

    void push(const std::string& name, phx::AssetType type, std::vector<uint8_t>&& blob) {
        entries_.push_back(Entry{ phx::fnv1a(name.c_str()), type, std::move(blob), name, source_ });
    }

    uint8_t target_;
    bool    compress_ = false;
    std::string source_;
    uint32_t file_crc32_ = 0;
    std::vector<Entry> entries_;
};

} // namespace phxtool
#endif // PHX_TOOLS_BUNDLE_WRITER_H
