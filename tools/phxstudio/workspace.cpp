// tools/phxstudio/workspace.cpp — the Studio's Editor workspace (see workspace.h).
#include "workspace.h"
#include "pixeldoc.h"
#include "../phxtmap/editor.h"
#include "../phxentity/editor.h"
#include "font.h"                  // tools/phxpack: New font
#include "synth.h"                 // tools/phxpack: New sound effect / New song
#include "dialogue.h"              // tools/phxpack: New dialogue
#include "ascii_font.h"            // tools/common: the new font's starting glyphs

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>

namespace phxstudio {

using namespace twk;

namespace {

bool write_text_file(const std::string& path, const std::string& text, std::string* err) {
    std::error_code ec;
    pfs::create_directories(pfs::path(path).parent_path(), ec);
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { if (err) *err = "cannot write " + path; return false; }
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    std::fclose(f);
    if (!ok && err) *err = "short write to " + path;
    return ok;
}

bool exists(const std::string& p) { std::error_code ec; return pfs::exists(p, ec); }

std::string config_dir() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    const char* home = std::getenv("HOME");
    if (xdg && *xdg) return std::string(xdg) + "/phxstudio";
    const char* appdata = std::getenv("APPDATA");        // Windows, started outside a Unix shell
    if (home && *home) return std::string(home) + "/.config/phxstudio";
    if (appdata && *appdata) return std::string(appdata) + "/phxstudio";
    return "";
}

// A name the user typed -> a safe file stem (letters, digits, _ and -).
std::string clean_stem(const std::string& s) {
    std::string o;
    for (char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-') o += c;
        else if (c == ' ') o += '_';
    }
    return o;
}

// Shared layout for the New dialogs: label on the left, widget on the right.
struct Form {
    Gui& g;
    Rect body;
    int y;
    int label_w;
    Form(Gui& gg, const Rect& b, int lw = 70) : g(gg), body(b), y(b.y), label_w(lw) {}
    Rect row(const char* label, int h = 13) {
        g.text(body.x, y + (h - 8) / 2 + 1, label, g.th.dim);
        const Rect r{ body.x + label_w, y, body.w - label_w, h };
        y += h + 4;
        return r;
    }
    void note(const std::string& s, Rgba c) {
        for (const std::string& line : wrap_words(s, size_t(std::max(10, body.w / kAdv)))) { g.text(body.x, y, line, c); y += 9; }
        y += 2;
    }
    static std::vector<std::string> wrap_words(const std::string& s, size_t cols) {
        std::vector<std::string> out;
        std::string cur;
        size_t p = 0;
        while (p <= s.size()) {
            size_t e = s.find(' ', p);
            if (e == std::string::npos) e = s.size();
            const std::string w = s.substr(p, e - p);
            if (!cur.empty() && cur.size() + 1 + w.size() > cols) { out.push_back(cur); cur.clear(); }
            cur += (cur.empty() ? "" : " ") + w;
            p = e + 1;
        }
        if (!cur.empty()) out.push_back(cur);
        return out;
    }
    // OK / Cancel at the bottom. Returns 1 = ok, -1 = cancel, 0 = nothing.
    int buttons(const char* ok, bool ok_enabled = true) {
        const Rect br{ body.right() - 150, body.bottom() - 14, 70, 14 };
        Btn b; b.enabled = ok_enabled; b.on = ok_enabled;
        if (g.button(br, ok, b) || (ok_enabled && g.focus() == 0 && g.key(PHX_KEY_ENTER))) return 1;
        if (g.button(Rect{ br.right() + 6, br.y, 70, 14 }, "Cancel") || g.key(PHX_KEY_ESCAPE)) return -1;
        return 0;
    }
};

} // namespace

// ---------------------------------------------------------------------------------------------
std::string Workspace::rel(Host& h, const std::string& abs) const {
    const std::string& r = h.root();
    if (abs.compare(0, r.size() + 1, r + "/") == 0) return abs.substr(r.size() + 1);
    return abs;
}

void Workspace::init(Host& h) {
    root_ = h.root();
    tree.open(root_);
    files_cache_.clear();
    filter_.clear();
    tree_scroll_ = 0;
    sel_path_.clear(); sel_dir_.clear();
    recent.clear();
    api_.clear();
    for (const auto& m : h.api_dirs()) api_.push_back(ApiMod{ m.first, m.second, {}, false, false });
}

void Workspace::load_api_files(ApiMod& m) {
    m.files.clear();
    std::error_code ec;
    for (pfs::recursive_directory_iterator it(m.dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string e = lower_ext(it->path().string());
        if (e == ".h" || e == ".hpp") m.files.push_back(it->path().generic_string());
    }
    std::sort(m.files.begin(), m.files.end());
    m.loaded = true;
}

std::vector<Workspace::QuickItem> Workspace::quick_items() {
    std::vector<QuickItem> out;
    for (const std::string& p : all_files()) out.push_back(QuickItem{ p, tree.abs(p), false });
    for (ApiMod& m : api_) {
        if (!m.loaded) load_api_files(m);
        for (const std::string& f : m.files)
            out.push_back(QuickItem{ f.size() > m.dir.size() + 1 ? f.substr(m.dir.size() + 1) : base_name(f), f, true });
    }
    return out;
}

void Workspace::close_all(Host& h) {
    for (auto& d : docs) d->release(h);
    docs.clear();
    active = -1;
}

const std::vector<std::string>& Workspace::all_files() {
    if (files_cache_.empty()) files_cache_ = tree.all_files();
    return files_cache_;
}

bool Workspace::open(Host& h, const std::string& abs0, bool as_text, int line, int col) {
    std::error_code ec;
    std::string abs = pfs::weakly_canonical(abs0, ec).generic_string();   // "/" on Windows too
    if (ec || abs.empty()) abs = abs0;
    // The project boundary: every open (Explorer, quick open, error links, drops, --open) passes here.
    const Access acc = h.access(abs);
    if (acc == Access::None) {
        h.toast(base_name(abs) + " is outside this project - the Studio only opens the project's own files", Toast::Warn);
        return false;
    }
    const bool read_only = acc == Access::Read;
    for (size_t i = 0; i < docs.size(); ++i)
        if (docs[i]->path == abs && (!as_text || docs[i]->kind() == FileKind::Code)) {
            active = int(i);
            if (line > 0) docs[i]->goto_line(line, col);
            note_recent(abs);
            return true;
        }
    std::string err;
    std::unique_ptr<DocView> v = open_view(h, abs, as_text, &err, read_only);
    if (!v) { h.toast(err.empty() ? "cannot open " + base_name(abs) : err, Toast::Bad); return false; }
    if (line > 0) v->goto_line(line, col);
    docs.push_back(std::move(v));
    active = int(docs.size()) - 1;
    if (!read_only) { note_recent(abs); sel_path_ = rel(h, abs); }
    return true;
}

bool Workspace::open_tile(Host& h, const std::string& abs, int index, int tw, int th) {
    if (!open(h, abs)) return false;
    if (DocView* d = current()) d->focus_tile(index, tw, th);
    return true;
}

bool Workspace::save(Host& h, int i) {
    if (i < 0 || i >= int(docs.size())) return false;
    std::string err;
    if (!docs[size_t(i)]->save(h, &err)) { h.toast("save failed: " + err, Toast::Bad); return false; }
    docs[size_t(i)]->disk_stamp = file_stamp(docs[size_t(i)]->path);
    h.toast("saved " + rel(h, docs[size_t(i)]->path), Toast::Good);
    files_cache_.clear();
    return true;
}

bool Workspace::save_all(Host& h) {
    bool ok = true;
    int n = 0;
    for (size_t i = 0; i < docs.size(); ++i)
        if (docs[i]->dirty()) { ok = save(h, int(i)) && ok; ++n; }
    if (n > 1 && ok) h.toast(fmt("saved %d files", n), Toast::Good);
    return ok;
}

bool Workspace::any_dirty() const {
    for (const auto& d : docs) if (d->dirty()) return true;
    return false;
}
int Workspace::dirty_count() const {
    int n = 0;
    for (const auto& d : docs) n += d->dirty() ? 1 : 0;
    return n;
}

void Workspace::close_now(Host& h, int i) {
    if (i < 0 || i >= int(docs.size())) return;
    docs[size_t(i)]->release(h);
    docs.erase(docs.begin() + i);
    if (active >= int(docs.size())) active = int(docs.size()) - 1;
    else if (active > i) --active;
}

void Workspace::close(Host& h, int i) {
    if (i < 0 || i >= int(docs.size())) return;
    if (!docs[size_t(i)]->dirty()) { close_now(h, i); return; }
    DocView* d = docs[size_t(i)].get();
    const std::string name = base_name(d->path);
    h.modal("Unsaved changes", 300, 78, [this, &h, d, name](Gui& g, Rect body) {
        int idx = -1;
        for (size_t k = 0; k < docs.size(); ++k) if (docs[k].get() == d) idx = int(k);
        if (idx < 0) return false;
        g.text(body.x, body.y + 2, "Save changes to " + name + "?", g.th.text, kSubText, body.w);
        g.text(body.x, body.y + 14, "Your changes are lost if you don't save them.", g.th.dim, kSubText, body.w);
        const int bw = 84, y = body.bottom() - 14;
        Btn save_b; save_b.on = true;
        if (g.button(Rect{ body.x, y, bw, 14 }, "Save", save_b) || g.key(PHX_KEY_ENTER)) { if (save(h, idx)) close_now(h, idx); return false; }
        if (g.button(Rect{ body.x + bw + 6, y, bw, 14 }, "Don't save")) { close_now(h, idx); return false; }
        if (g.button(Rect{ body.x + 2 * (bw + 6), y, bw, 14 }, "Cancel") || g.key(PHX_KEY_ESCAPE)) return false;
        return true;
    });
}

void Workspace::next_tab(int dir) {
    if (docs.empty()) return;
    active = ((active + dir) % int(docs.size()) + int(docs.size())) % int(docs.size());
}

void Workspace::note_recent(const std::string& abs) {
    recent.erase(std::remove(recent.begin(), recent.end(), abs), recent.end());
    recent.insert(recent.begin(), abs);
    if (recent.size() > 12) recent.resize(12);
}

std::string Workspace::session_file() const {
    const std::string dir = config_dir();
    if (dir.empty() || root_.empty()) return "";
    uint32_t hsh = 2166136261u;                            // one session per workspace root
    for (char c : root_) { hsh ^= uint8_t(c); hsh *= 16777619u; }
    char name[64];
    std::snprintf(name, sizeof(name), "/session-%08x.txt", unsigned(hsh));
    return dir + name;
}

void Workspace::save_session() const {
    const std::string file = session_file();
    if (file.empty()) return;
    std::error_code ec;
    pfs::create_directories(config_dir(), ec);
    FILE* f = std::fopen(file.c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "# phxstudio session\nroot %s\nside %d %d\n", root_.c_str(), side_w, side_open ? 1 : 0);
    for (const auto& d : docs) std::fprintf(f, "open %s\n", d->path.c_str());
    if (active >= 0 && active < int(docs.size())) std::fprintf(f, "active %d\n", active);
    for (const std::string& r : recent) std::fprintf(f, "recent %s\n", r.c_str());
    std::fclose(f);
}

void Workspace::load_session(Host& h, bool reopen) {
    const std::string file = session_file();
    if (file.empty()) return;
    std::ifstream in(file);
    std::string line, root;
    std::vector<std::string> opens;
    int act = -1;
    while (std::getline(in, line)) {
        if (line.compare(0, 5, "root ") == 0) root = line.substr(5);
        else if (line.compare(0, 5, "open ") == 0) opens.push_back(line.substr(5));
        else if (line.compare(0, 7, "recent ") == 0) {
            const std::string r = line.substr(7);
            if (exists(r) && h.access(r) == Access::Write) recent.push_back(r);
        }
        else if (line.compare(0, 7, "active ") == 0) act = std::atoi(line.c_str() + 7);
        else if (line.compare(0, 5, "side ") == 0) {
            int w = 0, o = 1;
            if (std::sscanf(line.c_str() + 5, "%d %d", &w, &o) == 2) { side_w = std::max(100, std::min(400, w)); side_open = o != 0; }
        }
    }
    if (root != root_) { recent.clear(); return; }           // a different checkout: start clean
    if (reopen)
        for (const std::string& p : opens) if (exists(p) && h.access(p) != Access::None) open(h, p);
    if (act >= 0 && act < int(docs.size())) active = act;
}

void Workspace::poll_disk(Host& h) {
    for (auto& d : docs) {
        const int64_t st = file_stamp(d->path);
        if (!st || st == d->disk_stamp) continue;
        if (!d->dirty()) {
            std::string err;
            if (d->reload(h, &err)) h.toast(base_name(d->path) + " changed on disk - reloaded", Toast::Info);
        } else {
            h.toast(base_name(d->path) + " changed on disk (you have unsaved edits; saving overwrites it)", Toast::Warn);
        }
        d->disk_stamp = st;
    }
}

// ---------------------------------------------------------------------------------------------
void Workspace::draw(Host& h, const Rect& area0) {
    Gui& g = h.gui();
    Rect area = area0;
    if (side_open) {
        const Rect side = cut_left(area, side_w);
        const Rect handle = cut_left(area, 3);
        g.splitter(g.id("ws-split"), handle, side_w, 100, std::max(120, area0.w / 2), true);
        draw_explorer(h, side);
    }
    // tab strip
    const Rect tabs_r = cut_top(area, 14);
    if (docs.empty()) {
        g.rect(tabs_r, g.th.bar, kSubFill);
        draw_welcome(h, area);
        return;
    }
    std::vector<std::string> labels;
    std::vector<bool> marks;
    std::vector<int> icons;
    for (const auto& d : docs) {
        labels.push_back(base_name(d->path));
        marks.push_back(d->dirty());
        icons.push_back(d->icon());
    }
    int close_i = -1;
    const int hit = g.tabs(tabs_r, labels, active, &close_i, &marks, &icons);
    if (hit >= 0) active = hit;
    if (close_i >= 0) close(h, close_i);
    // middle-click a tab closes it (same geometry as Gui::tabs)
    if ((g.in->pressed & kMouseM) && g.hover(tabs_r)) {
        int need = 0;
        for (const std::string& l : labels) need += Gui::text_w(l) + kIconSize + 3 + 12 + 1;
        const int pad = std::max(6, std::min(14, (tabs_r.w - need) / std::max(1, int(labels.size()))));
        int x = tabs_r.x;
        for (size_t i = 0; i < labels.size(); ++i) {
            const int w = Gui::text_w(labels[i]) + pad + kIconSize + 3 + 12;
            if (g.mx() >= x && g.mx() < x + w) { g.in->pressed &= ~kMouseM; close(h, int(i)); break; }
            x += w + 1;
        }
    }
    if (DocView* d = current()) {
        g.tip(tabs_r, rel(h, d->path));
        g.push_clip(area);
        d->draw(h, area);
        g.pop_clip();
    }
}

void Workspace::draw_explorer(Host& h, const Rect& r0) {
    Gui& g = h.gui();
    Rect r = r0;
    g.rect(r, g.th.panel, kSubFill);
    // header
    const Rect head = cut_top(r, 15);
    g.rect(head, g.th.bar, kSubFill);
    const std::string title = h.project_name().empty() ? std::string("EXPLORER") : h.project_name();
    g.text(head.x + 5, head.y + 4, title, h.project_name().empty() ? g.th.dim : g.th.accent, kSubText, head.w - 40);
    g.tip(Rect{ head.x, head.y, head.w - 32, head.h }, h.project_name().empty() ? root_ : "Project folder: " + root_);
    Gui::Row tools(Rect{ head.x, head.y + 2, head.w - 3, 11 }, 1);
    const uint32_t new_menu = g.id("ex-new");
    if (g.icon_button(tools.take_right(13), kIconRefresh, "Rescan the folder tree")) refresh_tree();
    if (g.icon_button(tools.take_right(13), kIconFileNew, "New sprite, map, table, image or file in the selected folder"))
        g.open_popup(new_menu, Rect{ head.right() - 30, head.y, 14, head.h });
    const std::string dir = sel_dir_;
    switch (g.menu(new_menu, { MenuItem{ "Sprite (sheet + clips)...", "", true, false, false, kIconSprite },
                               MenuItem{ "Image / tileset...", "", true, false, false, kIconFileImage },
                               MenuItem{ "Tilemap...", "", true, false, false, kIconFileMap },
                               MenuItem{ "Data table...", "", true, false, false, kIconFileTable },
                               MenuItem{ "Code / text file...", "", true, false, false, kIconFileCode } })) {
    case 0: new_sprite(h, dir); break;
    case 1: new_image(h, dir); break;
    case 2: new_map(h, dir); break;
    case 3: new_table(h, dir); break;
    case 4: new_code(h, dir); break;
    default: break;
    }
    // filter
    const Rect fr = cut_top(r, 16).inset(3, 2);
    g.text_field(g.id("ex-filter"), fr, filter_, "filter files...", kFieldLive, "Type to find files anywhere in the repo (fuzzy)");

    const int row_h = 11;
    Rect list = r.inset(0, 1);
    const Rect sb = cut_right(list, 5);
    const int visible = std::max(1, list.h / row_h);
    g.push_clip(list);
    if (!filter_.empty()) {
        // flat fuzzy results
        struct Hit { int score; const std::string* p; };
        std::vector<Hit> hits;
        for (const std::string& p : all_files()) { const int s = fuzzy_score(filter_, p); if (s >= 0) hits.push_back(Hit{ s, &p }); }
        std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.score > b.score; });
        if (hits.size() > 400) hits.resize(400);
        g.wheel_scroll(list, tree_scroll_, int(hits.size()), visible);
        tree_scroll_ = clamp_scroll(tree_scroll_, int(hits.size()), visible);
        for (int i = 0; i < visible && tree_scroll_ + i < int(hits.size()); ++i) {
            const std::string& p = *hits[size_t(tree_scroll_ + i)].p;
            const Rect rr{ list.x, list.y + i * row_h, list.w, row_h };
            const FileKind k = kind_for(p, lower_ext(p) == ".json" ? read_head(tree.abs(p), 1024) : "");
            if (g.hover(rr)) { g.rect(rr, g.th.hover, kSubWidget); g.hint = p; }
            g.icon(rr.x + 3, rr.y + 1, kind_icon(k), kind_colour(g.th, k));
            const std::string b = base_name(p);
            g.text(rr.x + 16, rr.y + 2, b, g.th.text, kSubText, rr.w - 18);
            const int bw = Gui::text_w(b) + 22;
            if (bw < rr.w - 10) g.text(rr.x + bw, rr.y + 2, dir_name(p), g.th.faint, kSubText, rr.w - bw - 2);
            if (g.clicked(rr)) { open(h, tree.abs(p)); }
        }
        g.pop_clip();
        g.scrollbar_v(g.id("ex-sb"), sb, int(hits.size()), visible, tree_scroll_);
        if (g.focus() == g.id("ex-filter") && g.key(PHX_KEY_ENTER) && !hits.empty()) open(h, tree.abs(*hits[0].p));
        return;
    }

    // One list: the project tree, then (project mode) the read-only ENGINE API section.
    struct ExRow { FileNode* node = nullptr; int depth = 0; int kind = 0; int mod = -1; int file = -1; };
    // kind: 0 project entry, 1 "ENGINE API" header, 2 API module, 3 API header file
    std::vector<ExRow> rows;
    for (const FileTree::Row& fr : tree.rows()) rows.push_back(ExRow{ fr.node, fr.depth, 0, -1, -1 });
    if (!api_.empty()) {
        rows.push_back(ExRow{ nullptr, 0, 1, -1, -1 });
        if (api_open_)
            for (int m = 0; m < int(api_.size()); ++m) {
                rows.push_back(ExRow{ nullptr, 1, 2, m, -1 });
                if (api_[size_t(m)].expanded) {
                    if (!api_[size_t(m)].loaded) load_api_files(api_[size_t(m)]);
                    for (int f = 0; f < int(api_[size_t(m)].files.size()); ++f) rows.push_back(ExRow{ nullptr, 2, 3, m, f });
                }
            }
    }
    g.wheel_scroll(list, tree_scroll_, int(rows.size()), visible);
    tree_scroll_ = clamp_scroll(tree_scroll_, int(rows.size()), visible);
    for (int i = 0; i < visible && tree_scroll_ + i < int(rows.size()); ++i) {
        const ExRow& er = rows[size_t(tree_scroll_ + i)];
        const Rect rr{ list.x, list.y + i * row_h, list.w, row_h };
        if (er.kind != 0) {
            // ---- the engine's public API (read-only) ----
            const bool h_ = g.hover(rr);
            if (h_) g.rect(rr, g.th.hover, kSubWidget);
            int x = rr.x + 2 + er.depth * 8;
            if (er.kind == 1) {
                g.rect(Rect{ rr.x, rr.y, rr.w, 1 }, g.th.line, kSubImage);
                g.icon(x, rr.y + 1, api_open_ ? kIconChevronDown : kIconChevronRight, g.th.faint);
                g.icon(x + 9, rr.y + 1, kIconLock, g.th.warn);
                g.text(x + 21, rr.y + 2, "ENGINE API", g.th.warn, kSubText, rr.right() - x - 60);
                g.text(rr.right() - 44, rr.y + 2, "read-only", g.th.faint);
                if (h_) g.hint = "The engine's public headers (phx/...): read them for reference; a project can't change them";
                if (g.clicked(rr)) api_open_ = !api_open_;
            } else if (er.kind == 2) {
                ApiMod& m = api_[size_t(er.mod)];
                g.icon(x, rr.y + 1, m.expanded ? kIconChevronDown : kIconChevronRight, g.th.faint);
                g.icon(x + 9, rr.y + 1, m.expanded ? kIconFolderOpen : kIconFolder, g.th.dim);
                g.text(x + 21, rr.y + 2, "phx/" + m.name, g.th.dim, kSubText, rr.right() - x - 23);
                if (h_) g.hint = "engine module '" + m.name + "': its public API headers (read-only)";
                if (g.clicked(rr)) m.expanded = !m.expanded;
            } else {
                const ApiMod& m = api_[size_t(er.mod)];
                const std::string& f = m.files[size_t(er.file)];
                const bool open_doc = [&] { for (auto& d : docs) if (d->path == f) return true; return false; }();
                g.icon(x + 9, rr.y + 1, kIconLock, g.th.faint);
                g.text(x + 21, rr.y + 2, base_name(f), open_doc ? g.th.accent : g.th.dim, kSubText, rr.right() - x - 23);
                const std::string inc = f.size() > m.dir.size() + 1 ? f.substr(m.dir.size() + 1) : base_name(f);
                if (h_) g.hint = "#include \"" + inc + "\"  (read-only engine API)";
                if (g.clicked(rr)) open(h, f);
            }
            continue;
        }
        FileNode* n = er.node;
        const int depth = er.depth;
        const bool sel = n->path == sel_path_;
        const bool open_doc = [&] { for (auto& d : docs) if (d->path == tree.abs(n->path)) return true; return false; }();
        if (sel) g.rect(rr, g.th.panel2, kSubWidget);
        else if (g.hover(rr)) g.rect(rr, g.th.hover, kSubWidget);
        int x = rr.x + 2 + depth * 8;
        if (n->dir) g.icon(x, rr.y + 1, n->expanded ? kIconChevronDown : kIconChevronRight, g.th.faint);
        x += 9;
        g.icon(x, rr.y + 1, n->dir && n->expanded ? kIconFolderOpen : kind_icon(n->kind), kind_colour(g.th, n->kind));
        x += 12;
        g.text(x, rr.y + 2, n->name, open_doc ? g.th.accent : n->dir ? g.th.text : g.th.dim, kSubText, rr.right() - x - 2);
        if (g.hover(rr)) g.hint = n->path + "  (" + kind_name(n->kind) + ")";
        if (g.right_clicked(rr)) {
            ctx_path_ = n->path; ctx_dir_ = n->dir;
            sel_path_ = n->path;
            sel_dir_ = n->dir ? n->path : dir_name(n->path);
            g.open_context(g.id("ex-ctx"));
        }
        if (g.clicked(rr)) {
            sel_path_ = n->path;
            if (n->dir) { n->expanded = !n->expanded; if (n->expanded && !n->loaded) tree.load(*n); sel_dir_ = n->path; }
            else { sel_dir_ = dir_name(n->path); open(h, tree.abs(n->path)); }
        }
    }
    g.pop_clip();
    g.scrollbar_v(g.id("ex-sb"), sb, int(rows.size()), visible, tree_scroll_);
    explorer_context(h);
}

void Workspace::explorer_context(Host& h) {
    Gui& g = h.gui();
    const uint32_t id = g.id("ex-ctx");
    if (!g.popup_open(id)) return;
    const std::string dir = ctx_dir_ ? ctx_path_ : dir_name(ctx_path_);
    std::vector<MenuItem> items;
    if (!ctx_dir_) {
        items.push_back(MenuItem{ "Open", "", true, false, false, kIconFile });
        items.push_back(MenuItem{ "Open as text", "", true, false, false, kIconFileCode });
    } else {
        items.push_back(MenuItem{ "Expand / collapse", "", true, false, false, kIconFolderOpen });
        items.push_back(MenuItem{ "Collapse all", "", true, false, false, kIconFolder });
    }
    items.push_back(MenuItem::sep());
    items.push_back(MenuItem{ "New sprite here...", "", true, false, false, kIconSprite });
    items.push_back(MenuItem{ "New image here...", "", true, false, false, kIconFileImage });
    items.push_back(MenuItem{ "New tilemap here...", "", true, false, false, kIconFileMap });
    items.push_back(MenuItem{ "New table here...", "", true, false, false, kIconFileTable });
    items.push_back(MenuItem{ "New font here...", "", true, false, false, kIconFileImage });
    items.push_back(MenuItem{ "New sound effect here...", "", true, false, false, kIconFileSound });
    items.push_back(MenuItem{ "New song here...", "", true, false, false, kIconFileSound });
    items.push_back(MenuItem{ "New dialogue here...", "", true, false, false, kIconFileCode });
    items.push_back(MenuItem{ "New file here...", "", true, false, false, kIconFileCode });
    items.push_back(MenuItem::sep());
    items.push_back(MenuItem{ "Copy path", "", true, false, false, kIconCopy });
    const int hit = g.menu(id, items);
    switch (hit) {
    case 0:
        if (!ctx_dir_) open(h, tree.abs(ctx_path_));
        else if (FileNode* n = tree.find(ctx_path_)) { n->expanded = !n->expanded; if (n->expanded) tree.load(*n); }
        break;
    case 1:
        if (!ctx_dir_) open(h, tree.abs(ctx_path_), true);
        else if (FileNode* n = tree.find(ctx_path_)) { n->expanded = false; for (FileNode& k : n->kids) k.expanded = false; }
        break;
    case 3: new_sprite(h, dir); break;
    case 4: new_image(h, dir); break;
    case 5: new_map(h, dir); break;
    case 6: new_table(h, dir); break;
    case 7: new_font(h, dir); break;
    case 8: new_sfx(h, dir); break;
    case 9: new_song(h, dir); break;
    case 10: new_dialogue(h, dir); break;
    case 11: new_code(h, dir); break;
    case 13: phx_desktop_clipboard_set(ctx_path_.c_str()); h.toast("copied " + ctx_path_); break;
    default: break;
    }
}

void Workspace::draw_welcome(Host& h, const Rect& r) {
    Gui& g = h.gui();
    g.rect(r, g.th.bg, kSubBg);
    const int cx = r.x + std::max(12, (r.w - 420) / 2);
    int y = r.y + std::max(10, (r.h - 250) / 3);
    const std::string pname = h.project_name();
    g.text(cx, y, pname.empty() ? std::string("Phosphorus Studio") : pname, g.th.accent, kSubText, r.w, 2);
    y += 20;
    if (!pname.empty()) {
        g.text(cx, y, "A Phosphorus game project: " + root_, g.th.dim, kSubText, r.w - 20);
        g.text(cx, y + 10, "Everything you edit here stays inside this folder. The engine's public", g.th.dim, kSubText, r.w - 20);
        g.text(cx, y + 20, "API is in the Explorer under ENGINE API, read-only. Run > Play builds it.", g.th.dim, kSubText, r.w - 20);
    } else {
        g.text(cx, y, "Make sprites, tilemaps, data tables and code for one engine that runs", g.th.dim, kSubText, r.w - 20);
        g.text(cx, y + 10, "on GBA, PSP and PC. Everything saves as ordinary author files that the", g.th.dim, kSubText, r.w - 20);
        g.text(cx, y + 20, "phxpack bake turns into a .phxp bundle.", g.th.dim, kSubText, r.w - 20);
    }
    y += 40;
    const std::string dir = sel_dir_;
    g.section(cx, y, 200, "CREATE", g.th.faint);
    int yy = y + 13;
    struct Item { const char* label; int icon; const char* help; int what; };
    const Item create[] = {
        { "New sprite", kIconSprite, "A sprite sheet PNG + a .sprdef with animation clips", 0 },
        { "New tilemap", kIconFileMap, "A Tiled .tmj map: tile layers, collision, spawns", 1 },
        { "New data table", kIconFileTable, "A phxbin record table (stats, prefabs, dialogue...)", 2 },
        { "New image / tileset", kIconFileImage, "A blank PNG to paint tiles or a portrait in", 3 },
        { "New font", kIconFileImage, "A .font over a glyph sheet PNG (starts as a 5x7 ASCII font)", 5 },
        { "New sound effect", kIconFileSound, "A .sfx: a jump, coin, laser... from presets", 6 },
        { "New song", kIconFileSound, "A .song: a small tracker for the game's music", 7 },
        { "New dialogue", kIconFileCode, "A .dlg: conversations with choices (Talk NPCs, cutscenes)", 8 },
        { "New code file", kIconFileCode, "C++, Markdown, a script...", 4 },
    };
    for (const Item& it : create) {
        const Rect br{ cx, yy, 190, 14 };
        Btn b; b.icon = it.icon; b.help = it.help; b.flat = true; b.left = true;
        if (g.button(br, it.label, b)) {
            if (it.what == 0) new_sprite(h, dir); else if (it.what == 1) new_map(h, dir);
            else if (it.what == 2) new_table(h, dir); else if (it.what == 3) new_image(h, dir);
            else if (it.what == 5) new_font(h, dir); else if (it.what == 6) new_sfx(h, dir);
            else if (it.what == 7) new_song(h, dir); else if (it.what == 8) new_dialogue(h, dir); else new_code(h, dir);
        }
        yy += 16;
    }
    const int rx = cx + 216;
    g.section(rx, y, 200, "OPEN", g.th.faint);
    yy = y + 13;
    {
        Btn b; b.icon = kIconSearch; b.flat = true; b.left = true; b.help = "Fuzzy-find any file in the repository";
        if (g.button(Rect{ rx, yy, 190, 14 }, "Quick open...  Ctrl+P", b)) quick_open(h);
        yy += 18;
    }
    if (!recent.empty()) {
        g.text(rx, yy, "recent", g.th.faint);
        yy += 11;
        for (size_t i = 0; i < recent.size() && i < 8; ++i) {
            const Rect br{ rx, yy, 200, 12 };
            const std::string rp = rel(h, recent[i]);
            const FileKind k = kind_for(rp, lower_ext(rp) == ".json" ? read_head(recent[i], 1024) : "");
            if (g.hover(br)) { g.rect(br, g.th.hover, kSubWidget); g.hint = rp; }
            g.icon(br.x + 2, br.y + 1, kind_icon(k), kind_colour(g.th, k));
            g.text(br.x + 15, br.y + 2, base_name(rp), g.th.text, kSubText, 80);
            g.text(br.x + 15 + std::min(80, Gui::text_w(base_name(rp))) + 6, br.y + 2, dir_name(rp), g.th.faint, kSubText, 100);
            if (g.clicked(br)) { open(h, recent[i]); break; }
            yy += 12;
        }
    }
    y = std::max(yy, y + 13 + int(sizeof(create) / sizeof(create[0])) * 16) + 14;   // below both columns
    g.section(cx, y, 416, "HANDY KEYS", g.th.faint);
    y += 13;
    const char* keys[][2] = {
        { "Ctrl+P", "quick open" }, { "Ctrl+S / Ctrl+Shift+S", "save / save all" }, { "Ctrl+W", "close tab" },
        { "Ctrl+Tab", "next tab" }, { "Ctrl+Z / Ctrl+Y", "undo / redo" },
        { pname.empty() ? "Ctrl+1..4" : "Ctrl+1..3", pname.empty() ? "Overview / Assets / Editor / Run" : "Editor / Assets / Run" },
        { "Ctrl+= / Ctrl+-", "bigger / smaller UI" }, { "F1", "all shortcuts" },
    };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        const int ky = y + int(i) * 10;
        g.text(cx, ky, keys[i][0], g.th.accent);
        g.text(cx + 138, ky, keys[i][1], g.th.dim, kSubText, r.right() - cx - 140);
    }
}

// ---------------------------------------------------------------------------------------------
void Workspace::quick_open(Host& h) {
    struct St { std::string q; int sel = 0; bool first = true; std::vector<QuickItem> items; };
    auto st = std::make_shared<St>();
    h.modal("Quick open", std::min(460, h.gui().W - 40), 214, [this, &h, st](Gui& g, Rect body) {
        const uint32_t fid = g.id("qo");
        if (st->first) { g.set_focus(fid, true); g.edit().set("", false); st->first = false; }
        if (st->items.empty()) st->items = quick_items();
        struct Hit { int score; const QuickItem* p; };
        std::vector<Hit> hits;
        for (const QuickItem& it : st->items) {
            const int sc = fuzzy_score(st->q, it.display);
            if (sc >= 0) hits.push_back(Hit{ it.api ? sc / 2 : sc, &it });     // the project's own files first
        }
        if (!st->q.empty()) std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.score > b.score; });
        const int rows = (body.h - 20) / 11;
        if (g.key(PHX_KEY_DOWN)) st->sel = std::min(int(hits.size()) - 1, st->sel + 1);
        if (g.key(PHX_KEY_UP)) st->sel = std::max(0, st->sel - 1);
        if (g.key(PHX_KEY_ESCAPE)) return false;
        if (g.key(PHX_KEY_ENTER)) {
            if (st->sel >= 0 && st->sel < int(hits.size())) open(h, hits[size_t(st->sel)].p->abs);
            return false;
        }
        const std::string before = st->q;
        g.text_field(fid, Rect{ body.x, body.y, body.w, 14 }, st->q, "type part of a file name (fuzzy)", kFieldLive);
        if (st->q != before) st->sel = 0;
        if (g.focus() != fid) g.set_focus(fid, true);
        const int first = std::max(0, st->sel - rows + 1);
        for (int i = 0; i < rows && first + i < int(hits.size()); ++i) {
            const int k = first + i;
            const QuickItem& it = *hits[size_t(k)].p;
            const std::string& p = it.display;
            const Rect rr{ body.x, body.y + 18 + i * 11, body.w, 11 };
            if (k == st->sel) g.rect(rr, g.th.sel, kSubWidget);
            else if (g.hover(rr)) g.rect(rr, g.th.hover, kSubWidget);
            const FileKind kk = kind_for(p);
            g.icon(rr.x + 2, rr.y + 1, it.api ? kIconLock : kind_icon(kk), it.api ? g.th.warn : kind_colour(g.th, kk));
            g.text(rr.x + 15, rr.y + 2, base_name(p), g.th.text, kSubText, 150);
            g.text(rr.x + 170, rr.y + 2, it.api ? "engine API (read-only)  " + dir_name(p) : dir_name(p), g.th.faint, kSubText, rr.w - 172);
            if (g.clicked(rr)) { open(h, it.abs); return false; }
        }
        if (hits.empty()) g.text(body.x + 4, body.y + 22, "no matching file", g.th.dim);
        return true;
    });
}

void Workspace::new_sprite(Host& h, const std::string& dir0) {
    struct St { std::string name = "hero", dir; int fw = 16, fh = 16, frames = 4; bool json = false; int pal = 0; std::string err; };
    auto st = std::make_shared<St>();
    st->dir = dir0;
    h.modal("New sprite", 330, 170, [this, &h, st](Gui& g, Rect body) {
        Form f(g, body);
        g.text_field(g.id("ns-name"), f.row("name"), st->name, "e.g. hero");
        g.text_field(g.id("ns-dir"), f.row("folder"), st->dir, "(repo root)");
        const Rect fr = f.row("frame size");
        g.int_field(g.id("ns-fw"), Rect{ fr.x, fr.y, 50, fr.h }, st->fw, 1, 256, 1, "Frame width in pixels");
        g.text(fr.x + 54, fr.y + 3, "x", g.th.dim);
        g.int_field(g.id("ns-fh"), Rect{ fr.x + 62, fr.y, 50, fr.h }, st->fh, 1, 256, 1, "Frame height in pixels");
        g.int_field(g.id("ns-n"), Rect{ f.row("frames").x, f.y - 17, 50, 13 }, st->frames, 1, 256, 1, "Frames in the sheet (laid out left to right)");
        const Rect ff = f.row("format");
        if (g.button(Rect{ ff.x, ff.y, 60, 13 }, ".sprdef", Btn{ !st->json })) st->json = false;
        if (g.button(Rect{ ff.x + 64, ff.y, 60, 13 }, ".json", Btn{ st->json })) st->json = true;
        const std::string stem = clean_stem(st->name);
        const std::string base = join_path(st->dir, stem);
        f.note("Creates " + base + ".png (" + std::to_string(st->fw * st->frames) + "x" + std::to_string(st->fh) + ") and " +
               base + (st->json ? ".json" : ".sprdef") + " with an 'idle' clip.", g.th.faint);
        if (!st->err.empty()) f.note(st->err, g.th.bad);
        const int b = f.buttons("Create", !stem.empty());
        if (b < 0) return false;
        if (b > 0) {
            const std::string png = join_path(h.root(), base + ".png");
            const std::string def = join_path(h.root(), base + (st->json ? ".json" : ".sprdef"));
            if (h.access(png) != Access::Write || h.access(def) != Access::Write) { st->err = "that folder is outside the project"; return true; }
            if (exists(png) || exists(def)) { st->err = "a file with that name already exists"; return true; }
            std::error_code ec;
            pfs::create_directories(pfs::path(png).parent_path(), ec);
            PixelDoc img = PixelDoc::blank(st->fw * st->frames, st->fh, 0);
            std::string err;
            if (!img.save_png(png, &err)) { st->err = err; return true; }
            SprDoc sd;
            sd.sheet = stem + ".png"; sd.frame_w = st->fw; sd.frame_h = st->fh; sd.json = st->json;
            sd.clips.push_back(SprClip{ "idle", 0, st->frames, 8, true });
            if (!sd.save(def, &err)) { st->err = err; return true; }
            refresh_tree();
            tree.reveal(rel(h, def));
            open(h, def);
            h.file_saved(def);
            h.toast("created " + rel(h, def), Toast::Good);
            return false;
        }
        return true;
    });
}

void Workspace::new_image(Host& h, const std::string& dir0) {
    struct St { std::string name = "tiles", dir; int w = 128, hh = 64; std::string err; };
    auto st = std::make_shared<St>();
    st->dir = dir0;
    h.modal("New image / tileset", 320, 132, [this, &h, st](Gui& g, Rect body) {
        Form f(g, body);
        g.text_field(g.id("ni-name"), f.row("name"), st->name, "e.g. tiles");
        g.text_field(g.id("ni-dir"), f.row("folder"), st->dir, "(repo root)");
        const Rect sr = f.row("size");
        g.int_field(g.id("ni-w"), Rect{ sr.x, sr.y, 50, sr.h }, st->w, 1, 2048, 8, "Width in pixels (tilesets: a multiple of the tile size)");
        g.text(sr.x + 54, sr.y + 3, "x", g.th.dim);
        g.int_field(g.id("ni-h"), Rect{ sr.x + 62, sr.y, 50, sr.h }, st->hh, 1, 2048, 8, "Height in pixels");
        const std::string stem = clean_stem(st->name);
        f.note("GBA tip: keep tiles 8x8 multiples with <= 15 colours each.", g.th.faint);
        if (!st->err.empty()) f.note(st->err, g.th.bad);
        const int b = f.buttons("Create", !stem.empty());
        if (b < 0) return false;
        if (b > 0) {
            const std::string png = join_path(h.root(), join_path(st->dir, stem + ".png"));
            if (h.access(png) != Access::Write) { st->err = "that folder is outside the project"; return true; }
            if (exists(png)) { st->err = "that file already exists"; return true; }
            std::error_code ec;
            pfs::create_directories(pfs::path(png).parent_path(), ec);
            PixelDoc img = PixelDoc::blank(st->w, st->hh, 0);
            std::string err;
            if (!img.save_png(png, &err)) { st->err = err; return true; }
            refresh_tree();
            tree.reveal(rel(h, png));
            open(h, png);
            h.file_saved(png);
            h.toast("created " + rel(h, png), Toast::Good);
            return false;
        }
        return true;
    });
}

// A text file at `path` (fails if it exists or is outside the project).
static bool create_text(Host& h, const std::string& path, const std::string& text, std::string& err) {
    if (h.access(path) != Access::Write) { err = "that folder is outside the project"; return false; }
    if (pfs::exists(path)) { err = "that file already exists"; return false; }
    std::error_code ec;
    pfs::create_directories(pfs::path(path).parent_path(), ec);
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { err = "cannot write " + path; return false; }
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    std::fclose(f);
    if (!ok) err = "short write to " + path;
    return ok;
}

void Workspace::new_font(Host& h, const std::string& dir0) {
    struct St { std::string name = "font", dir; std::string err; };
    auto st = std::make_shared<St>();
    st->dir = dir0;
    h.modal("New font", 330, 120, [this, &h, st](Gui& g, Rect body) {
        Form f(g, body);
        g.text_field(g.id("nf-name"), f.row("name"), st->name, "e.g. font");
        g.text_field(g.id("nf-dir"), f.row("folder"), st->dir, "(repo root)");
        const std::string stem = clean_stem(st->name);
        f.note("A 128x48 sheet of 8x8 cells (ASCII 32..127, a 5x7 font to repaint) + " + stem + ".font.", g.th.faint);
        if (!st->err.empty()) f.note(st->err, g.th.bad);
        const int b = f.buttons("Create", !stem.empty());
        if (b < 0) return false;
        if (b > 0) {
            const std::string png = join_path(h.root(), join_path(st->dir, stem + ".png"));
            const std::string def = join_path(h.root(), join_path(st->dir, stem + ".font"));
            if (h.access(png) != Access::Write) { st->err = "that folder is outside the project"; return true; }
            if (exists(png) || exists(def)) { st->err = "that file already exists"; return true; }
            std::error_code ec;
            pfs::create_directories(pfs::path(png).parent_path(), ec);
            PixelDoc img = PixelDoc::blank(phxtool::kAsciiFontW, phxtool::kAsciiFontH, 0);
            phxtool::build_ascii_font(img.px.data());
            std::string err;
            if (!img.save_png(png, &err)) { st->err = err; return true; }
            phxtool::FontDef fd;
            fd.image = stem + ".png";
            if (!create_text(h, def, phxtool::fontdef_to_json(fd), st->err)) return true;
            refresh_tree();
            tree.reveal(rel(h, def));
            open(h, def);
            h.file_saved(def);
            h.toast("created " + rel(h, def), Toast::Good);
            return false;
        }
        return true;
    });
}

void Workspace::new_sfx(Host& h, const std::string& dir0) {
    struct St { std::string name = "blip", dir; int kind = 6; std::string err; };
    auto st = std::make_shared<St>();
    st->dir = dir0;
    h.modal("New sound effect", 330, 120, [this, &h, st](Gui& g, Rect body) {
        Form f(g, body);
        g.text_field(g.id("ns-name"), f.row("name"), st->name, "e.g. coin");
        g.text_field(g.id("ns-dir"), f.row("folder"), st->dir, "(repo root)");
        g.dropdown(g.id("ns-kind"), f.row("start as"), phxtool::sfx_preset_names(), st->kind, "A preset to start from");
        const std::string stem = clean_stem(st->name);
        if (!st->err.empty()) f.note(st->err, g.th.bad);
        const int b = f.buttons("Create", !stem.empty());
        if (b < 0) return false;
        if (b > 0) {
            const std::string p = join_path(h.root(), join_path(st->dir, stem + ".sfx"));
            const phxtool::SfxParams sp = phxtool::sfx_preset(phxtool::sfx_preset_names()[size_t(st->kind)], 1);
            if (!create_text(h, p, phxtool::sfx_to_json(sp), st->err)) return true;
            refresh_tree();
            tree.reveal(rel(h, p));
            open(h, p);
            h.file_saved(p);
            h.toast("created " + rel(h, p) + " - res->sound(\"" + stem + "\"_hash)", Toast::Good);
            return false;
        }
        return true;
    });
}

void Workspace::new_song(Host& h, const std::string& dir0) {
    struct St { std::string name = "music", dir; std::string err; };
    auto st = std::make_shared<St>();
    st->dir = dir0;
    h.modal("New song", 330, 110, [this, &h, st](Gui& g, Rect body) {
        Form f(g, body);
        g.text_field(g.id("ng-name"), f.row("name"), st->name, "e.g. level1");
        g.text_field(g.id("ng-dir"), f.row("folder"), st->dir, "(repo root)");
        const std::string stem = clean_stem(st->name);
        f.note("Starts with 4 instruments and two 16-row patterns.", g.th.faint);
        if (!st->err.empty()) f.note(st->err, g.th.bad);
        const int b = f.buttons("Create", !stem.empty());
        if (b < 0) return false;
        if (b > 0) {
            const std::string p = join_path(h.root(), join_path(st->dir, stem + ".song"));
            if (!create_text(h, p, phxtool::song_to_json(phxtool::song_starter()), st->err)) return true;
            refresh_tree();
            tree.reveal(rel(h, p));
            open(h, p);
            h.file_saved(p);
            h.toast("created " + rel(h, p) + " - the flow table's music column plays it", Toast::Good);
            return false;
        }
        return true;
    });
}

void Workspace::new_dialogue(Host& h, const std::string& dir0) {
    struct St { std::string name = "dialogue", dir; std::string err; };
    auto st = std::make_shared<St>();
    st->dir = dir0;
    h.modal("New dialogue", 340, 118, [this, &h, st](Gui& g, Rect body) {
        Form f(g, body);
        g.text_field(g.id("nd-name"), f.row("name"), st->name, "e.g. dialogue");
        g.text_field(g.id("nd-dir"), f.row("folder"), st->dir, "(repo root)");
        const std::string stem = clean_stem(st->name);
        f.note("The game flow plays assets/dialogue.dlg (a talk screen, a Talk component).", g.th.faint);
        if (!st->err.empty()) f.note(st->err, g.th.bad);
        const int b = f.buttons("Create", !stem.empty());
        if (b < 0) return false;
        if (b > 0) {
            const std::string p = join_path(h.root(), join_path(st->dir, stem + ".dlg"));
            if (!create_text(h, p, phxtool::dlg_to_json(phxtool::dlg_starter()), st->err)) return true;
            refresh_tree();
            tree.reveal(rel(h, p));
            open(h, p);
            h.file_saved(p);
            h.toast("created " + rel(h, p), Toast::Good);
            return false;
        }
        return true;
    });
}

void Workspace::new_map(Host& h, const std::string& dir0) {
    struct St { std::string name = "level1", dir; int w = 32, hh = 20, tw = 8, th = 8; int tileset = 0; std::string err;
                std::vector<std::string> pngs; };
    auto st = std::make_shared<St>();
    st->dir = dir0;
    st->pngs.push_back("(none - coloured swatches)");
    for (const std::string& p : h.files_with({ ".png" })) st->pngs.push_back(p);
    h.modal("New tilemap", 360, 172, [this, &h, st](Gui& g, Rect body) {
        Form f(g, body, 76);
        g.text_field(g.id("nm-name"), f.row("name"), st->name, "e.g. level1");
        g.text_field(g.id("nm-dir"), f.row("folder"), st->dir, "(repo root)");
        const Rect sr = f.row("map (tiles)");
        g.int_field(g.id("nm-w"), Rect{ sr.x, sr.y, 50, sr.h }, st->w, 1, 1024, 1, "Map width in tiles");
        g.text(sr.x + 54, sr.y + 3, "x", g.th.dim);
        g.int_field(g.id("nm-h"), Rect{ sr.x + 62, sr.y, 50, sr.h }, st->hh, 1, 1024, 1, "Map height in tiles");
        const Rect tr = f.row("tile size");
        g.int_field(g.id("nm-tw"), Rect{ tr.x, tr.y, 50, tr.h }, st->tw, 1, 64, 1, "Tile width (the GBA PPU uses 8)");
        g.text(tr.x + 54, tr.y + 3, "x", g.th.dim);
        g.int_field(g.id("nm-th"), Rect{ tr.x + 62, tr.y, 50, tr.h }, st->th, 1, 64, 1, "Tile height");
        g.dropdown(g.id("nm-ts"), f.row("tileset"), st->pngs, st->tileset, "The tileset image (its file name is the texture the bake references)");
        const std::string stem = clean_stem(st->name);
        if (!st->err.empty()) f.note(st->err, g.th.bad);
        else f.note("The LAST tile layer is the gameplay (solid) layer; earlier layers are parallax backdrops.", g.th.faint);
        const int b = f.buttons("Create", !stem.empty());
        if (b < 0) return false;
        if (b > 0) {
            const std::string tmj = join_path(h.root(), join_path(st->dir, stem + ".tmj"));
            if (h.access(tmj) != Access::Write) { st->err = "that folder is outside the project"; return true; }
            if (exists(tmj)) { st->err = "that file already exists"; return true; }
            phxtool::TmapDoc d = phxtool::TmapDoc::blank(st->w, st->hh, st->tw, st->th, "tiles");
            if (st->tileset > 0) {
                const std::string png_rel = st->pngs[size_t(st->tileset)];
                std::error_code ec;
                d.tileset = stem_of(png_rel);
                d.tileset_image = pfs::relative(join_path(h.root(), png_rel), pfs::path(tmj).parent_path(), ec).generic_string();
                if (ec || d.tileset_image.empty()) d.tileset_image = png_rel;
            }
            std::error_code ec;
            pfs::create_directories(pfs::path(tmj).parent_path(), ec);
            if (!d.save_file(tmj)) { st->err = "cannot write " + tmj; return true; }
            refresh_tree();
            tree.reveal(rel(h, tmj));
            open(h, tmj);
            h.file_saved(tmj);
            h.toast("created " + rel(h, tmj), Toast::Good);
            return false;
        }
        return true;
    });
}

void Workspace::new_table(Host& h, const std::string& dir0) {
    struct St { std::string name = "prefabs", dir, sname = "Prefab", fields = "type:str16, hp:u16, speed:u8, damage:u8"; std::string err; };
    auto st = std::make_shared<St>();
    st->dir = dir0;
    h.modal("New data table", 380, 158, [this, &h, st](Gui& g, Rect body) {
        Form f(g, body, 76);
        g.text_field(g.id("nt-name"), f.row("file name"), st->name, "e.g. prefabs");
        g.text_field(g.id("nt-dir"), f.row("folder"), st->dir, "(repo root)");
        g.text_field(g.id("nt-s"), f.row("struct"), st->sname, "C struct name, e.g. Prefab");
        g.text_field(g.id("nt-f"), f.row("fields"), st->fields, "name:type, ...", 0,
                     "u8 i8 u16 i16 u32 i32 f32 str8 str16 str32 str64. A str 'type' column makes it a prefab table");
        if (!st->err.empty()) f.note(st->err, g.th.bad);
        else f.note("A string column named 'type' makes this the prefab vocabulary the map editor places.", g.th.faint);
        const std::string stem = clean_stem(st->name);
        const int b = f.buttons("Create", !stem.empty());
        if (b < 0) return false;
        if (b > 0) {
            std::vector<std::string> specs;
            std::string cur;
            for (char c : st->fields + ",") {
                if (c != ',') { cur += c; continue; }
                const size_t a = cur.find_first_not_of(' '), e = cur.find_last_not_of(' ');
                if (a != std::string::npos) specs.push_back(cur.substr(a, e - a + 1));
                cur.clear();
            }
            phxtool::BinDoc d;
            std::string err;
            if (!phxtool::BinDoc::valid_ident(st->sname)) { st->err = "struct name must be a C identifier"; return true; }
            if (!phxtool::BinDoc::blank(st->sname, specs, d, &err)) { st->err = err; return true; }
            for (const auto& fl : d.fields) if (!phxtool::BinDoc::valid_ident(fl.name)) { st->err = "field '" + fl.name + "' is not a C identifier"; return true; }
            d.add_record(size_t(-1));
            const std::string json = join_path(h.root(), join_path(st->dir, stem + ".json"));
            if (h.access(json) != Access::Write) { st->err = "that folder is outside the project"; return true; }
            if (exists(json)) { st->err = "that file already exists"; return true; }
            std::error_code ec;
            pfs::create_directories(pfs::path(json).parent_path(), ec);
            if (!d.save_file(json)) { st->err = "cannot write " + json; return true; }
            refresh_tree();
            tree.reveal(rel(h, json));
            open(h, json);
            h.file_saved(json);
            h.toast("created " + rel(h, json), Toast::Good);
            return false;
        }
        return true;
    });
}

void Workspace::new_code(Host& h, const std::string& dir0) {
    struct St { std::string path; std::string err; bool first = true; };
    auto st = std::make_shared<St>();
    st->path = dir0.empty() ? "" : dir0 + "/";
    h.modal("New file", 360, 100, [this, &h, st](Gui& g, Rect body) {
        Form f(g, body, 40);
        const uint32_t id = g.id("nc-path");
        if (st->first) { g.set_focus(id, true); g.edit().set(st->path, false); st->first = false; }
        g.text_field(id, f.row("path"), st->path, "e.g. src/player.cpp");
        if (!st->err.empty()) f.note(st->err, g.th.bad);
        else f.note(h.project_name().empty() ? "Relative to the repository root. .h files start with an include guard."
                                             : "Relative to the project folder. .h files start with an include guard.", g.th.faint);
        const bool ok = !st->path.empty() && st->path.back() != '/';
        const int b = f.buttons("Create", ok);
        if (b < 0) return false;
        if (b > 0) {
            const std::string abs = join_path(h.root(), st->path);
            if (h.access(abs) != Access::Write) { st->err = "that path is outside the project"; return true; }
            if (exists(abs)) { open(h, abs); return false; }        // already there: just open it
            std::string err;
            if (!write_text_file(abs, code_template(st->path), &err)) { st->err = err; return true; }
            refresh_tree();
            tree.reveal(st->path);
            open(h, abs);
            h.file_saved(abs);
            h.toast("created " + st->path, Toast::Good);
            return false;
        }
        return true;
    });
}

} // namespace phxstudio
