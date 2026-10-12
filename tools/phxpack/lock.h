// tools/phxpack/lock.h — the assembler's reproducibility/incrementality sidecars
// (host-only, STL allowed). Implements the pipeline guarantees of docs/08 §2/§9:
//
//   <out>.lock          — records the tool version, bundle-format version, target tier,
//     compression flag, every source input's content hash (CRC32) + size, WHICH assets each
//     input produced, and the CRC32 of the written bundle. A later invocation uses it to
//     (a) prove the bundle on disk is exactly the recorded bake (stale-bundle detection —
//     CI can diff `output crc32` against the file), (b) skip the bake entirely when nothing
//     changed, and (c) re-bake only the changed inputs, reusing the other assets straight
//     out of the previous bundle (per-asset incremental).
//
//   <out>.manifest.txt  — human-readable hash <-> name <-> source-path table for dev-build
//     debugging (name hashes are one-way; this is how you answer "what IS 0x9C3A...?").
//
//   --upgrade           — re-bake a bundle from its OWN lock's recorded source list (after
//     a tool/format upgrade), without repeating the input list on the command line.
//
// The lock format is deliberately plain text (one fact per line, path last so paths may
// contain spaces) — diffable, greppable, and versioned by its first line.
#ifndef PHX_TOOLS_LOCK_H
#define PHX_TOOLS_LOCK_H

#include "phx/core/crc32.h"
#include "phx/resource/bundle.h"
#include "bundle_writer.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace phxtool {

// Bump when the BAKE LOGIC changes in a way that alters output for identical inputs (new
// encoders, resample changes, ...). Distinct from kBundleVersion (the on-disk format): either
// changing invalidates every lock, so stale bundles are re-baked instead of trusted.
constexpr uint32_t kPhxpackToolVersion = 3;

struct LockAsset { uint32_t hash = 0; uint16_t type = 0; };
struct LockInput {
    std::string path;
    uint32_t crc = 0, size = 0;
    std::vector<LockAsset> assets;   // what this input contributed to the bundle
};
struct LockFile {
    uint32_t tool = 0;               // kPhxpackToolVersion at bake time
    uint16_t bundle_version = 0;     // kBundleVersion at bake time
    int      target = 2;
    bool     compress = false;
    uint32_t out_crc = 0;            // CRC32 of the complete written bundle file
    std::vector<LockInput> inputs;
};

// CRC32 + size of a file's bytes. False when the file can't be read.
inline bool file_crc32(const std::string& path, uint32_t& crc, uint32_t& size) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    uint32_t c = 0xFFFFFFFFu, n = 0;
    uint8_t buf[16384];
    size_t got;
    while ((got = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        c = phx::crc32(buf, got, c);
        n += uint32_t(got);
    }
    const bool read = std::ferror(f) == 0;
    const bool closed = std::fclose(f) == 0;
    if (!read || !closed) return false;
    crc = phx::crc32_finish(c); size = n;
    return true;
}

inline bool lock_read(const std::string& path, LockFile& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    out = LockFile{};
    char line[4096];
    bool ok = std::fgets(line, sizeof(line), f) && std::strncmp(line, "phxpack-lock 1", 14) == 0;
    while (ok && std::fgets(line, sizeof(line), f)) {
        unsigned a = 0, b = 0; int t = 0;
        if (std::sscanf(line, "tool %u", &a) == 1)                out.tool = a;
        else if (std::sscanf(line, "bundle-version %u", &a) == 1) out.bundle_version = uint16_t(a);
        else if (std::sscanf(line, "target %d", &t) == 1)         out.target = t;
        else if (std::sscanf(line, "compress %u", &a) == 1)       out.compress = a != 0;
        else if (std::sscanf(line, "output crc32 %x", &a) == 1)   out.out_crc = a;
        else if (std::sscanf(line, "input crc32 %x size %u", &a, &b) == 2) {
            // path = everything after the third space-separated field
            const char* p = std::strstr(line, "size ");
            if (!p) { ok = false; break; }
            p += 5; while (*p && *p != ' ') ++p; while (*p == ' ') ++p;
            std::string ipath(p);
            while (!ipath.empty() && (ipath.back() == '\n' || ipath.back() == '\r')) ipath.pop_back();
            LockInput in; in.path = ipath; in.crc = a; in.size = b;
            out.inputs.push_back(std::move(in));
        } else if (std::sscanf(line, " asset %x type %u", &a, &b) == 2) {
            if (out.inputs.empty()) { ok = false; break; }
            out.inputs.back().assets.push_back(LockAsset{ a, uint16_t(b) });
        }
        // unknown lines are ignored (forward compatibility within lock format 1)
    }
    std::fclose(f);
    return ok;
}

inline bool lock_write(const std::string& path, const LockFile& l) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fprintf(f, "phxpack-lock 1\n");
    std::fprintf(f, "tool %u\n", l.tool);
    std::fprintf(f, "bundle-version %u\n", l.bundle_version);
    std::fprintf(f, "target %d\n", l.target);
    std::fprintf(f, "compress %u\n", l.compress ? 1u : 0u);
    std::fprintf(f, "output crc32 %08x\n", l.out_crc);
    for (const LockInput& in : l.inputs) {
        std::fprintf(f, "input crc32 %08x size %u %s\n", in.crc, in.size, in.path.c_str());
        for (const LockAsset& a : in.assets)
            std::fprintf(f, " asset %08x type %u\n", a.hash, unsigned(a.type));
    }
    const bool wrote = std::ferror(f) == 0;
    const bool closed = std::fclose(f) == 0;
    return wrote && closed;
}

inline const char* asset_type_name(phx::AssetType t) {
    switch (t) {
    case phx::AssetType::Texture: return "texture";
    case phx::AssetType::Tilemap: return "tilemap";
    case phx::AssetType::Sound:   return "sound";
    case phx::AssetType::Font:    return "font";
    case phx::AssetType::Blob:    return "blob";
    case phx::AssetType::Sprite:  return "sprite";
    case phx::AssetType::Spawns:  return "spawns";
    case phx::AssetType::Dialogue: return "dialogue";
    }
    return "?";
}

// The dev-build manifest (docs/06 §2): one line per asset, hash <-> name <-> source path.
// Merged/reused assets have no recorded name (bundles store hashes only) — marked as such.
inline bool manifest_write(const std::string& path, const std::string& bundle,
                           const BundleWriter& w) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fprintf(f, "# phxpack manifest — %s (%u assets)\n", bundle.c_str(), w.entry_count());
    for (uint32_t i = 0; i < w.entry_count(); ++i) {
        const std::string& nm  = w.entry_name(i);
        const std::string& src = w.entry_source(i);
        std::fprintf(f, "0x%08x %-8s %-16s <- %s\n",
                     w.entry_hash(i), asset_type_name(w.entry_type(i)),
                     nm.empty() ? "(merged)" : nm.c_str(),
                     src.empty() ? "(in-memory)" : src.c_str());
    }
    std::fclose(f);
    return true;
}

} // namespace phxtool
#endif // PHX_TOOLS_LOCK_H
