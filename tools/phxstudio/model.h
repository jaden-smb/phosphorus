// tools/phxstudio/model.h — Phosphorus Studio's headless document model (docs/gui-editor-feasibility.md
// §6). Everything the studio SHOWS is computed here as pure C++ over the real tree and the real
// bundle format, with no window, renderer or process involved — so it is unit-tested in the
// pipeline suite exactly like phxtmap's TmapDoc and phxentity's BinDoc. The GUI shell
// (main.cpp) only lays this data out. Host-only: STL + std::filesystem are fine here.
//
//   EngineMap   the module graph as BUILT: layers parsed from tools/common/depcheck.py (the one
//               source of truth for the dependency law), edges scanned from real #includes,
//               per-module headers / backends / line counts / header summary.
//   NameBook    bundles store FNV-1a name hashes only; this recovers readable names by hashing
//               every string literal in the example/tool/test sources + phxpack manifests.
//   BundleDoc   a `.phxp` read + validated the way ResourceCache::mount does (magic, version,
//               TOC/blob bounds, CRC32), with typed views over each decompressed blob.
//   TierReport  one texture re-encoded for every render tier with the SAME encoders the bake
//               uses (tools/phxpack/tex_encode.h), plus the GBA per-tile colour budget.
//   Launch      the runnable catalog (games, editors, gates, suites, cross builds), with the
//               suite list read from the Makefile's `check:` line so it can never drift.
//   LogRing     child-process output split into classified lines for the console panel.
#ifndef PHX_TOOLS_PHXSTUDIO_MODEL_H
#define PHX_TOOLS_PHXSTUDIO_MODEL_H

#include "phx/core/crc32.h"
#include "phx/core/pixel.h"
#include "phx/core/types.h"
#include "phx/resource/bundle.h"
#include "phx/resource/lz.h"

#include "tex_encode.h"   // tools/phxpack: the bake's per-target texture encoders
#include "twk_geom.h"     // tools/common: the widget kit's Rect + scroll math

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace phxstudio {

namespace fs = std::filesystem;

// ============================================================================================
// small helpers
// ============================================================================================

inline bool read_text(const std::string& path, std::string& out) {
    out.clear();
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[8192];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    std::fclose(f);
    return true;
}

inline bool read_bytes(const std::string& path, std::vector<uint8_t>& out) {
    out.clear();
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.insert(out.end(), buf, buf + n);
    std::fclose(f);
    return true;
}

inline bool starts_with(const std::string& s, const char* p) {
    const size_t n = std::strlen(p);
    return s.size() >= n && s.compare(0, n, p) == 0;
}

inline std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

inline std::string lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

// "1.2 KB" style byte counts for labels.
// Whole units print without decimals ("224 KB", "24 MB") so table cells stay narrow.
inline std::string human_bytes(uint64_t n) {
    char b[32];
    const uint64_t kb = 1024, mb = 1024 * 1024;
    if (n < kb)               std::snprintf(b, sizeof(b), "%llu B", (unsigned long long)n);
    else if (n < mb)          (n % kb) ? std::snprintf(b, sizeof(b), "%.1f KB", double(n) / 1024.0)
                                       : std::snprintf(b, sizeof(b), "%llu KB", (unsigned long long)(n / kb));
    else                      (n % mb) ? std::snprintf(b, sizeof(b), "%.2f MB", double(n) / double(mb))
                                       : std::snprintf(b, sizeof(b), "%llu MB", (unsigned long long)(n / mb));
    return b;
}

// Greedy word wrap to `cols` characters per line; honours '\n'. Words longer than a line are
// hard-split so nothing is ever clipped silently.
inline std::vector<std::string> wrap(const std::string& text, size_t cols) {
    std::vector<std::string> out;
    if (cols == 0) return out;
    std::string line;
    size_t i = 0;
    auto flush = [&]() { out.push_back(line); line.clear(); };
    while (i < text.size()) {
        if (text[i] == '\n') { flush(); ++i; continue; }
        if (text[i] == ' ') { ++i; continue; }
        size_t j = i;
        while (j < text.size() && text[j] != ' ' && text[j] != '\n') ++j;
        std::string word = text.substr(i, j - i);
        i = j;
        while (word.size() > cols) {                        // hard-split an over-long word
            if (!line.empty()) flush();
            out.push_back(word.substr(0, cols));
            word.erase(0, cols);
        }
        if (line.empty())                           line = word;
        else if (line.size() + 1 + word.size() <= cols) line += ' ' + word;
        else { flush(); line = word; }
    }
    if (!line.empty()) out.push_back(line);
    return out;
}

// ============================================================================================
// EngineMap — the module dependency graph as it exists in the tree
// ============================================================================================

// Parse the `LAYERS = [ [...], ... ]` table out of tools/common/depcheck.py, so the studio and
// the build gate can never disagree about which module sits on which layer.
// `notes` (optional) receives each layer's trailing comment ("services", "gameplay systems").
inline bool parse_depcheck_layers(const std::string& py, std::vector<std::vector<std::string>>& out,
                                  std::vector<std::string>* notes = nullptr) {
    out.clear();
    if (notes) notes->clear();
    const size_t at = py.find("LAYERS = [");
    if (at == std::string::npos) return false;
    size_t p = py.find('\n', at);
    while (p != std::string::npos && p < py.size()) {
        const size_t e = py.find('\n', p + 1);
        std::string line = py.substr(p + 1, (e == std::string::npos ? py.size() : e) - p - 1);
        const size_t hash = line.find('#');
        std::string note;
        if (hash != std::string::npos) {
            note = trim(line.substr(hash + 1));
            line.erase(hash);
            const size_t colon = note.find(':');                 // "L2: services" -> "services"
            if (colon != std::string::npos && colon < 4) note = trim(note.substr(colon + 1));
        }
        if (trim(line) == "]") break;
        std::vector<std::string> layer;
        size_t q = 0;
        while ((q = line.find('"', q)) != std::string::npos) {
            const size_t r = line.find('"', q + 1);
            if (r == std::string::npos) break;
            layer.push_back(line.substr(q + 1, r - q - 1));
            q = r + 1;
        }
        if (!layer.empty()) {
            out.push_back(layer);
            if (notes) notes->push_back(note);
        }
        p = e;
    }
    return !out.empty();
}

// The leading comment block of a header, de-commented and joined into one paragraph, with the
// conventional "phx/x/y.h — " self-reference stripped (every engine header opens that way).
inline std::string header_summary(const std::string& text) {
    std::string out;
    size_t p = 0;
    bool in_block = false;
    while (p < text.size()) {
        size_t e = text.find('\n', p);
        if (e == std::string::npos) e = text.size();
        std::string line = trim(text.substr(p, e - p));
        p = e + 1;
        if (in_block) {
            const size_t close = line.find("*/");
            if (close != std::string::npos) { line.erase(close); in_block = false; }
            if (starts_with(line, "*")) line.erase(0, 1);
        } else if (starts_with(line, "//")) {
            line.erase(0, 2);
        } else if (starts_with(line, "/*")) {
            line.erase(0, 2);
            const size_t close = line.find("*/");
            if (close != std::string::npos) line.erase(close); else in_block = true;
        } else {
            break;
        }
        line = trim(line);
        if (line.empty()) { if (!out.empty() && !in_block) break; continue; }   // paragraph end
        if (!out.empty()) out += ' ';
        out += line;
    }
    // "phx/ui/ui.h — an immediate-mode UI ..." -> "an immediate-mode UI ..."
    static const char* kDash = "\xE2\x80\x94";                 // UTF-8 em dash
    const size_t d = out.find(kDash);
    if (d != std::string::npos && d < 64) out = trim(out.substr(d + 3));
    // the bitmap font is ASCII-only: fold the typographic characters the docs use
    std::string ascii;
    for (size_t i = 0; i < out.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(out[i]);
        if (c < 0x80) { ascii += char(c); continue; }
        if (c == 0xE2 && i + 2 < out.size()) {
            const unsigned char c1 = static_cast<unsigned char>(out[i + 1]);
            const unsigned char c2 = static_cast<unsigned char>(out[i + 2]);
            if (c1 == 0x80 && (c2 == 0x94 || c2 == 0x93)) ascii += '-';           // em/en dash
            else if (c1 == 0x86 && c2 == 0x92)            ascii += "->";          // arrow
            else if (c1 == 0x80 && (c2 == 0x98 || c2 == 0x99)) ascii += '\'';
            else if (c1 == 0x80 && (c2 == 0x9C || c2 == 0x9D)) ascii += '"';
            else if (c1 == 0x80 && c2 == 0xA6)            ascii += "...";
            else                                          ascii += '?';
            i += 2;
        } else if (c == 0xC3 && i + 1 < out.size() && static_cast<unsigned char>(out[i + 1]) == 0x97) {
            ascii += 'x'; ++i;                                                     // ×
        } else if (c == 0xC2 && i + 1 < out.size() && static_cast<unsigned char>(out[i + 1]) == 0xA7) {
            ascii += 'S'; ++i;                                                     // § (section)
        } else {
            while (i + 1 < out.size() && (static_cast<unsigned char>(out[i + 1]) & 0xC0) == 0x80) ++i;
            ascii += '?';
        }
    }
    return ascii;
}

// Every `phx/<module>/` a source file includes (quoted or angled), in order of appearance.
inline std::vector<std::string> included_modules(const std::string& text) {
    std::vector<std::string> mods;
    size_t p = 0;
    while ((p = text.find("#", p)) != std::string::npos) {
        size_t q = p + 1;
        while (q < text.size() && (text[q] == ' ' || text[q] == '\t')) ++q;
        if (text.compare(q, 7, "include") != 0) { p = q; continue; }
        q += 7;
        while (q < text.size() && (text[q] == ' ' || text[q] == '\t')) ++q;
        if (q < text.size() && (text[q] == '"' || text[q] == '<') && text.compare(q + 1, 4, "phx/") == 0) {
            const size_t s = q + 5;
            size_t t = s;
            while (t < text.size() && ((text[t] >= 'a' && text[t] <= 'z') || text[t] == '_')) ++t;
            if (t < text.size() && text[t] == '/' && t > s) mods.push_back(text.substr(s, t - s));
        }
        p = q;
    }
    return mods;
}

struct Module {
    std::string name;
    int layer = -1;
    std::vector<std::string> headers;    // file names under include/phx/<name>/
    std::vector<std::string> backends;   // subfolders of src/ (one is linked per build)
    int files = 0;
    int lines = 0;
    std::string summary;                 // leading comment of the module's main header
    std::vector<int> deps;               // modules this one includes (direct edges)
    std::vector<int> users;              // modules that include this one
};

struct EngineMap {
    std::vector<std::vector<std::string>> layers;   // from depcheck.py, L0 first
    std::vector<std::string> layer_notes;            // depcheck's comment per layer
    std::vector<Module> modules;                     // ordered by layer, then table order
    std::vector<std::string> violations;             // "a (L2) -> b (L3)" upward/sideways edges
    int edges = 0;
    int total_files = 0;
    int total_lines = 0;

    int find(const std::string& name) const {
        for (size_t i = 0; i < modules.size(); ++i) if (modules[i].name == name) return int(i);
        return -1;
    }
    // Transitive reachability: does `from` depend on `to`, directly or through others?
    bool reaches(int from, int to) const {
        if (from < 0 || to < 0) return false;
        std::vector<char> seen(modules.size(), 0);
        std::vector<int> stack{ from };
        while (!stack.empty()) {
            const int m = stack.back(); stack.pop_back();
            for (int d : modules[size_t(m)].deps) {
                if (d == to) return true;
                if (!seen[size_t(d)]) { seen[size_t(d)] = 1; stack.push_back(d); }
            }
        }
        return false;
    }
};

inline int count_lines(const std::string& text) {
    int n = 0;
    for (char c : text) if (c == '\n') ++n;
    if (!text.empty() && text.back() != '\n') ++n;
    return n;
}

inline bool is_source_file(const fs::path& p) {
    const std::string e = p.extension().string();
    return e == ".h" || e == ".hpp" || e == ".c" || e == ".cpp";
}

// Scan `<root>/engine` against the layer table in `<root>/tools/common/depcheck.py`.
inline bool scan_engine(const std::string& root, EngineMap& out, std::string* err = nullptr) {
    out = EngineMap{};
    std::string py;
    if (!read_text(root + "/tools/common/depcheck.py", py) ||
        !parse_depcheck_layers(py, out.layers, &out.layer_notes)) {
        if (err) *err = "cannot read the layer table from tools/common/depcheck.py";
        return false;
    }
    for (size_t l = 0; l < out.layers.size(); ++l)
        for (const std::string& m : out.layers[l]) {
            Module mod;
            mod.name = m;
            mod.layer = int(l);
            out.modules.push_back(mod);
        }

    std::error_code ec;
    std::set<std::pair<int, int>> edge_set;
    for (size_t mi = 0; mi < out.modules.size(); ++mi) {
        Module& mod = out.modules[mi];
        const fs::path dir = fs::path(root) / "engine" / mod.name;
        if (!fs::is_directory(dir, ec)) continue;

        const fs::path inc = dir / "include" / "phx" / mod.name;
        if (fs::is_directory(inc, ec))
            for (const auto& e : fs::directory_iterator(inc, ec))
                if (e.is_regular_file(ec) && is_source_file(e.path()))
                    mod.headers.push_back(e.path().filename().string());
        std::sort(mod.headers.begin(), mod.headers.end());

        const fs::path src = dir / "src";
        if (fs::is_directory(src, ec))
            for (const auto& e : fs::directory_iterator(src, ec))
                if (e.is_directory(ec)) mod.backends.push_back(e.path().filename().string());
        std::sort(mod.backends.begin(), mod.backends.end());

        for (auto it = fs::recursive_directory_iterator(dir, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (!it->is_regular_file(ec) || !is_source_file(it->path())) continue;
            std::string text;
            if (!read_text(it->path().string(), text)) continue;
            ++mod.files;
            mod.lines += count_lines(text);
            for (const std::string& dst : included_modules(text)) {
                const int di = out.find(dst);
                if (di < 0 || di == int(mi)) continue;
                edge_set.insert({ int(mi), di });
            }
        }

        // summary: the module's main header (its namesake, or the one that defines its central
        // type), else the header with the longest leading comment
        static const char* const kMain[][2] = {
            { "core", "types.h" }, { "memory", "allocators.h" }, { "render", "renderer.h" },
            { "audio", "mixer.h" }, { "resource", "cache.h" }, { "ecs", "world.h" }, { "runtime", "app.h" },
        };
        std::string main_h = mod.name + ".h";
        for (const auto& m : kMain) if (mod.name == m[0]) main_h = m[1];
        std::string best;
        for (const std::string& h : mod.headers) {
            std::string text;
            if (!read_text((inc / h).string(), text)) continue;
            const std::string s = header_summary(text);
            if (h == main_h) { best = s; break; }
            if (s.size() > best.size()) best = s;
        }
        mod.summary = best;
        out.total_files += mod.files;
        out.total_lines += mod.lines;
    }

    for (const auto& e : edge_set) {
        Module& a = out.modules[size_t(e.first)];
        const Module& b = out.modules[size_t(e.second)];
        a.deps.push_back(e.second);
        out.modules[size_t(e.second)].users.push_back(e.first);
        if (b.layer >= a.layer) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "%s (L%d) -> %s (L%d)", a.name.c_str(), a.layer,
                          b.name.c_str(), b.layer);
            out.violations.push_back(buf);
        }
    }
    out.edges = int(edge_set.size());
    return true;
}

// ============================================================================================
// TierCaps — the per-target capability table, parsed from phx/core/caps.h
// ============================================================================================

// A host build only ever sees ITS tier's caps macros, so the three-way comparison is read
// straight out of the header's #if/#elif/#else blocks instead of being copied here.
struct TierCaps {
    std::string name;                 // "GBA" / "PSP" / "PC"
    uint64_t total_ram = 0, scratch_ram = 0, max_entities = 0, max_sprites = 0, audio_channels = 0;
    int has_float_hw = 0, has_filesystem = 0, render_tier = -1;
};

// "(224u * 1024u)" -> 229376. Products of integer literals only (all caps.h uses).
inline uint64_t eval_product(std::string e) {
    const size_t c = e.find("//");
    if (c != std::string::npos) e.erase(c);
    uint64_t v = 1;
    bool any = false;
    size_t i = 0;
    while (i < e.size()) {
        if (e[i] >= '0' && e[i] <= '9') {
            uint64_t n = 0;
            while (i < e.size() && e[i] >= '0' && e[i] <= '9') n = n * 10 + uint64_t(e[i++] - '0');
            v *= n;
            any = true;
        } else {
            ++i;
        }
    }
    return any ? v : 0;
}

inline bool parse_caps(const std::string& text, std::vector<TierCaps>& out) {
    out.clear();
    TierCaps* cur = nullptr;
    size_t p = 0;
    while (p < text.size()) {
        size_t e = text.find('\n', p);
        if (e == std::string::npos) e = text.size();
        const std::string line = trim(text.substr(p, e - p));
        p = e + 1;
        if (starts_with(line, "#if defined(PHX_TARGET_") || starts_with(line, "#elif defined(PHX_TARGET_")) {
            const size_t a = line.find("PHX_TARGET_") + 11, b = line.find(')', a);
            out.push_back(TierCaps{});
            out.back().name = line.substr(a, b - a);
            cur = &out.back();
        } else if (starts_with(line, "#else") && cur) {
            out.push_back(TierCaps{});
            out.back().name = "PC";
            cur = &out.back();
        } else if (starts_with(line, "#endif") && cur) {
            break;
        } else if (cur && starts_with(line, "#define PHX_CAPS_")) {
            const size_t ns = 17, ne = line.find_first_of(" \t", ns);
            if (ne == std::string::npos) continue;
            const std::string key = line.substr(ns, ne - ns);
            const uint64_t v = eval_product(line.substr(ne));
            if      (key == "TOTAL_RAM")      cur->total_ram = v;
            else if (key == "SCRATCH_RAM")    cur->scratch_ram = v;
            else if (key == "MAX_ENTITIES")   cur->max_entities = v;
            else if (key == "MAX_SPRITES")    cur->max_sprites = v;
            else if (key == "AUDIO_CHANNELS") cur->audio_channels = v;
            else if (key == "HAS_FLOAT_HW")   cur->has_float_hw = int(v);
            else if (key == "HAS_FILESYSTEM") cur->has_filesystem = int(v);
            else if (key == "RENDER_TIER")    cur->render_tier = int(v);
        }
    }
    return out.size() >= 2;
}

// ============================================================================================
// NameBook — readable names for the FNV-1a hashes a bundle stores
// ============================================================================================

class NameBook {
public:
    void add(const std::string& name) {
        if (name.empty() || name.size() > 64) return;
        names_.emplace(phx::fnv1a_n(name.c_str(), name.size()), name);
    }

    // Every "string literal" made of identifier/path characters — asset, clip and spawn-type
    // names in the bake sources are exactly these (w.add_sprite("hero", ...), "coin"_hash).
    void add_literals(const std::string& src) {
        size_t p = 0;
        while ((p = src.find('"', p)) != std::string::npos) {
            size_t q = p + 1;
            bool ok = true;
            while (q < src.size() && src[q] != '"') {
                const char c = src[q];
                if (c == '\n' || c == '\\') { ok = false; break; }
                const bool ident = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                   (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == '/';
                if (!ident) ok = false;
                ++q;
            }
            if (q >= src.size()) break;
            if (ok && q > p + 1) add(src.substr(p + 1, q - p - 1));
            p = q + 1;
        }
    }

    // phxpack --manifest lines: "0x0cb633fd texture  li_t   <- build/li_t.ppm"
    void add_manifest(const std::string& text) {
        size_t p = 0;
        while (p < text.size()) {
            size_t e = text.find('\n', p);
            if (e == std::string::npos) e = text.size();
            const std::string line = text.substr(p, e - p);
            p = e + 1;
            if (!starts_with(line, "0x")) continue;
            char kind[32] = {0}, name[96] = {0};
            unsigned hash = 0;
            if (std::sscanf(line.c_str(), "0x%x %31s %95s", &hash, kind, name) == 3)
                names_.emplace(phx::NameHash(hash), name);
        }
    }

    const std::string* find(phx::NameHash h) const {
        const auto it = names_.find(h);
        return it == names_.end() ? nullptr : &it->second;
    }
    // The name, or the raw hash as "#0cb633fd" when it cannot be recovered.
    std::string label(phx::NameHash h) const {
        if (const std::string* s = find(h)) return *s;
        char b[16];
        std::snprintf(b, sizeof(b), "#%08x", unsigned(h));
        return b;
    }
    size_t size() const { return names_.size(); }

private:
    std::unordered_map<phx::NameHash, std::string> names_;
};

// Harvest names from every source under examples/, tools/ and tests/, the stems of asset-ish
// files, and every phxpack manifest in the root and build/.
// Recover names from a GAME PROJECT only (project mode never reads the engine's sources): every
// string literal in its code + data, every asset file stem, and any phxpack manifest in the project
// folder, its build/ folder, or next to the extra bundles it lists.
inline void scan_names_in(const std::string& dir, const std::vector<std::string>& bundle_paths, NameBook& book) {
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const fs::path& p = it->path();
        const std::string ext = p.extension().string();
        if (is_source_file(p) || ext == ".json" || ext == ".tmj" || ext == ".sprdef") {
            std::string text;
            if (read_text(p.string(), text)) book.add_literals(text);
        }
        if (ext == ".png" || ext == ".wav" || ext == ".sfx" || ext == ".song" || ext == ".font" || ext == ".fnt" || ext == ".dlg" || ext == ".tmj" || ext == ".json" || ext == ".sprdef")
            book.add(p.stem().string());
        const std::string n = p.filename().string();
        if (n.size() > 13 && n.compare(n.size() - 13, 13, ".manifest.txt") == 0) {
            std::string text;
            if (read_text(p.string(), text)) book.add_manifest(text);
        }
    }
    for (const std::string& b : bundle_paths) {
        std::string text;
        if (read_text(b + ".manifest.txt", text)) book.add_manifest(text);
    }
}

inline void scan_names(const std::string& root, NameBook& book) {
    std::error_code ec;
    for (const char* sub : { "examples", "tools", "tests" }) {
        const fs::path dir = fs::path(root) / sub;
        for (auto it = fs::recursive_directory_iterator(dir, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (!it->is_regular_file(ec)) continue;
            const fs::path& p = it->path();
            const std::string ext = p.extension().string();
            if (is_source_file(p) || ext == ".json" || ext == ".tmj" || ext == ".sprdef") {
                std::string text;
                if (read_text(p.string(), text)) book.add_literals(text);
            }
            if (ext == ".png" || ext == ".wav" || ext == ".sfx" || ext == ".song" || ext == ".font" || ext == ".fnt" || ext == ".dlg" || ext == ".tmj" || ext == ".json" || ext == ".sprdef")
                book.add(p.stem().string());
        }
    }
    for (const fs::path& dir : { fs::path(root), fs::path(root) / "build" }) {
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            const std::string n = e.path().filename().string();
            if (n.size() > 13 && n.compare(n.size() - 13, 13, ".manifest.txt") == 0) {
                std::string text;
                if (read_text(e.path().string(), text)) book.add_manifest(text);
            }
        }
    }
}

// ============================================================================================
// BundleDoc — one .phxp, validated like ResourceCache::mount, with typed views per asset
// ============================================================================================

inline const char* type_name(phx::AssetType t) {
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

inline const char* format_name(phx::PixelFormat f) {
    switch (f) {
    case phx::PixelFormat::RGBA8:      return "RGBA8";
    case phx::PixelFormat::PAL8:       return "PAL8";
    case phx::PixelFormat::PAL4_TILES: return "PAL4_TILES";
    case phx::PixelFormat::RGBA8_SWZ:  return "RGBA8_SWZ";
    }
    return "?";
}

inline const char* tier_name(int tier) {
    switch (tier) {
    case 0: return "GBA (tier 0 - PPU)";
    case 1: return "PSP (tier 1 - GU)";
    case 2: return "PC (tier 2 - GL)";
    }
    return "unknown tier";
}

struct AssetEntry {
    phx::NameHash  hash   = 0;
    phx::AssetType type   = phx::AssetType::Blob;
    uint16_t       flags  = 0;      // TocFlags
    uint32_t       offset = 0;
    uint32_t       size   = 0;      // stored
    uint32_t       usize  = 0;      // decompressed
    bool           ok     = false;  // decoded + in bounds
    std::vector<uint8_t> data;      // the decompressed blob
};

struct BundleDoc {
    std::string path;
    bool        ok = false;          // structurally valid (mountable)
    std::string error;               // why not, when !ok
    phx::BundleHeader hdr{};
    uint64_t    file_size = 0;
    int         crc = 0;             // 1 = matches, -1 = MISMATCH, 0 = header opted out (0)
    std::vector<AssetEntry> assets;  // TOC order (sorted by name hash)

    // Read + validate `p`. Returns false (with `error`) for anything the runtime would refuse;
    // a bad CRC is reported but still loads, so the studio can show what is inside.
    static bool load(const std::string& p, BundleDoc& d) {
        d = BundleDoc{};
        d.path = p;
        std::vector<uint8_t> buf;
        if (!read_bytes(p, buf)) { d.error = "cannot read file"; return false; }
        d.file_size = buf.size();
        if (buf.size() < sizeof(phx::BundleHeader)) { d.error = "truncated: smaller than a header"; return false; }
        std::memcpy(&d.hdr, buf.data(), sizeof(d.hdr));
        if (d.hdr.magic != phx::kBundleMagic) { d.error = "bad magic (not a PHXP bundle)"; return false; }
        if (d.hdr.version != phx::kBundleVersion) {
            char b[96];
            std::snprintf(b, sizeof(b), "format version %u (this engine reads %u)",
                          unsigned(d.hdr.version), unsigned(phx::kBundleVersion));
            d.error = b;
            return false;
        }
        if (d.hdr.total_size != buf.size()) { d.error = "size mismatch (truncated or padded)"; return false; }
        const uint64_t toc_end = uint64_t(d.hdr.toc_offset) + uint64_t(d.hdr.asset_count) * sizeof(phx::TocEntry);
        if (d.hdr.toc_offset < sizeof(phx::BundleHeader) || toc_end > buf.size()) {
            d.error = "TOC out of bounds";
            return false;
        }
        if (d.hdr.blob_crc32 != 0) {
            const uint32_t c = phx::crc32_of(buf.data() + d.hdr.toc_offset, buf.size() - d.hdr.toc_offset);
            d.crc = (c == d.hdr.blob_crc32) ? 1 : -1;
        }
        d.assets.reserve(d.hdr.asset_count);
        for (uint32_t i = 0; i < d.hdr.asset_count; ++i) {
            phx::TocEntry e{};
            std::memcpy(&e, buf.data() + d.hdr.toc_offset + i * sizeof(phx::TocEntry), sizeof(e));
            AssetEntry a;
            a.hash = e.name_hash; a.type = phx::AssetType(e.type); a.flags = e.flags;
            a.offset = e.offset; a.size = e.size; a.usize = e.usize;
            if (uint64_t(e.offset) + e.size > buf.size()) {
                d.error = "blob out of bounds";
                return false;
            }
            a.data.resize(e.usize);
            if (e.flags & phx::kTocLZ) {
                a.ok = phx::lz_decode(buf.data() + e.offset, e.size, a.data.data(), e.usize) == e.usize;
            } else {
                a.ok = e.size == e.usize;
                if (a.ok && e.usize) std::memcpy(a.data.data(), buf.data() + e.offset, e.usize);
            }
            if (!a.ok) { d.error = "corrupt compressed blob"; return false; }
            d.assets.push_back(std::move(a));
        }
        if (d.crc < 0) d.error = "CRC32 mismatch (bit-rot or a hand-edited file)";
        d.ok = true;
        return true;
    }

    int find(phx::NameHash h, phx::AssetType t) const {
        for (size_t i = 0; i < assets.size(); ++i)
            if (assets[i].hash == h && assets[i].type == t) return int(i);
        return -1;
    }
    uint32_t count(phx::AssetType t) const {
        uint32_t n = 0;
        for (const AssetEntry& a : assets) n += a.type == t;
        return n;
    }
    uint64_t stored_bytes() const { uint64_t n = 0; for (const AssetEntry& a : assets) n += a.size;  return n; }
    uint64_t raw_bytes()    const { uint64_t n = 0; for (const AssetEntry& a : assets) n += a.usize; return n; }
};

// ---- typed views over a decompressed blob (host mirrors of the ResourceCache views) ----

struct TexView {
    uint16_t w = 0, h = 0;
    phx::PixelFormat fmt = phx::PixelFormat::RGBA8;
    const uint8_t* payload = nullptr;       // the bytes after TextureBlobHeader
    uint32_t payload_size = 0;
    uint16_t pal_count = 0;                 // PAL4_TILES only
};

inline bool view_texture(const AssetEntry& a, TexView& v) {
    if (a.type != phx::AssetType::Texture) return false;
    if (a.data.size() < sizeof(phx::TextureBlobHeader)) return false;
    phx::TextureBlobHeader th{};
    std::memcpy(&th, a.data.data(), sizeof(th));
    v = TexView{};
    v.w = th.width; v.h = th.height; v.fmt = phx::PixelFormat(th.format);
    v.payload = a.data.data() + sizeof(th);
    v.payload_size = uint32_t(a.data.size() - sizeof(th));
    const uint64_t texels = uint64_t(v.w) * v.h;
    switch (v.fmt) {
    case phx::PixelFormat::RGBA8:
    case phx::PixelFormat::RGBA8_SWZ:
        return texels > 0 && v.payload_size >= texels * 4;
    case phx::PixelFormat::PAL4_TILES: {
        if (v.payload_size < sizeof(phx::TexturePal4Header) || (v.w % 8) || (v.h % 8)) return false;
        phx::TexturePal4Header ph{};
        std::memcpy(&ph, v.payload, sizeof(ph));
        v.pal_count = ph.pal_count;
        return ph.pal_count > 0 &&
               v.payload_size >= phx::pal4_payload_size(ph.pal_count, phx::pal4_tile_count(v.w, v.h));
    }
    default:
        return false;
    }
}

// Any baked texture format -> linear RGBA8 (the inverse of the bake's per-target encode).
inline bool decode_rgba8(const TexView& v, std::vector<uint32_t>& out) {
    const uint32_t w = v.w, h = v.h;
    out.assign(size_t(w) * h, 0u);
    switch (v.fmt) {
    case phx::PixelFormat::RGBA8:
        std::memcpy(out.data(), v.payload, out.size() * 4);
        return true;
    case phx::PixelFormat::RGBA8_SWZ:
        if (!phx::swz_size_ok(v.w, v.h)) return false;
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
                std::memcpy(&out[y * w + x], v.payload + phx::swz_texel_index(x, y, w) * 4u, 4);
        return true;
    case phx::PixelFormat::PAL4_TILES: {
        const uint32_t nt = phx::pal4_tile_count(v.w, v.h);
        const uint8_t* pals  = v.payload + phx::pal4_palettes_off();
        const uint8_t* tpal  = v.payload + phx::pal4_tile_pal_off(v.pal_count);
        const uint8_t* tiles = v.payload + phx::pal4_tile_data_off(v.pal_count, nt);
        const uint32_t tcols = w / 8;
        for (uint32_t t = 0; t < nt; ++t) {
            const uint32_t pal = tpal[t] < v.pal_count ? tpal[t] : 0u;
            for (uint32_t y = 0; y < 8; ++y)
                for (uint32_t x = 0; x < 8; ++x) {
                    const uint8_t n = phx::pal4_texel(tiles, t, x, y);
                    if (n == 0) continue;
                    uint16_t c;
                    std::memcpy(&c, pals + (pal * 16u + n) * 2u, 2);
                    out[((t / tcols) * 8 + y) * w + (t % tcols) * 8 + x] = phx::bgr555_to_rgba8(c);
                }
        }
        return true;
    }
    default:
        return false;
    }
}

struct MapView {
    uint16_t w = 0, h = 0;
    uint8_t  layers = 0, tile_w = 8, tile_h = 8;
    phx::NameHash tileset = 0;
    const uint16_t* indices = nullptr;
    std::vector<int32_t> parallax_q16;    // 2 per layer {fx, fy}; empty = all 1:1
    const uint8_t* tile_flags = nullptr;
    uint32_t tile_flag_count = 0;
};

inline bool view_tilemap(const AssetEntry& a, MapView& v) {
    if (a.type != phx::AssetType::Tilemap || a.data.size() < sizeof(phx::TilemapBlobHeader)) return false;
    phx::TilemapBlobHeader mh{};
    std::memcpy(&mh, a.data.data(), sizeof(mh));
    v = MapView{};
    v.w = mh.width; v.h = mh.height; v.layers = mh.layers;
    v.tile_w = mh.tile_w; v.tile_h = mh.tile_h; v.tileset = mh.tileset;
    size_t end = sizeof(mh) + size_t(v.w) * v.h * v.layers * 2;
    if (end > a.data.size() || v.tile_w == 0 || v.tile_h == 0) return false;
    v.indices = reinterpret_cast<const uint16_t*>(a.data.data() + sizeof(mh));   // vector storage is aligned
    if (mh.flags & phx::kTilemapHasParallax) {
        const size_t off = (end + 3) & ~size_t(3);
        const size_t n = size_t(v.layers) * 2;
        if (off + n * 4 > a.data.size()) return false;
        v.parallax_q16.resize(n);
        std::memcpy(v.parallax_q16.data(), a.data.data() + off, n * 4);
        end = off + n * 4;
    }
    if (mh.flags & phx::kTilemapHasTileFlags) {
        const size_t off = (end + 3) & ~size_t(3);
        if (off + 4 > a.data.size()) return false;
        std::memcpy(&v.tile_flag_count, a.data.data() + off, 4);
        if (off + 4 + v.tile_flag_count > a.data.size()) return false;
        v.tile_flags = a.data.data() + off + 4;
    }
    return true;
}

struct SpriteInfo {
    phx::NameHash texture = 0;
    uint16_t frame_w = 0, frame_h = 0, cols = 0;
    std::vector<phx::SpriteClipDef> clips;
    std::vector<phx::SpriteTransDef> trans;   // the transitions trailer (bundle.h), if any
};

inline bool view_sprite(const AssetEntry& a, SpriteInfo& v) {
    if (a.type != phx::AssetType::Sprite || a.data.size() < sizeof(phx::SpriteBlobHeader)) return false;
    phx::SpriteBlobHeader sh{};
    std::memcpy(&sh, a.data.data(), sizeof(sh));
    if (sizeof(sh) + size_t(sh.clip_count) * sizeof(phx::SpriteClipDef) > a.data.size()) return false;
    v = SpriteInfo{};
    v.texture = sh.texture; v.frame_w = sh.frame_w; v.frame_h = sh.frame_h; v.cols = sh.cols;
    v.clips.resize(sh.clip_count);
    if (sh.clip_count)
        std::memcpy(v.clips.data(), a.data.data() + sizeof(sh), v.clips.size() * sizeof(phx::SpriteClipDef));
    const size_t end = sizeof(sh) + v.clips.size() * sizeof(phx::SpriteClipDef);
    if (end + 8 <= a.data.size()) {
        uint32_t hdr[2];
        std::memcpy(hdr, a.data.data() + end, sizeof(hdr));
        if (hdr[0] == phx::kSpriteTransMagic && end + 8 + size_t(hdr[1]) * sizeof(phx::SpriteTransDef) <= a.data.size()) {
            v.trans.resize(hdr[1]);
            if (hdr[1]) std::memcpy(v.trans.data(), a.data.data() + end + 8, v.trans.size() * sizeof(phx::SpriteTransDef));
        }
    }
    return v.frame_w > 0 && v.frame_h > 0;
}

// A Font asset: the glyph table (bundle.h FontBlobHeader + FontGlyphDef) of its atlas texture.
struct FontInfo {
    phx::FontBlobHeader hdr{};
    std::vector<phx::FontGlyphDef> glyphs;
};
inline bool view_font(const AssetEntry& a, FontInfo& v) {
    if (a.type != phx::AssetType::Font || a.data.size() < sizeof(phx::FontBlobHeader)) return false;
    v = FontInfo{};
    std::memcpy(&v.hdr, a.data.data(), sizeof(v.hdr));
    if (sizeof(v.hdr) + size_t(v.hdr.glyph_count) * sizeof(phx::FontGlyphDef) > a.data.size()) return false;
    v.glyphs.resize(v.hdr.glyph_count);
    if (v.hdr.glyph_count)
        std::memcpy(v.glyphs.data(), a.data.data() + sizeof(v.hdr), v.glyphs.size() * sizeof(phx::FontGlyphDef));
    return true;
}
// The pen width of `s` in font `f` (UI::text_width's rule: characters past the table use the header advance).
inline int font_text_width(const FontInfo& f, const std::string& s) {
    int w = 0;
    for (unsigned char c : s) {
        const int i = int(c) - int(f.hdr.first_char);
        w += i >= 0 && i < int(f.glyphs.size()) ? f.glyphs[size_t(i)].advance : f.hdr.advance;
    }
    return w;
}

// A Dialogue asset's tables (bundle.h DialogueHeader ...), bounds-checked like the runtime's load.
struct DialogueInfo {
    phx::DialogueHeader hdr{};
    std::vector<phx::DlgConvDef> convs;
    std::vector<phx::DlgNodeDef> nodes;
    std::vector<phx::DlgChoiceDef> choices;
    std::string strings;
    const char* str(uint32_t off) const { return off < strings.size() ? strings.c_str() + off : ""; }
};
inline bool view_dialogue(const AssetEntry& a, DialogueInfo& v) {
    if (a.type != phx::AssetType::Dialogue || a.data.size() < sizeof(phx::DialogueHeader)) return false;
    v = DialogueInfo{};
    std::memcpy(&v.hdr, a.data.data(), sizeof(v.hdr));
    if (v.hdr.magic != phx::kDialogueMagic) return false;
    size_t at = sizeof(v.hdr);
    const size_t need = at + v.hdr.conv_count * sizeof(phx::DlgConvDef) + v.hdr.speaker_count * sizeof(phx::DlgSpeakerDef) +
                        v.hdr.node_count * sizeof(phx::DlgNodeDef) + v.hdr.choice_count * sizeof(phx::DlgChoiceDef) +
                        v.hdr.op_count * sizeof(phx::DlgOp) + v.hdr.strings_size;
    if (need > a.data.size()) return false;
    auto take = [&](auto& vec, size_t n) {
        vec.resize(n);
        if (n) std::memcpy(vec.data(), a.data.data() + at, n * sizeof(vec[0]));
        at += n * sizeof(vec[0]);
    };
    take(v.convs, v.hdr.conv_count);
    at += v.hdr.speaker_count * sizeof(phx::DlgSpeakerDef);
    take(v.nodes, v.hdr.node_count);
    take(v.choices, v.hdr.choice_count);
    at += v.hdr.op_count * sizeof(phx::DlgOp);
    v.strings.assign(reinterpret_cast<const char*>(a.data.data() + at), v.hdr.strings_size);
    return true;
}

struct SoundInfo {
    const int16_t* samples = nullptr;
    uint32_t frames = 0, rate = 0;
};

inline bool view_sound(const AssetEntry& a, SoundInfo& v) {
    if (a.type != phx::AssetType::Sound || a.data.size() < sizeof(phx::SoundBlobHeader)) return false;
    phx::SoundBlobHeader sh{};
    std::memcpy(&sh, a.data.data(), sizeof(sh));
    if (sizeof(sh) + size_t(sh.frames) * 2 > a.data.size()) return false;
    v.samples = reinterpret_cast<const int16_t*>(a.data.data() + sizeof(sh));
    v.frames = sh.frames; v.rate = sh.rate;
    return true;
}

inline bool view_spawns(const AssetEntry& a, std::vector<phx::SpawnDef>& out) {
    out.clear();
    if (a.type != phx::AssetType::Spawns || a.data.size() < sizeof(phx::SpawnBlobHeader)) return false;
    phx::SpawnBlobHeader sh{};
    std::memcpy(&sh, a.data.data(), sizeof(sh));
    if (sizeof(sh) + size_t(sh.count) * sizeof(phx::SpawnDef) > a.data.size()) return false;
    out.resize(sh.count);
    if (sh.count) std::memcpy(out.data(), a.data.data() + sizeof(sh), out.size() * sizeof(phx::SpawnDef));
    return true;
}

// One-line human description of an asset ("64x32 PAL4_TILES", "320x20 x4 layers", "1.20 s").
inline std::string describe(const AssetEntry& a) {
    char b[96];
    switch (a.type) {
    case phx::AssetType::Dialogue: {
        DialogueInfo d;
        if (!view_dialogue(a, d)) return "malformed dialogue";
        std::snprintf(b, sizeof(b), "%u conversation%s, %u lines, %u choices", unsigned(d.convs.size()),
                      d.convs.size() == 1 ? "" : "s", unsigned(d.nodes.size()), unsigned(d.choices.size()));
        return b;
    }
    case phx::AssetType::Font: {
        FontInfo f;
        if (!view_font(a, f)) return "malformed font";
        std::snprintf(b, sizeof(b), "%u glyphs from '%c', line %u, %s", unsigned(f.glyphs.size()),
                      f.hdr.first_char >= 32 && f.hdr.first_char < 127 ? char(f.hdr.first_char) : '?',
                      unsigned(f.hdr.line_h), (f.hdr.flags & phx::kFontProportional) ? "proportional" : "fixed");
        return b;
    }
    case phx::AssetType::Texture: {
        TexView v;
        if (!view_texture(a, v)) return "malformed texture";
        std::snprintf(b, sizeof(b), "%ux%u %s", unsigned(v.w), unsigned(v.h), format_name(v.fmt));
        return b;
    }
    case phx::AssetType::Tilemap: {
        MapView v;
        if (!view_tilemap(a, v)) return "malformed tilemap";
        std::snprintf(b, sizeof(b), "%ux%u tiles x%u layer%s", unsigned(v.w), unsigned(v.h),
                      unsigned(v.layers), v.layers == 1 ? "" : "s");
        return b;
    }
    case phx::AssetType::Sprite: {
        SpriteInfo v;
        if (!view_sprite(a, v)) return "malformed sprite";
        std::snprintf(b, sizeof(b), "%ux%u frames, %u clip%s", unsigned(v.frame_w), unsigned(v.frame_h),
                      unsigned(v.clips.size()), v.clips.size() == 1 ? "" : "s");
        std::string d = b;
        if (!v.trans.empty()) d += ", " + std::to_string(v.trans.size()) + " transition" + (v.trans.size() == 1 ? "" : "s");
        return d;
    }
    case phx::AssetType::Sound: {
        SoundInfo v;
        if (!view_sound(a, v) || v.rate == 0) return "malformed sound";
        std::snprintf(b, sizeof(b), "%.2f s @ %u Hz", double(v.frames) / double(v.rate), unsigned(v.rate));
        return b;
    }
    case phx::AssetType::Spawns: {
        std::vector<phx::SpawnDef> s;
        if (!view_spawns(a, s)) return "malformed spawns";
        std::snprintf(b, sizeof(b), "%zu spawn point%s", s.size(), s.size() == 1 ? "" : "s");
        return b;
    }
    case phx::AssetType::Blob:
        return human_bytes(a.usize);
    }
    return "?";
}

// ============================================================================================
// TierReport — the same texture as every render tier would receive it
// ============================================================================================

struct TierReport {
    uint16_t w = 0, h = 0;
    uint32_t rgba_bytes = 0;                // tier 2 (PC): RGBA8 as-is
    bool     swz_ok = false;                // tier 1 (PSP): swizzle-able (w%4, h%8)
    std::vector<uint8_t> swz;               //   the RGBA8_SWZ payload
    bool     pal4_ok = false;               // tier 0 (GBA): 4bpp paletted tiles
    std::vector<uint8_t> pal4;              //   the PAL4_TILES payload (bake-identical)
    uint16_t pal4_palettes = 0;
    int      tiles_x = 0, tiles_y = 0;      // 8x8 tile grid (0 when not 8px-aligned)
    std::vector<uint8_t> tile_colors;       // unique opaque BGR555 colours per tile
    int      tiles_over = 0;                // tiles needing > 15 colours (the GBA can't)
    const char* pal4_reason = "";           // why tier 0 falls back to RGBA8, when it does
};

inline TierReport analyse_tiers(const std::vector<uint32_t>& rgba, uint16_t w, uint16_t h) {
    TierReport r;
    r.w = w; r.h = h;
    r.rgba_bytes = uint32_t(w) * h * 4;
    if (rgba.size() < size_t(w) * h) return r;
    r.swz_ok = phxtool::swz_encode(rgba.data(), w, h, r.swz);
    if (w % 8 || h % 8) {
        r.pal4_reason = "not 8px-aligned - GBA keeps RGBA8";
        return r;
    }
    r.tiles_x = w / 8; r.tiles_y = h / 8;
    r.tile_colors.assign(size_t(r.tiles_x) * r.tiles_y, 0);
    for (int ty = 0; ty < r.tiles_y; ++ty)
        for (int tx = 0; tx < r.tiles_x; ++tx) {
            uint16_t seen[64];
            int n = 0;
            for (int y = 0; y < 8; ++y)
                for (int x = 0; x < 8; ++x) {
                    const uint32_t c = rgba[size_t(ty * 8 + y) * w + size_t(tx * 8 + x)];
                    if ((c >> 24) == 0) continue;
                    const uint16_t b = phx::rgba8_to_bgr555(c);
                    bool dup = false;
                    for (int k = 0; k < n; ++k) if (seen[k] == b) { dup = true; break; }
                    if (!dup) seen[n++] = b;
                }
            r.tile_colors[size_t(ty * r.tiles_x + tx)] = uint8_t(n);
            if (n > 15) ++r.tiles_over;
        }
    r.pal4_ok = phxtool::pal4_encode(rgba.data(), w, h, r.pal4);
    if (r.pal4_ok) {
        phx::TexturePal4Header ph{};
        std::memcpy(&ph, r.pal4.data(), sizeof(ph));
        r.pal4_palettes = ph.pal_count;
    } else {
        r.pal4_reason = r.tiles_over ? "a tile needs >15 colours - GBA keeps RGBA8"
                                     : "palettes won't deduplicate into 255 - GBA keeps RGBA8";
    }
    return r;
}

// ============================================================================================
// Makefile + launch catalog
// ============================================================================================

// True when the Makefile defines `target:` at the start of a line.
inline bool make_has_target(const std::string& mk, const std::string& target) {
    const std::string needle = target + ":";
    size_t p = 0;
    while ((p = mk.find(needle, p)) != std::string::npos) {
        const bool bol = p == 0 || mk[p - 1] == '\n';
        const bool not_assign = p + needle.size() >= mk.size() || mk[p + needle.size()] != '=';
        if (bol && not_assign) return true;
        p += needle.size();
    }
    return false;
}

// The prerequisites listed on `target:`'s rule line (backslash continuations joined).
inline std::vector<std::string> make_prereqs(const std::string& mk, const std::string& target) {
    std::vector<std::string> out;
    const std::string needle = target + ":";
    size_t p = 0;
    while ((p = mk.find(needle, p)) != std::string::npos) {
        if (p == 0 || mk[p - 1] == '\n') break;
        p += needle.size();
    }
    if (p == std::string::npos) return out;
    p += needle.size();
    std::string deps;
    while (p < mk.size()) {
        size_t e = mk.find('\n', p);
        if (e == std::string::npos) e = mk.size();
        std::string line = mk.substr(p, e - p);
        if (!line.empty() && line.back() == '\r') line.pop_back();   // a CRLF checkout (Windows)
        const bool cont = !line.empty() && line.back() == '\\';
        if (cont) line.pop_back();
        deps += ' ' + line;
        p = e + 1;
        if (!cont) break;
    }
    const size_t bar = deps.find('|');                   // drop order-only prerequisites
    if (bar != std::string::npos) deps.erase(bar);
    size_t i = 0;
    while (i < deps.size()) {
        while (i < deps.size() && (deps[i] == ' ' || deps[i] == '\t')) ++i;
        size_t j = i;
        while (j < deps.size() && deps[j] != ' ' && deps[j] != '\t') ++j;
        if (j > i) out.push_back(deps.substr(i, j - i));
        i = j;
    }
    return out;
}

enum class Group : uint8_t { Play, Build, Test, Edit, Gate, Suite, Cross, Count };   // Build/Test: project launches

inline const char* group_name(Group g) {
    switch (g) {
    case Group::Play:  return "PLAY";
    case Group::Build: return "BUILD";
    case Group::Test:  return "TEST";
    case Group::Edit:  return "EDIT";
    case Group::Gate:  return "GATES";
    case Group::Suite: return "TEST SUITES";
    case Group::Cross: return "CONSOLE BUILDS";
    default:           return "";
    }
}

struct Launch {
    std::string label;                  // shown on the card/chip
    Group       group = Group::Suite;
    std::string make_target;            // must exist in the Makefile (validated)
    std::string command;                // shell command, run from the repository root
    std::string blurb;                  // one line for the status bar
    std::vector<std::string> needs;     // executables (PATH) or absolute paths required
    bool        windowed = false;       // opens its own window (a game / an editor)
};

// The catalog. Suites come from the Makefile's `check:` prerequisites (every gate suite), so a
// new suite shows up in the studio the moment it joins the gate.
inline std::vector<Launch> default_launches(const std::string& mk, const std::string& devkitarm) {
    std::vector<Launch> v;
    const std::string gba_cxx = devkitarm + "/bin/arm-none-eabi-g++";
    auto add = [&](const char* label, Group g, const char* target, std::string cmd, const char* blurb,
                   std::vector<std::string> needs, bool windowed) {
        if (!make_has_target(mk, target)) return;        // tolerate a trimmed Makefile
        Launch l;
        l.label = label; l.group = g; l.make_target = target; l.command = std::move(cmd);
        l.blurb = blurb; l.needs = std::move(needs); l.windowed = windowed;
        v.push_back(std::move(l));
    };
    // games (each builds, then opens its own window)
    add("Emberwing", Group::Play, "emberwing-sdl", "make emberwing-sdl && ./build/emberwing_sdl",
        "The engine's showcase platformer: software renderer + real audio device", { "sdl2-config" }, true);
    add("Emberwing GL", Group::Play, "emberwing-gl", "make emberwing-gl && ./build/emberwing_gl",
        "The same game through the OpenGL backend", { "sdl2-config" }, true);
    add("Platformer", Group::Play, "sdl", "make sdl && ./build/platformer_sdl",
        "The reference platformer (the MVP gate), software renderer", { "sdl2-config" }, true);
    add("Platformer GL", Group::Play, "gl", "make gl && ./build/platformer_gl",
        "The reference platformer through the OpenGL backend", { "sdl2-config" }, true);
    add("tinyLLM", Group::Play, "tinyllm-sdl",
        "make tinyllm-sdl && TINYLLM_MODEL=build/tinyllm.phxllm ./build/tinyllm_sdl",
        "A quantized language model on the engine's BG-tilemap text console", { "sdl2-config" }, true);
    add("Miracle", Group::Play, "miracle", "make miracle && ./build/miracle",
        "The music visualizer (synthetic tone without the song file)", { "sdl2-config" }, true);
    // editors
    add("Map editor", Group::Edit, "tmap",
        "make tmap && if [ -f build/studio_level.tmj ]; then ./build/phxtmap build/studio_level.tmj; "
        "else ./build/phxtmap --out build/studio_level.tmj --size 40x20; fi",
        "phxtmap on build/studio_level.tmj (Enter saves)", { "sdl2-config" }, true);
    add("Entity editor", Group::Edit, "entity",
        "make entity && if [ -f build/studio_enemies.json ]; then ./build/phxentity build/studio_enemies.json; "
        "else ./build/phxentity --new Enemy --fields hp:u16,atk:i8,speed:u8 --out build/studio_enemies.json; fi",
        "phxentity on build/studio_enemies.json (Enter saves)", { "sdl2-config" }, true);
    // gates
    add("make check", Group::Gate, "check", "make check",
        "THE gate: every unit + integration suite, the asset pipeline and depcheck", {}, false);
    add("determinism", Group::Gate, "determinism", "make determinism",
        "Both scalar tiers (float / fixed16) must produce byte-identical results + frame", {}, false);
    add("sanitize", Group::Gate, "sanitize", "make sanitize",
        "The whole check suite under AddressSanitizer + UBSan (slow)", {}, false);
    add("release", Group::Gate, "release", "make release",
        "The check suite with PHX_BUILD_RELEASE=1 (asserts compiled out)", {}, false);
    add("depcheck", Group::Gate, "depcheck", "make depcheck",
        "The acyclic, strictly-layered module dependency law", { "python3" }, false);
    add("golden frame", Group::Gate, "render", "make render",
        "Render the software golden-reference frame to build/render_out.ppm", {}, false);
    // console / cross builds
    add("GBA Emberwing", Group::Cross, "gba-emberwing-ppu",
        "make gba-emberwing-ppu && (command -v mgba-qt >/dev/null && mgba-qt build/gba/phx-emberwing-ppu.gba "
        "|| echo 'ROM built: build/gba/phx-emberwing-ppu.gba (install mgba-qt to auto-launch)')",
        "devkitARM -> the shipping GBA ROM (native PPU), opened in mGBA", { gba_cxx }, true);
    add("GBA Platformer", Group::Cross, "gba-platformer-ppu",
        "make gba-platformer-ppu && (command -v mgba-qt >/dev/null && mgba-qt build/gba/phx-platformer-ppu.gba "
        "|| echo 'ROM built: build/gba/phx-platformer-ppu.gba')",
        "devkitARM -> the platformer on GBA PPU hardware, opened in mGBA", { gba_cxx }, true);
    add("GBA size gate", Group::Cross, "size-gate", "make size-gate",
        "GBA ROM / IWRAM / EWRAM budgets (a CI gate)", { gba_cxx, "python3" }, false);
    add("PSP Emberwing", Group::Cross, "psp-emberwing", "make psp-emberwing",
        "pspsdk -> build/psp/emberwing/EBOOT.PBP (run it in PPSSPP)", { "psp-g++" }, false);
    add("PSP GU smoke", Group::Cross, "psp-gu", "make psp-gu",
        "pspsdk -> the sceGu hardware-backend smoke EBOOT", { "psp-g++" }, false);
    add("Windows .exe", Group::Cross, "win", "make win",
        "MinGW-w64 -> every host binary as a static PE32+ .exe", { "x86_64-w64-mingw32-g++" }, false);
    // suites: exactly what `make check` runs
    for (const std::string& s : make_prereqs(mk, "check")) {
        Launch l;
        l.label = s; l.group = Group::Suite; l.make_target = s; l.command = "make " + s;
        l.blurb = "make " + s + " - one suite of the check gate";
        if (s == "depcheck") l.needs = { "python3" };
        v.push_back(std::move(l));
    }
    return v;
}

// ---- tools on PATH, and the shell the launches run in ----------------------------------------
// Launch commands are POSIX shell. On Linux/macOS that is /bin/sh; on Windows it is the sh.exe of
// MSYS2 or Git for Windows (never cmd.exe), found by find_posix_shell below.

// A PATH-style list (':' on POSIX, ';' on Windows) -> its non-empty entries.
inline std::vector<std::string> split_path_list(const std::string& s, char sep) {
    std::vector<std::string> v;
    size_t i = 0;
    while (i <= s.size()) {
        size_t j = s.find(sep, i);
        if (j == std::string::npos) j = s.size();
        if (j > i) v.push_back(s.substr(i, j - i));
        i = j + 1;
    }
    return v;
}

inline bool is_file_at(const std::string& p) {
    std::error_code ec;
    return fs::exists(p, ec) && !fs::is_directory(p, ec);
}

// Where `tool` resolves on disk, or "". A bare name is looked up in `dirs`. A name with a '/' is a
// path and is checked as is. With `windows`, a tool may also carry .exe/.cmd/.bat (shell scripts
// such as sdl2-config have none, so the bare name is tried first), and a POSIX-absolute path
// ("/opt/devkitpro/...") is also looked for under `posix_root`, the folder the shell calls "/".
inline std::string resolve_tool(const std::string& tool, const std::vector<std::string>& dirs, bool windows,
                                const std::string& posix_root = "") {
    static const char* const kWinExts[] = { "", ".exe", ".cmd", ".bat" };
    const size_t n_ext = windows ? 4 : 1;
    auto try_exts = [&](const std::string& base) -> std::string {
        for (size_t e = 0; e < n_ext; ++e)
            if (is_file_at(base + kWinExts[e])) return base + kWinExts[e];
        return "";
    };
    if (tool.empty()) return "";
    if (tool.find('/') != std::string::npos) {
        std::string hit = try_exts(tool);
        if (hit.empty() && windows && !posix_root.empty() && tool[0] == '/' && tool.compare(0, 2, "//") != 0)
            hit = try_exts(posix_root + tool);
        return hit;
    }
    for (const std::string& d : dirs) {
        std::string hit = try_exts((fs::path(d) / tool).generic_string());
        if (!hit.empty()) return hit;
    }
    return "";
}

// The POSIX shell for Windows launches, and the folders to put in front of PATH so the shell finds
// make and the compiler even when the Studio was started from Explorer rather than a terminal.
struct PosixShell {
    std::string sh;                         // sh.exe ("" = none found)
    std::string root;                       // the folder the shell calls "/" (the MSYS2 or Git install)
    std::vector<std::string> add_path;      // the toolchain's bin and the shell's bin, when not on PATH
};

// Search order: `override_sh` (PHX_SH), then the sh.exe beside the first make.exe on PATH (so the
// shell and make share one MSYS runtime), then sh.exe on PATH, then the usual install folders
// (`fallbacks`). `msystem` (MSYS2's MSYSTEM, e.g. UCRT64) picks the toolchain folder; without it
// the first of ucrt64, mingw64, clang64 that exists is used.
inline PosixShell find_posix_shell(const std::vector<std::string>& path, const std::string& override_sh,
                                   const std::string& msystem, const std::vector<std::string>& fallbacks) {
    auto norm = [](std::string s) {
        for (char& c : s) {
            if (c == '\\') c = '/';
            else if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
        }
        while (s.size() > 1 && s.back() == '/') s.pop_back();
        return s;
    };
    auto slashes = [](std::string s) { for (char& c : s) if (c == '\\') c = '/'; return s; };
    auto ends_with = [](const std::string& s, const char* t) {
        const size_t n = std::strlen(t);
        return s.size() >= n && s.compare(s.size() - n, n, t) == 0;
    };
    PosixShell r;
    if (!override_sh.empty() && is_file_at(override_sh)) r.sh = slashes(override_sh);
    if (r.sh.empty()) {
        const std::string make = resolve_tool("make", path, true);
        if (!make.empty()) {
            const std::string beside = slashes(fs::path(make).parent_path().generic_string()) + "/sh.exe";
            if (is_file_at(beside)) r.sh = beside;
        }
    }
    if (r.sh.empty()) r.sh = slashes(resolve_tool("sh", path, true));
    for (size_t i = 0; r.sh.empty() && i < fallbacks.size(); ++i)
        if (is_file_at(fallbacks[i])) r.sh = slashes(fallbacks[i]);
    if (r.sh.empty()) return r;

    const std::string bin = r.sh.substr(0, r.sh.find_last_of('/'));
    const std::string lbin = norm(bin);
    if (ends_with(lbin, "/usr/bin")) r.root = bin.substr(0, bin.size() - 8);
    else if (ends_with(lbin, "/bin")) r.root = bin.substr(0, bin.size() - 4);

    std::vector<std::string> want;
    if (!r.root.empty()) {
        std::vector<std::string> envs;
        if (!msystem.empty()) envs.push_back(norm(msystem));
        else envs = { "ucrt64", "mingw64", "clang64" };
        for (const std::string& e : envs) {
            std::error_code ec;
            const std::string tc = r.root + "/" + e + "/bin";
            if (fs::is_directory(tc, ec)) { want.push_back(tc); break; }
        }
    }
    want.push_back(bin);
    for (const std::string& w : want) {
        bool on_path = false;
        for (const std::string& p : path) on_path = on_path || norm(p) == norm(w);
        if (!on_path) r.add_path.push_back(w);
    }
    return r;
}

#ifdef _WIN32
// This process's shell, found once. The first call also puts its folders in front of PATH, for
// the Studio's own tool checks and for every child it starts (they inherit the environment).
inline const PosixShell& host_posix_shell() {
    static const PosixShell shell = [] {
        auto env = [](const char* k) { const char* v = std::getenv(k); return std::string(v ? v : ""); };
        const std::string path = env("PATH");
        std::vector<std::string> fallbacks = { "C:/msys64/usr/bin/sh.exe" };
        for (const char* base : { "ProgramFiles", "ProgramW6432" })
            if (!env(base).empty()) {
                fallbacks.push_back(env(base) + "/Git/usr/bin/sh.exe");
                fallbacks.push_back(env(base) + "/Git/bin/sh.exe");
            }
        if (!env("LOCALAPPDATA").empty()) fallbacks.push_back(env("LOCALAPPDATA") + "/Programs/Git/usr/bin/sh.exe");
        PosixShell s = find_posix_shell(split_path_list(path, ';'), env("PHX_SH"), env("MSYSTEM"), fallbacks);
        if (!s.add_path.empty()) {
            std::string np = "PATH=";
            for (std::string d : s.add_path) {
                for (char& c : d) if (c == '/') c = '\\';
                np += d + ";";
            }
            _putenv((np + path).c_str());
        }
        return s;
    }();
    return shell;
}
#endif

// Is `tool` available? Absolute/relative paths are checked directly; bare names are looked up
// on PATH (the studio inherits the environment it was launched from; on Windows, extended by
// host_posix_shell so the answer matches what a launch will see).
inline bool tool_available(const std::string& tool) {
#ifdef _WIN32
    const PosixShell& sh = host_posix_shell();
    const char* path = std::getenv("PATH");
    return !resolve_tool(tool, split_path_list(path ? path : "", ';'), true, sh.root).empty();
#else
    const char* path = std::getenv("PATH");
    return !resolve_tool(tool, split_path_list(path ? path : "", ':'), false).empty();
#endif
}

// A launch need with its $VAR / ${VAR} references filled in by `env` (a lookup returning "" when
// unset). The console SDK variables default the way the Makefile defaults them: DEVKITPRO to
// /opt/devkitpro and DEVKITARM to $DEVKITPRO/devkitARM. Another unset variable stays as written.
template <class Env>
std::string expand_need_vars(const std::string& need, const Env& env) {
    auto value = [&](const std::string& k) -> std::string {
        std::string v = env(k);
        if (!v.empty()) return v;
        if (k == "DEVKITPRO") return "/opt/devkitpro";
        if (k == "DEVKITARM") {
            const std::string dkp = env("DEVKITPRO");
            return (dkp.empty() ? std::string("/opt/devkitpro") : dkp) + "/devkitARM";
        }
        return "$" + k;
    };
    auto ident = [](char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; };
    std::string o;
    for (size_t i = 0; i < need.size();) {
        if (need[i] != '$') { o += need[i++]; continue; }
        size_t b = i + 1, e;
        const bool braced = b < need.size() && need[b] == '{';
        if (braced) {
            e = need.find('}', b + 1);
            if (e == std::string::npos) { o += need.substr(i); break; }
            o += value(need.substr(b + 1, e - b - 1));
            i = e + 1;
        } else {
            e = b;
            while (e < need.size() && ident(need[e])) ++e;
            if (e == b) { o += need[i++]; continue; }
            o += value(need.substr(b, e - b));
            i = e;
        }
    }
    return o;
}

// The first missing requirement of a launch (with its variables filled in), or "" when it can run.
inline std::string missing_need(const Launch& l) {
    auto env = [](const std::string& k) { const char* v = std::getenv(k.c_str()); return std::string(v ? v : ""); };
    for (const std::string& n : l.needs) {
        const std::string need = expand_need_vars(n, env);
        if (!tool_available(need)) return need;
    }
    return "";
}

// A POSIX process wait status (pclose) -> an exit code; signals map to 128+N like a shell.
// (Windows jobs read the exit code directly; see winjob.h.)
inline int exit_code_from_status(int status) {
    if (status == -1) return -1;
    if ((status & 0x7f) == 0) return (status >> 8) & 0xff;
    return 128 + (status & 0x7f);
}

// ============================================================================================
// LogRing — process output as classified lines for the console panel
// ============================================================================================

enum class Tone : uint8_t { Plain, Dim, Good, Bad, Warn, Info, Cmd };

// Colour a line of build/test output: harness verdicts, compiler diagnostics, make errors.
inline Tone classify_line(const std::string& line) {
    const std::string t = trim(line);
    if (t.empty()) return Tone::Plain;
    if (starts_with(t, "$ ")) return Tone::Cmd;
    const std::string l = lower(t);
    const bool fail = l.find("fail") != std::string::npos && l.find(" 0 fail") == std::string::npos &&
                      l.find("0 failed") == std::string::npos;
    if (fail || l.find("error") != std::string::npos || starts_with(l, "make: ***") ||
        l.find("violation") != std::string::npos || l.find("abort") != std::string::npos)
        return Tone::Bad;
    if (l.find("warning") != std::string::npos) return Tone::Warn;
    if (starts_with(t, "PASS") || t.find(" PASS") != std::string::npos || l.find(": ok") != std::string::npos ||
        starts_with(t, "OK") || t.find(" OK ") != std::string::npos)
        return Tone::Good;
    if (starts_with(l, "g++") || starts_with(l, "c++") || starts_with(l, "clang") ||
        starts_with(l, "arm-none-eabi") || starts_with(l, "psp-") || l.find("/bin/arm-none-eabi") != std::string::npos ||
        starts_with(l, "x86_64-w64-mingw32"))
        return Tone::Dim;
    if (starts_with(l, "built ") || starts_with(l, "make[") || starts_with(l, "note:")) return Tone::Info;
    return Tone::Plain;
}

struct LogLine {
    std::string text;
    Tone tone = Tone::Plain;
};

class LogRing {
public:
    explicit LogRing(size_t cap = 5000) : cap_(cap ? cap : 1) {}

    // Feed raw bytes: splits on '\n', a bare '\r' restarts the line (progress meters), tabs
    // become spaces and non-ASCII bytes become '?' (the bitmap font is 7-bit).
    void feed(const char* data, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            const unsigned char c = static_cast<unsigned char>(data[i]);
            if (c == '\n')      { push(partial_); partial_.clear(); }
            else if (c == '\r') { if (i + 1 < n && data[i + 1] == '\n') continue; partial_.clear(); }
            else if (c == '\t') partial_ += "    ";
            else if (c == 0x1b) esc_ = true;                     // ANSI colour: skip to the 'm'
            else if (esc_)      { if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) esc_ = false; }
            else if (c < 0x20)  continue;
            else if (c >= 0x80) { if ((c & 0xC0) != 0x80) partial_ += '?'; }   // one '?' per UTF-8 char
            else                partial_ += char(c);
        }
    }
    void flush() { if (!partial_.empty()) { push(partial_); partial_.clear(); } }

    void push(const std::string& text) { push(text, classify_line(text)); }
    void push(const std::string& text, Tone tone) {
        lines_.push_back(LogLine{ text, tone });
        ++total_;
        while (lines_.size() > cap_) lines_.pop_front();
    }
    void clear() { lines_.clear(); partial_.clear(); esc_ = false; }

    size_t size() const            { return lines_.size(); }
    const LogLine& at(size_t i) const { return lines_[i]; }
    uint64_t total() const         { return total_; }      // lines ever pushed (for auto-follow)

private:
    std::deque<LogLine> lines_;
    std::string partial_;
    size_t   cap_;
    uint64_t total_ = 0;
    bool     esc_ = false;
};

// ============================================================================================
// layout math (pure; the GUI shell only draws what these compute)
// ============================================================================================

// The Rect / fit / scroll math is the widget kit's (tools/common/twk_geom.h), so the model and the
// GUI agree on one geometry type.
using twk::Rect;
using twk::fit_rect;
using twk::clamp_scroll;
using twk::scroll_thumb;
using twk::scroll_from_track;

// Waveform columns: per-column min/max sample (for a peak display `cols` pixels wide).
inline void waveform(const int16_t* s, uint32_t frames, int cols,
                     std::vector<int16_t>& mn, std::vector<int16_t>& mx) {
    mn.assign(size_t(std::max(0, cols)), 0);
    mx.assign(size_t(std::max(0, cols)), 0);
    if (!s || frames == 0 || cols <= 0) return;
    for (int c = 0; c < cols; ++c) {
        const uint64_t a = uint64_t(frames) * uint64_t(c) / uint64_t(cols);
        uint64_t b = uint64_t(frames) * uint64_t(c + 1) / uint64_t(cols);
        if (b <= a) b = a + 1;
        int16_t lo = s[a], hi = s[a];
        for (uint64_t i = a; i < b && i < frames; ++i) { lo = std::min(lo, s[i]); hi = std::max(hi, s[i]); }
        mn[size_t(c)] = lo; mx[size_t(c)] = hi;
    }
}

// Find the repository root: `start` or the nearest ancestor holding Makefile + engine/.
inline std::string find_repo_root(const std::string& start) {
    std::error_code ec;
    fs::path p = fs::weakly_canonical(fs::absolute(start, ec), ec);
    for (int i = 0; i < 16 && !p.empty(); ++i) {
        if (fs::exists(p / "Makefile", ec) && fs::is_directory(p / "engine", ec) &&
            fs::exists(p / "tools" / "common" / "depcheck.py", ec))
            return p.string();
        const fs::path up = p.parent_path();
        if (up == p) break;
        p = up;
    }
    return "";
}

// Every *.phxp in the root and build/, root first then by name.
inline std::vector<std::string> find_bundles(const std::string& root) {
    std::vector<std::string> out;
    std::error_code ec;
    for (const fs::path& dir : { fs::path(root), fs::path(root) / "build" }) {
        std::vector<std::string> here;
        for (const auto& e : fs::directory_iterator(dir, ec))
            if (e.is_regular_file(ec) && e.path().extension() == ".phxp")
                here.push_back(fs::relative(e.path(), root, ec).generic_string());
        std::sort(here.begin(), here.end());
        out.insert(out.end(), here.begin(), here.end());
    }
    return out;
}

} // namespace phxstudio
#endif // PHX_TOOLS_PHXSTUDIO_MODEL_H
