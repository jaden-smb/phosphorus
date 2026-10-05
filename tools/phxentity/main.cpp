// tools/phxentity/main.cpp — the standalone DATA TABLE editor: Phosphorus Studio's table panel
// (tools/phxstudio/ed_table.cpp) in a window of its own. It edits the phxbin author JSON (typed
// record tables: entity/prefab stats, items, tuning — never engine blobs; phxbin/phxpack bake what
// it saves, docs/08 §1) and dogfoods the engine. The document model (editor.h: BinDoc) is
// unit-tested headlessly.
//
//   phxentity [--out FILE.json] [--scale N] FILE.json
//   phxentity --new NAME --fields a:type,b:type [--out FILE.json]      start a fresh table
//
// Keys (the same as the Studio's table editor): arrows / Tab move, Enter or typing edits a cell,
// +/- step a number, Ctrl+D duplicates a record, right-click a header or row number for more,
// Ctrl+S save, Ctrl+Z / Ctrl+Y undo / redo. See instructions.md.
#include "../phxstudio/solo.h"
#include "editor.h"                        // BinDoc

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace phxstudio;

namespace {

std::vector<std::string> split_fields(const std::string& csv) {
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

} // namespace

int main(int argc, char** argv) {
    SoloShell shell;
    shell.app_name = "phxentity";
    shell.help = "arrows/Tab move  Enter/typing edit  +/- step  Ctrl+D duplicate  right-click: columns/rows  Ctrl+S save";
    std::string in_path, out_path, new_struct, new_fields;
    int scale = 2;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--out" && i + 1 < argc)         out_path = argv[++i];
        else if (a == "--new" && i + 1 < argc)    new_struct = argv[++i];
        else if (a == "--fields" && i + 1 < argc) new_fields = argv[++i];
        else if (a == "--shot" && i + 1 < argc)      shell.shot_path = argv[++i];
        else if (a == "--scale" && i + 1 < argc)  scale = std::atoi(argv[++i]);
        else if (a == "--help" || a == "-h") {
            std::printf("usage: phxentity [--out FILE.json] [--scale N] FILE.json\n"
                        "       phxentity --new NAME --fields a:type,b:type [--out FILE.json]\n"
                        "  --new     start a fresh record table named NAME (no input file needed)\n"
                        "  --fields  its schema; types: u8/i8/u16/i16/u32/i32/f32, str8/str16/str32\n"
                        "            (a str field named 'type' makes the table a prefab schema the\n"
                        "             map editor places from)\n"
                        "  --out     where Ctrl+S writes (default: FILE.json, or prefabs.json for --new)\n");
            return 0;
        } else in_path = argv[i];
    }
    shell.repo_root = solo_repo_root(".");
    if (shell.repo_root.empty()) shell.repo_root = abs_path(".");

    if (!new_struct.empty()) {
        phxtool::BinDoc d;
        std::string err;
        if (!phxtool::BinDoc::blank(new_struct, split_fields(new_fields), d, &err)) {
            std::fprintf(stderr, "phxentity: bad --new schema: %s\n", err.c_str());
            return 1;
        }
        d.add_record(0);                          // one zeroed row so the cursor has a home
        const std::string out = abs_path(out_path.empty() ? "prefabs.json" : out_path);
        std::printf("phxentity: new table '%s' (%zu fields) -> %s (Ctrl+S saves)\n",
                    new_struct.c_str(), d.fields.size(), out.c_str());
        shell.factory = [out, d](Host& h, std::string*) { return make_table_view_new(h, out, d); };
    } else {
        if (in_path.empty()) { std::fprintf(stderr, "phxentity: need an input .json or --new (see --help)\n"); return 1; }
        shell.save_as = out_path.empty() ? "" : abs_path(out_path);
        shell.factory = [in_path](Host& h, std::string* err) { return make_table_view(h, abs_path(in_path), err); };
    }
    return run_solo(shell, 640, 360, scale);
}
