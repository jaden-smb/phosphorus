// tools/phxpack/bundle_reader.h — host-side reader for an existing `.phxp` bundle (or an
// intermediate `.phxspr/.phxtmap/.phxsnd/.phxbin`, which are themselves single-source bundles).
// Lets the phxpack assembler MERGE pre-baked converter output into a combined bundle. STL is
// fine here (host-only). Shares the format with the runtime reader (phx/resource/bundle.h).
#ifndef PHX_TOOLS_BUNDLE_READER_H
#define PHX_TOOLS_BUNDLE_READER_H

#include "phx/resource/bundle.h"
#include "phx/resource/lz.h"
#include "phx/core/crc32.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace phxtool {

struct ReadAsset {
    phx::NameHash  hash;
    phx::AssetType type;
    std::vector<uint8_t> blob;   // always decompressed
};

// Parse `path` into its assets. Returns false on I/O error or malformed/foreign bundle.
// `hdr_out` (optional) receives the validated bundle header — e.g. so the assembler can
// refuse to merge an intermediate baked for a DIFFERENT target tier (now that blobs are
// per-target encoded, a tier mix-up must fail at pack time, not render wrong on device).
inline bool bundle_read(const std::string& path, std::vector<ReadAsset>& out,
                        phx::BundleHeader* hdr_out = nullptr) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz < long(sizeof(phx::BundleHeader))) { std::fclose(f); return false; }
    std::vector<uint8_t> buf;
    buf.resize(size_t(sz));
    size_t got = std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    if (got != buf.size()) return false;

    phx::BundleHeader h{};
    std::memcpy(&h, buf.data(), sizeof(h));
    if (h.magic != phx::kBundleMagic || h.version != phx::kBundleVersion) return false;
    if (h.target > 2 || h.total_size != buf.size() || h.toc_offset < sizeof(h) || h.toc_offset > buf.size()) return false;
    // Keep the format's explicit CRC=0 opt-out, but never merge a corrupt checked bundle.
    if (h.blob_crc32 && h.blob_crc32 != phx::crc32_of(buf.data() + h.toc_offset, buf.size() - h.toc_offset)) return false;
    if (size_t(h.toc_offset) + size_t(h.asset_count) * sizeof(phx::TocEntry) > buf.size()) return false;
    if (hdr_out) *hdr_out = h;

    out.clear();
    out.reserve(h.asset_count);
    for (uint32_t i = 0; i < h.asset_count; ++i) {
        phx::TocEntry e{};
        std::memcpy(&e, buf.data() + h.toc_offset + i * sizeof(phx::TocEntry), sizeof(e));
        if (e.offset < size_t(h.toc_offset) + size_t(h.asset_count) * sizeof(phx::TocEntry) ||
            size_t(e.offset) + e.size > buf.size()) return false;
        ReadAsset a;
        a.hash = e.name_hash;
        a.type = phx::AssetType(e.type);
        a.blob.resize(e.usize);
        if (e.flags & phx::kTocLZ) {
            if (phx::lz_decode(buf.data() + e.offset, e.size, a.blob.data(), e.usize) != e.usize) return false;
        } else {
            if (e.size != e.usize) return false;
            std::memcpy(a.blob.data(), buf.data() + e.offset, e.usize);
        }
        out.push_back(std::move(a));
    }
    return true;
}

} // namespace phxtool
#endif // PHX_TOOLS_BUNDLE_READER_H
