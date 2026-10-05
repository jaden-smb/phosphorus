// tools/phxtmap/main.cpp — the standalone TILEMAP editor: Phosphorus Studio's map panel
// (tools/phxstudio/ed_map.cpp) in a window of its own. It edits the open Tiled `.tmj` author
// format (never engine blobs — phxtile/phxpack bake what it saves, docs/08 §1) and dogfoods the
// engine: the same App loop, SDL window, software renderer and widget kit as the Studio and the
// games. The document model (editor.h: TmapDoc) is unit-tested headlessly.
//
//   phxtmap [--out FILE.tmj] [--size WxH] [--tile WxH] [--tileset PNG] [--types a,b,c]
//           [--prefabs TABLE.json] [--scale N] [FILE.tmj]
//
// Keys (the same as the Studio's map editor): B brush, E eraser, G fill, R rectangle, I picker,
// S select, T spawns, H hand; Tab next layer; V cycle the brush tile's collision; Ctrl+S save;
// Ctrl+Z / Ctrl+Y undo / redo; Ctrl+wheel zoom; middle-drag pan. See instructions.md.
#include "../phxstudio/solo.h"
#include "editor.h"                        // TmapDoc
#include "../phxentity/editor.h"           // BinDoc — the shared prefab-schema table (--prefabs)

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace phxstudio;

namespace {

std::vector<std::string> split_csv(const std::string& csv) {
    std::vector<std::string> out;
    size_t p = 0;
    while (p <= csv.size()) {
        size_t c = csv.find(',', p);
        if (c == std::string::npos) c = csv.size();
        if (c > p) out.emplace_back(csv.substr(p, c - p));
        p = c + 1;
    }
    return out;
}

bool read_all(const std::string& path, std::string& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    std::fclose(f);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    SoloShell shell;
    shell.app_name = "phxtmap";
    shell.help = "B brush  E eraser  G fill  R rect  I pick  S select  T spawns  Tab layer  V collision  Ctrl+S save";
    int bw = 24, bh = 18, tw = 8, th = 8, scale = 2;
    std::string in_path, out_path, prefabs_path, tileset_png;
    std::vector<std::string> types;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--out" && i + 1 < argc)            out_path = argv[++i];
        else if (a == "--size" && i + 1 < argc)      std::sscanf(argv[++i], "%dx%d", &bw, &bh);
        else if (a == "--tile" && i + 1 < argc)      std::sscanf(argv[++i], "%dx%d", &tw, &th);
        else if (a == "--tileset" && i + 1 < argc)   tileset_png = argv[++i];
        else if (a == "--types" && i + 1 < argc)     types = split_csv(argv[++i]);
        else if (a == "--prefabs" && i + 1 < argc)   prefabs_path = argv[++i];
        else if (a == "--shot" && i + 1 < argc)      shell.shot_path = argv[++i];
        else if (a == "--scale" && i + 1 < argc)     scale = std::atoi(argv[++i]);
        else if (a == "--help" || a == "-h") {
            std::printf("usage: phxtmap [--out FILE.tmj] [--size WxH] [--tile WxH] [--tileset PNG]\n"
                        "               [--types a,b,c] [--prefabs TABLE.json] [--scale N] [FILE.tmj]\n"
                        "  FILE.tmj   the map to edit (saves in place unless --out)\n"
                        "  --out      where Ctrl+S writes (default: FILE.tmj, or level.tmj for a new map)\n"
                        "  --size     a NEW map's size in tiles (default 24x18); --tile its tile size (8x8)\n"
                        "  --tileset  a NEW map's tileset PNG (its file name is the texture the bake uses)\n"
                        "  --types    extra spawn types to place (the map's own + player/coin/enemy/spike\n"
                        "             are always offered)\n"
                        "  --prefabs  read the placeable types from a phxentity record table's string\n"
                        "             'type'/'name' column (the shared prefab schema)\n");
            return 0;
        } else in_path = argv[i];
    }

    if (!prefabs_path.empty()) {                 // the prefab table IS the vocabulary
        std::string text, err;
        phxtool::BinDoc prefabs;
        if (!read_all(prefabs_path, text) || !phxtool::BinDoc::load(text, prefabs, &err)) {
            std::fprintf(stderr, "phxtmap: cannot load prefab table '%s'%s%s\n", prefabs_path.c_str(),
                         err.empty() ? "" : ": ", err.c_str());
            return 1;
        }
        const std::vector<std::string> names = prefabs.name_column();
        if (names.empty()) {
            std::fprintf(stderr, "phxtmap: prefab table '%s' has no string 'type'/'name' column "
                                 "(or no named records)\n", prefabs_path.c_str());
            return 1;
        }
        types.insert(types.begin(), names.begin(), names.end());
        std::printf("phxtmap: %zu prefab types from %s ('%s')\n", names.size(), prefabs_path.c_str(),
                    prefabs.struct_name.c_str());
    }
    shell.types = types;

    shell.repo_root = solo_repo_root(".");
    if (shell.repo_root.empty()) shell.repo_root = abs_path(".");

    if (!in_path.empty()) {
        shell.save_as = out_path;
        shell.factory = [in_path](Host& h, std::string* err) { return make_map_view(h, abs_path(in_path), err); };
    } else {
        const std::string out = abs_path(out_path.empty() ? "level.tmj" : out_path);
        phxtool::TmapDoc d = phxtool::TmapDoc::blank(bw, bh, tw, th, "tiles");
        if (!tileset_png.empty()) {
            d.tileset = stem_of(tileset_png);
            std::error_code ec;
            d.tileset_image = pfs::relative(abs_path(tileset_png), pfs::path(out).parent_path(), ec).generic_string();
            if (ec || d.tileset_image.empty()) d.tileset_image = abs_path(tileset_png);
        }
        shell.factory = [out, d](Host& h, std::string*) { return make_map_view_new(h, out, d); };
        std::printf("phxtmap: new %dx%d map -> %s (Ctrl+S saves)\n", bw, bh, out.c_str());
    }
    return run_solo(shell, 640, 360, scale);
}
