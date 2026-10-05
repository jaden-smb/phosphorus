// tools/phxstudio/host.h — the seam between an EDITOR PANEL and the window that hosts it.
//
// Each editor (code, sprite/pixel, map, table) is a DocView: it owns one document, draws itself
// into a rect with the tool widget kit (tools/common/twk.h), and asks its Host for everything
// outside that rect — status toasts, modal dialogs, opening another file, the prefab vocabulary,
// the animation clock. Phosphorus Studio is one Host (tabs, explorer, menus); the standalone
// phxtmap / phxentity windows are another (one document, a slim toolbar) — the SAME panels in
// both, so there is exactly one map editor and one table editor in the tree.
//
// Host-only. No SDL: windowing goes through phx/platform/desktop.h, drawing through twk/phx::UI.
#ifndef PHX_TOOLS_PHXSTUDIO_HOST_H
#define PHX_TOOLS_PHXSTUDIO_HOST_H

#include "twk.h"
#include "project.h"
#include "projectdoc.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace phx { class Renderer; }
namespace phxtool { class TmapDoc; class BinDoc; }

namespace phxstudio {

using twk::Rect;
using twk::Rgba;
using twk::rgba;

enum class Toast : uint8_t { Info, Good, Warn, Bad };

class Host {
public:
    virtual ~Host() = default;
    virtual twk::Gui& gui() = 0;
    virtual phx::Renderer& renderer() = 0;
    // The workspace root: the open PROJECT's folder (or, with --engine-dev, the engine checkout).
    // Every relative path the panels handle (pickers, dialogs) is relative to it.
    virtual const std::string& root() const = 0;
    // What the Studio may do with a path (projectdoc.h: write inside the project, read the engine's
    // public API + docs, nothing else). Panels check it before loading any secondary file.
    virtual Access access(const std::string& path_abs) { (void)path_abs; return Access::Write; }
    // The engine's public API folders for read-only browsing: (module, absolute include dir).
    virtual std::vector<std::pair<std::string, std::string>> api_dirs() { return {}; }
    virtual std::string project_name() const { return ""; }
    virtual uint64_t ticks() const = 0;                    // 60 Hz animation clock
    virtual void toast(const std::string& msg, Toast t = Toast::Info) = 0;
    // A modal dialog: `body` is called every frame with the content rect until it returns false.
    virtual void modal(const std::string& title, int w, int h, std::function<bool(twk::Gui&, Rect)> body) = 0;
    virtual bool modal_open() const = 0;
    // Open a file in the matching editor (Studio: a new tab; standalone: may refuse).
    virtual void open_file(const std::string& path_abs, int line = 0, int col = 0) { (void)path_abs; (void)line; (void)col; }
    // Whether open_file() opens anything (the Studio: yes; a one-document solo shell: no).
    virtual bool can_open_files() const { return false; }
    // Open an image (a tileset) in the pixel editor, focused on tile `index` of a tw x th grid.
    virtual void open_tile(const std::string& path_abs, int index, int tw, int th) {
        (void)index; (void)tw; (void)th;
        open_file(path_abs);
    }
    // The placeable spawn types (prefab tables' name columns + defaults), for the map editor.
    virtual std::vector<std::string> prefab_types() { return {}; }
    // Paths (relative to root) of every file with one of the extensions (for pickers).
    virtual std::vector<std::string> files_with(const std::vector<std::string>& exts) { (void)exts; return {}; }
    // Run a make target in the job runner (Studio only). Returns false when unsupported.
    virtual bool run_make(const std::string& target) { (void)target; return false; }
    // Play the project with the player starting at map pixel (x, y) (the map editor's "Play from
    // here"): the project's Play launch, with PHX_PLAY_FROM set. False when unsupported.
    virtual bool play_from(int x, int y) { (void)x; (void)y; return false; }
    // Tell the host the file on disk changed (so explorers/bundles refresh).
    virtual void file_saved(const std::string& path_abs) { (void)path_abs; }
    // Compiler diagnostics from the last build that point into this file.
    virtual std::vector<Diag> diagnostics_for(const std::string& path_abs) { (void)path_abs; return {}; }
    // Audition mono 16-bit PCM through the Studio's mixer (the SFX and song editors); the host
    // keeps its own copy alive while it plays. A new call replaces what plays. False when there is
    // no audio device.
    virtual bool play_pcm(const std::vector<int16_t>& pcm, uint32_t rate, bool loop = false) {
        (void)pcm; (void)rate; (void)loop;
        return false;
    }
    virtual void stop_pcm() {}
};

class DocView {
public:
    virtual ~DocView() = default;
    std::string path;                                      // absolute
    virtual FileKind kind() const = 0;
    virtual int icon() const = 0;
    virtual bool dirty() const = 0;
    virtual bool save(Host&, std::string* err) = 0;
    virtual void draw(Host&, const Rect& area) = 0;
    virtual bool undo() { return false; }
    virtual bool redo() { return false; }
    virtual std::string status() const { return ""; }      // right side of the status bar
    virtual void release(Host&) {}                         // free textures (tab closed)
    virtual void goto_line(int line, int col) { (void)line; (void)col; }
    // Image editors: select tile `index` of a tw x th grid and zoom onto it (the map editor's
    // "Edit tile").
    virtual void focus_tile(int index, int tw, int th) { (void)index; (void)tw; (void)th; }
    virtual bool reload(Host&, std::string* err) { (void)err; return false; }
    // Extra entries for the host's Edit menu (label, shortcut, action).
    struct Action { std::string label, shortcut; std::function<void()> run; bool enabled = true; };
    virtual std::vector<Action> actions() { return {}; }
    // mtime of the file when it was last loaded/saved (external-change detection).
    int64_t disk_stamp = 0;
};

inline int64_t file_stamp(const std::string& path) {
    std::error_code ec;
    const auto t = pfs::last_write_time(path, ec);
    if (ec) return 0;
    return int64_t(t.time_since_epoch().count());
}

// Factories (one per editor TU). Each returns nullptr and sets `err` when the file can't load.
std::unique_ptr<DocView> make_code_view(Host&, const std::string& path, std::string* err, bool read_only = false);
std::unique_ptr<DocView> make_sprite_view(Host&, const std::string& path, std::string* err);
std::unique_ptr<DocView> make_map_view(Host&, const std::string& path, std::string* err);
std::unique_ptr<DocView> make_table_view(Host&, const std::string& path, std::string* err);
std::unique_ptr<DocView> make_sfx_view(Host&, const std::string& path, std::string* err);
std::unique_ptr<DocView> make_song_view(Host&, const std::string& path, std::string* err);
std::unique_ptr<DocView> make_font_view(Host&, const std::string& path, std::string* err);
std::unique_ptr<DocView> make_dialogue_view(Host&, const std::string& path, std::string* err);
// A view over a document that exists only in memory yet (saved to `path` on the first save).
std::unique_ptr<DocView> make_map_view_new(Host&, const std::string& path, const phxtool::TmapDoc& doc);
std::unique_ptr<DocView> make_table_view_new(Host&, const std::string& path, const phxtool::BinDoc& doc);

// The right editor for a path (by kind); "as text" forces the code editor. A read-only file (the
// engine's public API, its docs) opens only in the code editor, in its read-only mode.
inline std::unique_ptr<DocView> open_view(Host& h, const std::string& path, bool as_text, std::string* err,
                                          bool read_only = false) {
    const std::string e = lower_ext(path);
    FileKind k = kind_for(path, e == ".json" ? read_head(path, 4096) : "");
    if (read_only) {
        if (k != FileKind::Code && k != FileKind::Text) { if (err) *err = base_name(path) + " is read-only"; return nullptr; }
        return make_code_view(h, path, err, true);
    }
    if (as_text) k = FileKind::Code;
    switch (k) {
    case FileKind::Image: case FileKind::Sprite:
        if (e == ".ppm") break;
        return make_sprite_view(h, path, err);
    case FileKind::Map:    return make_map_view(h, path, err);
    case FileKind::Table:  return make_table_view(h, path, err);
    case FileKind::Sfx:    return make_sfx_view(h, path, err);
    case FileKind::Song:   return make_song_view(h, path, err);
    case FileKind::Font:   return make_font_view(h, path, err);
    case FileKind::Dialogue: return make_dialogue_view(h, path, err);
    case FileKind::Code: case FileKind::Text: case FileKind::Other: return make_code_view(h, path, err);
    default: break;
    }
    if (err) *err = std::string("no editor for ") + kind_name(k) + " files (" + base_name(path) + ")";
    return nullptr;
}

inline int kind_icon(FileKind k) {
    switch (k) {
    case FileKind::Dir: return twk::kIconFolder;       case FileKind::Code: return twk::kIconFileCode;
    case FileKind::Image: return twk::kIconFileImage;  case FileKind::Sprite: return twk::kIconSprite;
    case FileKind::Map: return twk::kIconFileMap;      case FileKind::Table: return twk::kIconFileTable;
    case FileKind::Sound: return twk::kIconFileSound;  case FileKind::Bundle: return twk::kIconLayers;
    case FileKind::Sfx: return twk::kIconFileSound;    case FileKind::Song: return twk::kIconFileSound;
    case FileKind::Font: return twk::kIconFileImage;
    case FileKind::Dialogue: return twk::kIconFileCode;
    default: return twk::kIconFile;
    }
}
inline Rgba kind_colour(const twk::Theme& th, FileKind k) {
    switch (k) {
    case FileKind::Dir: return th.warn;     case FileKind::Code: return th.info;   case FileKind::Image: return th.violet;
    case FileKind::Sprite: return th.accent; case FileKind::Map: return th.good;    case FileKind::Table: return rgba(96, 212, 220);
    case FileKind::Sound: return th.violet; case FileKind::Bundle: return th.dim;
    case FileKind::Sfx: return th.violet;   case FileKind::Song: return th.violet;
    case FileKind::Font: return th.accent;  case FileKind::Dialogue: return th.good;
    default: return th.faint;
    }
}

// ---- a zoom/pan canvas (sprite + map editors) ----
// Maps between screen pixels and content pixels at an integer zoom (1..64), with the content's
// origin at (ox, oy) in screen space. Pans with the middle button / space+drag / arrow keys, zooms
// about the pointer with Ctrl+wheel (plain wheel scrolls, Shift+wheel scrolls sideways).
struct Canvas {
    int zoom = 4;
    int ox = 0, oy = 0;          // screen position of content (0,0)
    bool fitted = false;

    int to_cx(int sx) const { return floordiv(sx - ox, zoom); }
    int to_cy(int sy) const { return floordiv(sy - oy, zoom); }
    int to_sx(int cx) const { return ox + cx * zoom; }
    int to_sy(int cy) const { return oy + cy * zoom; }
    static int floordiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

    void fit(const Rect& area, int cw, int ch, int max_zoom = 16) {
        int z = 1;
        while (z < max_zoom && cw * (z + 1) <= area.w - 16 && ch * (z + 1) <= area.h - 16) ++z;
        zoom = z;
        ox = area.x + (area.w - cw * z) / 2;
        oy = area.y + (area.h - ch * z) / 2;
        fitted = true;
    }
    // Keep the content point under (sx, sy) fixed while the zoom changes.
    void zoom_at(int sx, int sy, int nz) {
        nz = std::max(1, std::min(64, nz));
        if (nz == zoom) return;
        ox = sx - int(int64_t(sx - ox) * nz / zoom);
        oy = sy - int(int64_t(sy - oy) * nz / zoom);
        zoom = nz;
    }
    static int step_zoom(int z, int dir) {
        static const int k[] = { 1, 2, 3, 4, 5, 6, 8, 10, 12, 16, 20, 24, 32, 48, 64 };
        const int n = int(sizeof(k) / sizeof(k[0]));
        if (dir > 0) { for (int i = 0; i < n; ++i) if (k[i] > z) return k[i]; return 64; }
        for (int i = n - 1; i >= 0; --i) if (k[i] < z) return k[i];
        return 1;
    }
    // Standard navigation. Returns true when the view changed. `space` = the pan modifier held.
    bool navigate(twk::Gui& g, const Rect& area, uint32_t pan_id, bool space_held) {
        bool changed = false;
        if (g.hover(area)) {
            const bool ctrl = (g.in->mods & (twk::kCtrl | twk::kGui)) != 0;
            if (ctrl) {
                if (const int w = g.wheel(area)) { zoom_at(g.mx(), g.my(), step_zoom(zoom, w > 0 ? 1 : -1)); changed = true; }
            } else if (g.in->mods & twk::kShift) {
                if (const int w = g.wheel(area)) { ox += w * 24; changed = true; }
            } else {
                if (const int w = g.wheel(area)) { oy += w * 24; changed = true; }
                if (const int w = g.wheel_x(area)) { ox -= w * 24; changed = true; }
            }
        }
        const uint32_t btn = space_held ? twk::kMouseL : twk::kMouseM;
        if (g.drag(pan_id, area, btn)) {
            if (g.just_pressed(pan_id)) { pan_x0_ = ox; pan_y0_ = oy; }
            ox = pan_x0_ + g.drag_dx(); oy = pan_y0_ + g.drag_dy();
            g.cursor = PHX_CURSOR_SIZE_ALL;
            changed = true;
        }
        return changed;
    }

private:
    int pan_x0_ = 0, pan_y0_ = 0;
};

// Checkerboard backdrop for transparent pixels (two greys, `cell` px squares, screen-anchored).
inline void checkerboard(twk::Gui& g, const Rect& r, int cell = 8, uint8_t sub = twk::kSubWidget) {
    g.rect(r, rgba(44, 44, 58), sub);
    const Rect c = twk::intersect(r, g.clip());
    if (c.empty()) return;
    auto fl = [](int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); };
    for (int cy = fl(c.y, cell); cy * cell < c.bottom(); ++cy)
        for (int cx = fl(c.x, cell); cx * cell < c.right(); ++cx)
            if (((cx + cy) & 1) == 0)                   // screen-anchored squares, clipped to r
                g.rect(twk::intersect(Rect{ cx * cell, cy * cell, cell, cell }, c), rgba(56, 56, 72), uint8_t(sub + 1));
}

// A titled side-panel section; returns the body below the title.
inline Rect panel_section(twk::Gui& g, Rect& col, int h, const std::string& title) {
    Rect r = twk::cut_top(col, h);
    g.rect(r, g.th.panel, twk::kSubFill);
    g.text(r.x + 5, r.y + 4, title, g.th.dim);
    g.rect(Rect{ r.x + 5 + twk::Gui::text_w(title) + 4, r.y + 8, std::max(0, r.w - twk::Gui::text_w(title) - 14), 1 }, g.th.line, twk::kSubWidget);
    twk::cut_top(col, 2);
    return Rect{ r.x + 4, r.y + 15, r.w - 8, r.h - 18 };
}

} // namespace phxstudio
#endif // PHX_TOOLS_PHXSTUDIO_HOST_H
