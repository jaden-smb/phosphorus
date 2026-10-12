// engine/runtime/src/flow.cpp — see phx/runtime/flow.h. Linked by game projects and the flow
// suite. Portable (every target): text is phx::UI sprites from the font sheet.
#include "phx/runtime/flow.h"
#include "phx/runtime/app.h"
#include "phx/runtime/audio.h"
#include "phx/runtime/font.h"
#include "phx/core/log.h"

#include <cstring>

namespace phx {
namespace {
constexpr Rgba kText = rgba(240, 240, 250), kDim = rgba(170, 170, 190), kAccent = rgba(255, 190, 80);

// Decimal text, including INT64_MIN (no printf or allocation on the frame path).
template <size_t N>
void int_text(int64_t v, char (&out)[N]) {
    static_assert(N >= 21, "decimal int64 needs 20 characters and a terminator");
    char tmp[20];
    int n = 0;
    uint64_t u = v < 0 ? uint64_t(-(v + 1)) + 1u : uint64_t(v);
    do { tmp[n++] = char('0' + u % 10); u /= 10; } while (u);
    int o = 0;
    if (v < 0) out[o++] = '-';
    while (n) out[o++] = tmp[--n];
    out[o] = 0;
}

// Keep the complete number and separator; truncate only the label's tail, by byte.
template <size_t N>
void hud_text(char (&out)[N], const char* label, int64_t value) {
    static_assert(N >= 22, "HUD needs room for a separator, int64 and terminator");
    char num[21];
    int_text(value, num);
    const size_t digits = std::strlen(num);
    const size_t label_limit = N - 2 - digits;
    size_t n = 0;
    while (n < label_limit && label[n]) { out[n] = label[n]; ++n; }
    out[n++] = ' ';
    std::memcpy(out + n, num, digits + 1);
}
} // namespace

Status GameFlow::start(App& app, ResourceCache& res, const FlowOptions& opt) {
    res_ = &res; opt_ = opt;
    if (!TableView::load(res, opt.table, table_) || table_.count() == 0) {
        PHX_LOG_ERROR("flow: no flow table in the bundle (assets/flow.json)");
        return Status::NotFound;
    }
    font_ = BitmapFont{};
    if (!load_font(app.render(), res, opt.font, font_))
        PHX_LOG_WARN("flow: no font (assets/font.font or font.png): screens show no text");
    for (uint32_t i = 0; i < kMaxTotals; ++i) { total_names_[i] = 0; total_vals_[i] = 0; }
    vars_.f = this;
    dlg_.load(app.render(), res, opt.dialogue);           // optional: a game may have no dialogue
    enter(app, 0);
    return Status::Ok;
}

NameHash GameFlow::screen() const { return row_ >= 0 ? table_.get_hash(row_, "name"_hash) : 0; }

int32_t GameFlow::find_row(NameHash name) const { return name ? table_.find("name"_hash, name) : -1; }

int32_t GameFlow::next_row(int32_t row) const {
    const int32_t named = find_row(table_.get_hash(row, "next"_hash));
    if (named >= 0) return named;
    return row + 1 < int32_t(table_.count()) ? row + 1 : 0;
}

int32_t& GameFlow::total_slot(NameHash name) {
    for (uint32_t i = 0; i < kMaxTotals; ++i) if (total_names_[i] == name) return total_vals_[i];
    for (uint32_t i = 0; i < kMaxTotals; ++i)
        if (!total_names_[i]) { total_names_[i] = name; total_vals_[i] = 0; return total_vals_[i]; }
    return total_scratch_;
}

int32_t GameFlow::total(NameHash counter) const {
    int32_t t = 0;
    for (uint32_t i = 0; i < kMaxTotals; ++i) if (total_names_[i] == counter) t = total_vals_[i];
    return t + (in_level_ ? beh_.counter(counter) : 0);
}

bool GameFlow::go(App& app, NameHash name) {
    const int32_t r = find_row(name);
    if (r < 0) return false;
    if (in_level_) leave_level(app, false);
    enter(app, r);
    return true;
}

void GameFlow::enter(App& app, int32_t row) {
    row_ = row; ticks_ = 0; paused_ = false; in_level_ = false;
    if (const NameHash m = table_.get_hash(row, "music"_hash))
        if (auto s = res_->sound(m)) {
            const int32_t vol = table_.get_int(row, "music_vol"_hash, 60);
            app.audio().play_music(to_sound(s.unwrap()), float(vol < 0 ? 0 : vol > 100 ? 100 : vol) / 100.0f);
        }
    dlg_.stop();
    if (table_.get_hash(row, "kind"_hash) == "talk"_hash) {   // a cutscene: its conversation
        const NameHash c = table_.get_hash(row, "dialogue"_hash);
        dlg_.start(c ? c : table_.get_hash(row, "name"_hash), vars_);
        return;
    }
    if (table_.get_hash(row, "kind"_hash) != "level"_hash) return;
    const NameHash map = table_.get_hash(row, "map"_hash);
    LevelOptions lo;
    lo.map = map ? map : NameHash("level"_hash);
    if (level_.load(app, *res_, &physics_, lo) != Status::Ok) {
        PHX_LOG_ERROR("flow: screen %d's map is not in the bundle", int(row));
        return;
    }
    physics_.set_gravity(vec2{ s_from_int(0), s_from_int(opt_.gravity) });
    beh_.start(app, level_, *res_, physics_);
    in_level_ = true;
}

void GameFlow::leave_level(App& app, bool bank) {
    if (bank)
        for (uint32_t i = 0; i < Behaviours::kMaxCounters; ++i)
            if (const NameHash n = beh_.counter_name(i)) total_slot(n) += beh_.counter_value(i);
    level_.unload(app);
    in_level_ = false;
}

void GameFlow::update(App& app, scalar dt) {
    if (row_ < 0) return;
    ++ticks_;
    const InputState& in = app.input();
    if (dlg_.active()) {                                    // a conversation: everything else waits
        dlg_.update(in, vars_);
        if (!dlg_.active() && !in_level_ && table_.get_hash(row_, "kind"_hash) == "talk"_hash)
            enter(app, next_row(row_));
        return;
    }
    if (!in_level_) {                                       // a title / end screen (or a failed level)
        if (ticks_ > 10 && (in.just(Button::Start) || in.just(Button::A))) {
            if (table_.get_hash(row_, "kind"_hash) == "end"_hash) {
                for (uint32_t i = 0; i < kMaxTotals; ++i) { total_names_[i] = 0; total_vals_[i] = 0; }
                enter(app, 0);
            } else {
                enter(app, next_row(row_));
            }
        }
        return;
    }
    if (in.just(Button::Start)) paused_ = !paused_;
    if (paused_) return;
    beh_.update(app, dt);
    const int32_t lives = table_.get_int(row_, "lives"_hash, 0);
    if (lives > 0 && beh_.counter("deaths"_hash) >= lives) {
        leave_level(app, false);
        const int32_t over = find_row("gameover"_hash);
        enter(app, over >= 0 ? over : row_);                // no game-over screen: try again
        return;
    }
    if (const NameHash t = beh_.talk()) {                   // the player talked to a Talk
        beh_.clear_talk();
        dlg_.start(t, vars_);
        return;
    }
    if (const NameHash ex = beh_.exit()) {
        beh_.clear_exit();
        const int32_t from = row_;
        leave_level(app, true);
        const int32_t named = find_row(ex);
        enter(app, named >= 0 ? named : next_row(from));
    }
}

// Lines of `s` ('|' breaks a line), from y down, centred or at the left margin.
void GameFlow::text_lines(App& app, const char* s, int y, bool centred) {
    if (!s || font_.tex == kNoTexture) return;
    const Camera2D cam = in_level_ ? beh_.camera() : Camera2D{};
    const int w = app.config().width;
    char line[72];
    while (*s) {
        int n = 0;
        while (n < 71 && s[n] && s[n] != '|') { line[n] = s[n]; ++n; }
        line[n] = 0;
        const int x = centred ? (w - UI::text_width(font_, line)) / 2 : 4;
        ui_.text(vec2{ cam.pos.x + s_from_int(x), cam.pos.y + s_from_int(y) }, font_, line, kText);
        y += font_.line_h;
        s += n;
        if (*s == '|') ++s;
    }
}

void GameFlow::render(App& app) {
    Renderer& r = app.render();
    const Camera2D cam = in_level_ ? beh_.camera() : Camera2D{};
    r.begin_frame(cam);
    if (in_level_) {
        level_.draw(r);
        draw_sprites(app.world(), r);
    }
    ui_.begin(r, app.input());
    const int w = app.config().width, h = app.config().height;
    auto at = [&](int x, int y) { return vec2{ cam.pos.x + s_from_int(x), cam.pos.y + s_from_int(y) }; };
    if (row_ >= 0 && font_.tex != kNoTexture) {
        if (in_level_) {
            // HUD: the counter, and the lives left
            const NameHash cn = table_.get_hash(row_, "counter"_hash);
            const char* label = table_.get_str(row_, "label"_hash);
            char buf[40];
            hud_text(buf, label && *label ? label : "COINS", total(cn ? cn : NameHash("coins"_hash)));
            ui_.text(at(4, 4), font_, buf, kAccent);
            if (const int32_t lives = table_.get_int(row_, "lives"_hash, 0)) {
                hud_text(buf, "LIVES", int64_t(lives) - beh_.counter("deaths"_hash));
                ui_.text(at(w - 4 - UI::text_width(font_, buf), 4), font_, buf, kAccent);
            }
            if (ticks_ < 120) text_lines(app, table_.get_str(row_, "text"_hash), h / 3, true);   // the banner
            if (paused_) ui_.text(at((w - UI::text_width(font_, "PAUSED")) / 2, h / 2 - 4), font_, "PAUSED", kText);
            if (beh_.can_talk() && !dlg_.active() && !paused_)
                ui_.text(at((w - UI::text_width(font_, "UP: TALK")) / 2, h - 12), font_, "UP: TALK", kDim);
        } else {
            const bool talk = table_.get_hash(row_, "kind"_hash) == "talk"_hash;
            text_lines(app, table_.get_str(row_, "text"_hash), talk ? 16 : h / 3, true);
            if (!dlg_.active() && (ticks_ / 30) % 2 == 0)
                ui_.text(at((w - UI::text_width(font_, "PRESS START")) / 2, h - 24), font_, "PRESS START", kDim);
        }
    }
    if (dlg_.active()) dlg_.render(ui_, font_, w, h, cam.pos);
    ui_.end();
    r.end_frame();
}

} // namespace phx
