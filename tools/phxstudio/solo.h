// tools/phxstudio/solo.h — a ONE-document window around a Studio editor panel. phxtmap and
// phxentity are this shell hosting the Studio's own map / table panels (ed_map.cpp,
// ed_table.cpp), so there is exactly one map editor and one table editor in the tree: the same
// widgets, keys and document models whether you open a file in Phosphorus Studio or on its own.
//
// The shell is a phx::Game on the usual App loop + SDL window: a slim bar (file name, Save, Undo,
// Redo, the panel's actions, help), the panel, a status line, toasts, modal dialogs, and a
// confirm-on-close for unsaved work. Host-only; no SDL header (desktop.h + the widget kit).
#ifndef PHX_TOOLS_PHXSTUDIO_SOLO_H
#define PHX_TOOLS_PHXSTUDIO_SOLO_H

#include "phx/runtime/app.h"
#include "phx/render/renderer.h"
#include "phx/platform/desktop.h"

#include "host.h"

#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

extern "C" int phx_sdl_readback(uint32_t* out, int lw, int lh);   // SDL backend extension

namespace phxstudio {

// An absolute path for `p` (relative to the working directory).
inline std::string abs_path(const std::string& p) {
    std::error_code ec;
    const pfs::path a = pfs::absolute(p, ec);
    return ec ? p : a.lexically_normal().generic_string();
}
// The Phosphorus checkout that contains `start` (the directory with Makefile + engine/ + tools/),
// or "" when there is none — pickers then list files under the working directory.
inline std::string solo_repo_root(const std::string& start) {
    std::error_code ec;
    pfs::path d = pfs::absolute(start, ec);
    for (int i = 0; i < 32 && !d.empty(); ++i) {
        if (pfs::exists(d / "Makefile", ec) && pfs::exists(d / "engine", ec) && pfs::exists(d / "tools", ec))
            return d.lexically_normal().generic_string();
        if (d == d.parent_path()) break;
        d = d.parent_path();
    }
    return "";
}

class SoloShell final : public phx::Game, public Host {
public:
    using Factory = std::function<std::unique_ptr<DocView>(Host&, std::string* err)>;

    std::string app_name;                  // "phxtmap"
    std::string repo_root;                 // for pickers (the working directory by default)
    std::string help;                      // one line of keys for the status bar
    Factory factory;
    std::vector<std::string> types;        // prefab_types() answer (--types / --prefabs)
    std::string save_as;                   // --out: save somewhere other than the file opened
    std::string shot_path;                 // --shot: write the window as a PPM after `shot_frame`, then quit
    int shot_frame = 45;

    // ---- Game ----
    void on_start(phx::App& app) override {
        app_ = &app;
        r_ = &app.render();
        g_.init(*r_);
        phx_desktop_set_quit_on_escape(0);
        phx_desktop_set_confirm_quit(1);
        phx_desktop_set_resizable(1);
        std::string err;
        view_ = factory(*this, &err);
        if (!view_) {
            std::fprintf(stderr, "%s: %s\n", app_name.c_str(), err.c_str());
            app.request_quit();
            return;
        }
        if (!save_as.empty()) view_->path = save_as;
    }
    void on_fixed_update(phx::App&, phx::scalar) override { ++ticks_; }
    void on_stop(phx::App&) override { if (view_) view_->release(*this); }

    void on_render(phx::App& app, phx::scalar) override {
        phx::Renderer& r = app.render();
        twk::collect_desktop(in_);
        phx_desktop_fb_size(&W_, &H_);
        r.begin_frame(phx::Camera2D{});
        g_.begin(&r, &app.input(), in_, W_, H_);
        if (!view_) { g_.end(); r.end_frame(); return; }
        using twk::kCtrl; using twk::kShift;
        if (g_.key('s', kCtrl)) save();
        if (!g_.text_focus() && modals_.empty()) {
            if (g_.key('z', kCtrl)) view_->undo();
            if (g_.key('y', kCtrl) || g_.key('z', kCtrl | kShift)) view_->redo();
        }
        if (g_.key('q', kCtrl) || in_.quit) request_quit();

        const twk::Rect bar{ 0, 0, W_, 16 };
        const twk::Rect body{ 0, 16, W_, H_ - 16 - 12 };
        view_->draw(*this, body);
        draw_bar(bar);
        draw_status();
        draw_toasts();
        if (!modals_.empty()) {
            Modal m = modals_.back();
            const twk::Rect b = g_.begin_modal(m.title, std::min(m.w, W_ - 16), std::min(m.h, H_ - 16));
            const bool keep = m.body(g_, b);
            g_.end_modal();
            if (!keep) { modals_.pop_back(); g_.blur(); }
        }
        const std::string title = base_name(view_->path) + (view_->dirty() ? " *" : "") + " - " + app_name;
        if (title != title_) { phx_desktop_set_title(title.c_str()); title_ = title; }
        if (quitting_) app.request_quit();
        g_.end();
        r.end_frame();
        if (!shot_path.empty() && app.frame() >= uint64_t(shot_frame)) { write_shot(); app.request_quit(); }
    }

    // ---- Host ----
    twk::Gui& gui() override { return g_; }
    phx::Renderer& renderer() override { return *r_; }
    const std::string& root() const override { return repo_root; }
    uint64_t ticks() const override { return ticks_; }
    void toast(const std::string& msg, Toast t) override {
        toasts_.push_back(ToastMsg{ msg, t, ticks_ + 210 });
        if (toasts_.size() > 4) toasts_.erase(toasts_.begin());
        std::printf("%s: %s\n", app_name.c_str(), msg.c_str());
    }
    void modal(const std::string& title, int w, int h, std::function<bool(twk::Gui&, twk::Rect)> body) override {
        modals_.push_back(Modal{ title, w, h, std::move(body) });
    }
    bool modal_open() const override { return !modals_.empty(); }
    // A one-document window can't open another document (the map editor's "Edit tile"): say where.
    void open_file(const std::string& path_abs, int line, int col) override {
        (void)line; (void)col;
        toast("open " + base_name(path_abs) + " in Phosphorus Studio to edit it", Toast::Info);
    }
    std::vector<std::string> prefab_types() override { return types; }
    std::vector<std::string> files_with(const std::vector<std::string>& exts) override {
        if (files_.empty()) { FileTree t; t.root = repo_root; files_ = t.all_files(4000); }
        std::vector<std::string> out;
        for (const std::string& f : files_) for (const std::string& e : exts) if (lower_ext(f) == e) { out.push_back(f); break; }
        return out;
    }

private:
    struct Modal { std::string title; int w, h; std::function<bool(twk::Gui&, twk::Rect)> body; };
    struct ToastMsg { std::string text; Toast tone; uint64_t until; };

    phx::App* app_ = nullptr;
    phx::Renderer* r_ = nullptr;
    twk::Gui g_;
    twk::Input in_;
    int W_ = 640, H_ = 360;
    uint64_t ticks_ = 0;
    std::unique_ptr<DocView> view_;
    std::vector<Modal> modals_;
    std::vector<ToastMsg> toasts_;
    std::vector<std::string> files_;
    std::string title_;
    bool quitting_ = false;

    void write_shot() {
        std::vector<uint32_t> px(size_t(W_) * size_t(H_));
        if (phx_sdl_readback(px.data(), W_, H_) != 0) { std::fprintf(stderr, "%s: readback failed\n", app_name.c_str()); return; }
        FILE* f = std::fopen(shot_path.c_str(), "wb");
        if (!f) return;
        std::fprintf(f, "P6\n%d %d\n255\n", W_, H_);
        for (uint32_t c : px) { const uint8_t rgb[3] = { uint8_t(c), uint8_t(c >> 8), uint8_t(c >> 16) }; std::fwrite(rgb, 1, 3, f); }
        std::fclose(f);
    }
    void save() {
        std::string err;
        if (!view_->dirty() && pfs::exists(view_->path)) { toast("no changes to save", Toast::Info); return; }
        if (view_->save(*this, &err)) toast("saved " + view_->path, Toast::Good);
        else toast("save failed: " + err, Toast::Bad);
    }
    void request_quit() {
        if (!view_ || !view_->dirty()) { quitting_ = true; return; }
        if (!modals_.empty()) return;
        modal("Unsaved changes", 300, 78, [this](twk::Gui& g, twk::Rect body) {
            g.text(body.x, body.y + 2, "Save changes to " + base_name(view_->path) + "?", g.th.text, twk::kSubText, body.w);
            const int y = body.bottom() - 14;
            twk::Btn sb; sb.on = true;
            if (g.button(twk::Rect{ body.x, y, 84, 14 }, "Save & quit", sb) || g.key(PHX_KEY_ENTER)) {
                std::string err;
                if (view_->save(*this, &err)) quitting_ = true;
                else toast("save failed: " + err, Toast::Bad);
                return false;
            }
            if (g.button(twk::Rect{ body.x + 90, y, 84, 14 }, "Don't save")) { quitting_ = true; return false; }
            if (g.button(twk::Rect{ body.x + 180, y, 84, 14 }, "Cancel") || g.key(PHX_KEY_ESCAPE)) return false;
            return true;
        });
    }
    void draw_bar(const twk::Rect& r) {
        g_.rect(r, g_.th.bar, twk::kSubFill);
        g_.rect(twk::Rect{ r.x, r.bottom() - 1, r.w, 1 }, g_.th.line, twk::kSubWidget);
        twk::Gui::Row row(twk::Rect{ r.x + 4, r.y + 2, r.w - 8, 12 }, 2);
        g_.text(row.take(twk::Gui::text_w(app_name) + 6).x, r.y + 4, app_name, g_.th.accent);
        if (g_.icon_button(row.take(14), twk::kIconSave, "Save (Ctrl+S)", false, view_->dirty())) save();
        if (g_.icon_button(row.take(14), twk::kIconUndo, "Undo (Ctrl+Z)")) view_->undo();
        if (g_.icon_button(row.take(14), twk::kIconRedo, "Redo (Ctrl+Y)")) view_->redo();
        std::vector<DocView::Action> acts = view_->actions();
        if (!acts.empty()) {
            const uint32_t mid = g_.id("solo-actions");
            g_.menu_button(mid, row.take(44), "more", twk::kIconChevronDown);
            std::vector<twk::MenuItem> items;
            for (const auto& a : acts) items.push_back(twk::MenuItem{ a.label, a.shortcut, a.enabled });
            const int hit = g_.menu(mid, items, 170);
            if (hit >= 0 && size_t(hit) < acts.size()) acts[size_t(hit)].run();
        }
        const std::string nm = view_->path + (view_->dirty() ? "  (unsaved)" : "");
        const twk::Rect rest = row.rest();
        g_.text(rest.x + 6, r.y + 4, nm, view_->dirty() ? g_.th.accent : g_.th.dim, twk::kSubText, rest.w - 8);
    }
    void draw_status() {
        const int y = H_ - 12;
        g_.rect(twk::Rect{ 0, y, W_, 12 }, g_.th.bar, twk::kSubFill);
        g_.rect(twk::Rect{ 0, y, W_, 1 }, g_.th.line, twk::kSubWidget);
        const std::string right = view_->status();
        const int rw = std::min(W_ / 2, twk::Gui::text_w(right));
        g_.text(W_ - 6 - rw, y + 3, right, g_.th.dim, twk::kSubText, rw);
        g_.text(6, y + 3, g_.hint.empty() ? help : g_.hint, g_.hint.empty() ? g_.th.faint : g_.th.text, twk::kSubText, W_ - rw - 20);
    }
    void draw_toasts() {
        toasts_.erase(std::remove_if(toasts_.begin(), toasts_.end(), [&](const ToastMsg& t) { return t.until < ticks_; }), toasts_.end());
        int y = H_ - 16;
        for (size_t i = toasts_.size(); i-- > 0; ) {
            const ToastMsg& t = toasts_[i];
            const int w = std::min(W_ - 20, twk::Gui::text_w(t.text) + 16);
            const twk::Rect r{ W_ - w - 8, y - 15, w, 15 };
            const twk::Rgba c = t.tone == Toast::Good ? g_.th.good : t.tone == Toast::Warn ? g_.th.warn : t.tone == Toast::Bad ? g_.th.bad : g_.th.info;
            const int saved = g_.plane();
            g_.set_plane(twk::kPlanePopup);
            g_.rect(r, g_.th.bar, twk::kSubFill);
            g_.frame_rect(r, c, twk::kSubWidget);
            g_.text(r.x + 8, r.y + 4, t.text, g_.th.text, twk::kSubText, r.w - 10);
            g_.set_plane(saved);
            y -= 18;
        }
    }
};

// Boot a SoloShell in a resizable window of `w` x `h` logical pixels at `scale`.
inline int run_solo(SoloShell& shell, int w = 640, int h = 360, int scale = 2) {
    phx_desktop_set_scale(scale);
    phx::Config cfg = phx::Config::from_defaults();
    cfg.title = shell.app_name.c_str();
    cfg.width = w; cfg.height = h;
    cfg.sim_hz = 60; cfg.vsync = true;
    phx::App app(cfg);
    return app.run(&shell);
}

} // namespace phxstudio
#endif // PHX_TOOLS_PHXSTUDIO_SOLO_H
