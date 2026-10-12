// tools/phxpack/main.cpp — the `.phxp` bundle assembler CLI. Host-only (STL allowed).
// Builds a baked, target-encoded bundle the engine mmaps, from either:
//   (a) author-friendly SOURCE files, baked directly via the shared builders (builders.h):
//         *.png/*.ppm -> Texture   *.tmcsv/*.tmj -> Tilemap(+Spawns)
//         *.sprdef    -> Texture+Sprite          *.wav / *.sfx / *.song -> Sound
//         *.font / *.fnt -> Texture+Font   *.dlg -> Dialogue
//   (b) pre-baked INTERMEDIATE files emitted by the per-format converters
//       (phxsprite/phxtile/phxsnd/phxbin) — each is itself a one-source bundle, MERGED in:
//         *.phxspr  *.phxtmap  *.phxsnd  *.phxbin  *.phxviz  *.phxp
// This is the two-stage pipeline of docs/08: converters -> intermediates -> phxpack -> bundle.
// Both paths share one bake implementation (builders.h), so output is identical either way.
//
//   phxpack --out assets.phxp [--target 0|1|2] [--compress] [--manifest] [--full] [--validate-references] <inputs...>
//   phxpack --upgrade old.phxp [--target N] [--compress]
//
//   --compress (-z): LZSS-compress each blob (kept only where it shrinks); the runtime
//                    ResourceCache decompresses on first access (see phx/resource/lz.h).
//   --manifest:      also write <out>.manifest.txt (hash <-> name <-> source, docs/06 §2).
//   --full:          ignore <out>.lock and re-bake everything from scratch.
//   --upgrade:       re-bake <out> from the sources recorded in its <out>.lock (after a
//                    tool/format upgrade; no input list needed).
//
// INCREMENTAL + REPRODUCIBLE (docs/08 §2/§9, tools/phxpack/lock.h): every bake writes
// <out>.lock (tool + format version, target, per-input content hashes, per-input asset
// list, output CRC32). On the next run, inputs whose bytes are unchanged are REUSED from
// the previous bundle instead of re-baked; if nothing changed at all the bake is skipped.
// An incremental rebake writes the same bytes a full bake would (asserted by the pipeline
// suite), so the lock is an optimization, never a semantic.
#include "builders.h"
#include "bundle_reader.h"
#include "lock.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

bool is_intermediate(const std::string& s) {
    return phxtool::ends_with(s, ".phxspr") || phxtool::ends_with(s, ".phxtmap") ||
           phxtool::ends_with(s, ".phxsnd") || phxtool::ends_with(s, ".phxbin")  ||
           phxtool::ends_with(s, ".phxviz") || phxtool::ends_with(s, ".phxp");
}

// Merge a pre-baked converter bundle into the assembler's output (names already hashed).
// Refuses an intermediate baked for a different target tier: blobs are per-target encoded,
// so mixing tiers must fail loudly here, not decode garbage on a console.
bool merge_intermediate(phxtool::BundleWriter& w, const std::string& in, int target) {
    std::vector<phxtool::ReadAsset> assets;
    phx::BundleHeader hdr{};
    if (!phxtool::bundle_read(in, assets, &hdr)) {
        std::fprintf(stderr, "phxpack: bad/unsupported bundle '%s'\n", in.c_str()); return false; }
    if (int(hdr.target) != target) {
        std::fprintf(stderr, "phxpack: '%s' was baked for target %d, this bundle is target %d\n",
                     in.c_str(), int(hdr.target), target);
        return false;
    }
    for (auto& a : assets) w.add_raw(a.hash, a.type, std::move(a.blob));
    std::printf("  + merged  %-12s %u asset(s)  (%s)\n",
                phxtool::stem(in).c_str(), unsigned(assets.size()), in.c_str());
    return true;
}

} // namespace

int main(int argc, char** argv) {
    std::string out, upgrade;
    int  target = 2;
    bool validate_references = false;
    bool target_set = false, compress = false, manifest = false, full = false;
    std::vector<std::string> inputs;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--out" && i + 1 < argc)          out = argv[++i];
        else if (a == "--target" && i + 1 < argc)  { target = std::atoi(argv[++i]); target_set = true; }
        else if (a == "--compress" || a == "-z")   compress = true;
        else if (a == "--manifest")                manifest = true;
        else if (a == "--validate-references") { validate_references = true; full = true; }
        else if (a == "--full")                    full = true;
        else if (a == "--upgrade" && i + 1 < argc) upgrade = argv[++i];
        else if (a == "--help" || a == "-h") {
            std::printf("usage: phxpack --out FILE [--target 0|1|2] [--compress] [--manifest] [--full] [--validate-references] <inputs...>\n"
                        "       phxpack --upgrade FILE.phxp [--target 0|1|2] [--compress]\n"
                        "  sources:       .png .ppm .tmcsv .tmj .wav .sfx .song .sprdef .font .fnt .dlg\n"
                        "  intermediates: .phxspr .phxtmap .phxsnd .phxbin .phxviz .phxp  (merged)\n"
                        "  sidecars:      FILE.lock (always; drives incremental rebakes),\n"
                        "                 FILE.manifest.txt (--manifest; hash <-> name <-> source)\n");
            return 0;
        } else inputs.push_back(a);
    }

    // --upgrade: the input list (and, unless overridden, the config) comes from the lock.
    if (!upgrade.empty()) {
        phxtool::LockFile l;
        if (!phxtool::lock_read(upgrade + ".lock", l)) {
            std::fprintf(stderr, "phxpack: --upgrade needs '%s.lock' (bake it once first)\n",
                         upgrade.c_str());
            return 2;
        }
        out = out.empty() ? upgrade : out;
        if (!target_set) target = l.target;
        compress = compress || l.compress;
        inputs.clear();
        for (const auto& in : l.inputs) inputs.push_back(in.path);
        full = true;                                  // the whole point: re-bake everything
        std::printf("phxpack: upgrading %s from its lock (%u inputs)\n",
                    out.c_str(), unsigned(inputs.size()));
    }
    if (out.empty()) { std::fprintf(stderr, "phxpack: --out is required (try --help)\n"); return 2; }

    // Hash the inputs up front — the lock's currency for "changed".
    struct CurInput { std::string path; uint32_t crc = 0, size = 0; };
    std::vector<CurInput> cur(inputs.size());
    for (size_t i = 0; i < inputs.size(); ++i) {
        cur[i].path = inputs[i];
        if (!phxtool::file_crc32(inputs[i], cur[i].crc, cur[i].size)) {
            std::fprintf(stderr, "phxpack: cannot read '%s'\n", inputs[i].c_str());
            return 1;
        }
    }

    // Load the previous lock and decide what the previous bundle can still vouch for.
    const std::string lock_path = out + ".lock";
    phxtool::LockFile prev;
    bool prev_ok = !full && phxtool::lock_read(lock_path, prev)
                   && prev.tool == phxtool::kPhxpackToolVersion
                   && prev.bundle_version == phx::kBundleVersion
                   && prev.target == target && prev.compress == compress;
    if (prev_ok) {                                   // the output must be the recorded bake
        uint32_t ocrc = 0, osize = 0;
        prev_ok = phxtool::file_crc32(out, ocrc, osize) && ocrc == prev.out_crc;
    }

    // Fully up to date? (same input list, same bytes, in order)
    if (prev_ok && prev.inputs.size() == cur.size()) {
        bool same = true;
        for (size_t i = 0; i < cur.size() && same; ++i)
            same = prev.inputs[i].path == cur[i].path && prev.inputs[i].crc == cur[i].crc
                   && prev.inputs[i].size == cur[i].size;
        if (same) {
            std::printf("phxpack: %s up to date (%u inputs unchanged)\n",
                        out.c_str(), unsigned(cur.size()));
            return 0;
        }
    }

    // Previous bundle's assets, for per-asset reuse of unchanged inputs.
    std::vector<phxtool::ReadAsset> old_assets;
    if (prev_ok && !phxtool::bundle_read(out, old_assets)) prev_ok = false;

    phxtool::BundleWriter w{uint8_t(target)};
    w.set_compression(compress);

    phxtool::LockFile next;
    next.tool = phxtool::kPhxpackToolVersion;
    next.bundle_version = phx::kBundleVersion;
    next.target = target; next.compress = compress;

    uint32_t reused = 0;
    std::vector<bool> old_used(old_assets.size(), false);
    for (size_t i = 0; i < cur.size(); ++i) {
        phxtool::LockInput rec;
        rec.path = cur[i].path; rec.crc = cur[i].crc; rec.size = cur[i].size;

        // Unchanged since the recorded bake? Lift its assets straight out of the old bundle.
        const phxtool::LockInput* was = nullptr;
        if (prev_ok)
            for (const auto& pi : prev.inputs)
                if (pi.path == cur[i].path && pi.crc == cur[i].crc && pi.size == cur[i].size
                    && !pi.assets.empty()) { was = &pi; break; }
        bool lifted = false;
        if (was) {
            lifted = true;
            std::vector<size_t> picks;
            for (const auto& a : was->assets) {
                size_t hit = old_assets.size();
                for (size_t k = 0; k < old_assets.size(); ++k)
                    if (!old_used[k] && old_assets[k].hash == a.hash
                        && uint16_t(old_assets[k].type) == a.type) { hit = k; break; }
                if (hit == old_assets.size()) { lifted = false; break; }
                picks.push_back(hit);
            }
            if (lifted) {
                w.set_source(cur[i].path);
                for (size_t k : picks) {
                    old_used[k] = true;
                    w.add_raw(old_assets[k].hash, old_assets[k].type,
                              std::move(old_assets[k].blob));
                }
                rec.assets = was->assets;
                ++reused;
                std::printf("  = reused  %-12s %u asset(s), unchanged  (%s)\n",
                            phxtool::stem(cur[i].path).c_str(),
                            unsigned(was->assets.size()), cur[i].path.c_str());
            }
        }

        if (!lifted) {                                // bake it (source or intermediate)
            w.set_source(cur[i].path);
            const uint32_t before = w.entry_count();
            const bool ok = is_intermediate(cur[i].path)
                                ? merge_intermediate(w, cur[i].path, target)
                                : phxtool::build_from_source(w, cur[i].path);
            if (!ok) return 1;
            for (uint32_t e = before; e < w.entry_count(); ++e)
                rec.assets.push_back(phxtool::LockAsset{ w.entry_hash(e),
                                                         uint16_t(w.entry_type(e)) });
        }
        next.inputs.push_back(std::move(rec));
    }

    if (!w.validate_inputs(validate_references)) return 1;
    if (!w.write(out)) { std::fprintf(stderr, "phxpack: cannot write '%s'\n", out.c_str()); return 1; }
    uint32_t output_crc = 0, output_size = 0;
    if (!phxtool::file_crc32(out, output_crc, output_size) || output_crc != w.file_crc32()) {
        std::fprintf(stderr, "phxpack: '%s' [Bundle]: written output failed checksum verification\n", out.c_str());
        return 1;
    }
    next.out_crc = output_crc;
    if (!phxtool::lock_write(lock_path, next)) {
        std::fprintf(stderr, "phxpack: cannot write '%s'\n", lock_path.c_str());
        return 1;
    }
    if (manifest) {
        const std::string mpath = out + ".manifest.txt";
        if (phxtool::manifest_write(mpath, out, w)) std::printf("  + manifest %s\n", mpath.c_str());
        else std::fprintf(stderr, "phxpack: warning: cannot write '%s'\n", mpath.c_str());
    }
    std::printf("phxpack: wrote %s  (%u assets, target tier %d%s%s)\n",
                out.c_str(), w.count(), target, compress ? ", LZ-compressed" : "",
                reused ? " — incremental" : "");
    if (reused)
        std::printf("phxpack: reused %u unchanged input(s), re-baked %u\n",
                    reused, unsigned(cur.size() - reused));
    return 0;
}
