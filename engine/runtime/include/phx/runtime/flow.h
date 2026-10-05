// phx/runtime/flow.h — the GAME FLOW as data: which screens a game has and how they chain — a
// title, the levels in order, a game over, an ending — plus the HUD and pause every level gets.
// It is a phxbin table (assets/flow.json, edited in Phosphorus Studio's table editor), one row per
// screen, read by column name:
//
//   | column  | type  | meaning                                                               |
//   |---------|-------|-----------------------------------------------------------------------|
//   | name    | str   | the screen's name (an Exit's `target` or another row's `next` names it)|
//   | kind    | str   | "title", "level", "talk" or "end" (an end screen restarts the game)    |
//   | map     | str   | level: the tilemap asset (its spawns + the prefab table build it)      |
//   | text    | str   | title / end: the lines to show ('|' breaks a line); level: a banner    |
//   | next    | str   | the screen after this one (default: the next row)                      |
//   | lives   | int   | level: deaths before "gameover" (0 = no limit)                         |
//   | counter | str   | level: the counter the HUD shows (default "coins"); `label` its caption |
//   | music   | str   | a sound asset to loop from this screen on (empty: keep what plays):    |
//   |         |       | a .song from the Studio's song editor, or any sound                   |
//   | music_vol | int | its volume in percent (default 60: headroom for the sound effects)     |
//   | dialogue| str   | talk: the conversation it plays (default: the row's name), then `next`  |
//
// Flow: the first row starts. START leaves a title (and pauses / resumes a level). A level ends
// when the player touches an Exit (to its `target` screen if one is named so, else `next`), or
// runs out of lives (to the screen called "gameover", else the level restarts). An end screen's
// START goes back to the first row and clears the totals. Counters add up across levels
// (total()). DIALOGUE (phx/runtime/dialogue.h): the conversations in FlowOptions::dialogue play in
// a box along the bottom — a "talk" screen (a cutscene) plays its `dialogue`, and in a level a Talk
// component (the player touches it and presses Up) plays its conversation while the level waits.
// A conversation's variables are these totals, so `if coins >= 5` / `do coins -= 5` / `do key = 1`
// read and change what the HUD counts and last until the game restarts.
// Text uses the font FlowOptions::font (phx/runtime/font.h: a baked `.font`, which may
// be proportional, or a plain 16-column 8x8 ASCII texture), drawn with phx::UI, so it shows on
// every target.
//
//     flow.start(app, *res);        // on_start, after mounting the bundle
//     flow.update(app, dt);         // on_fixed_update
//     flow.render(app);             // on_render
#ifndef PHX_RUNTIME_FLOW_H
#define PHX_RUNTIME_FLOW_H

#include "phx/runtime/behaviours.h"
#include "phx/runtime/dialogue.h"
#include "phx/ui/ui.h"

namespace phx {

struct FlowOptions {
    NameHash table   = "flow"_hash;          // the flow table asset
    NameHash font    = "font"_hash;          // the font (a Font asset, else a texture of that name)
    int32_t  gravity = 420;                  // px/s², for every level
    NameHash dialogue = "dialogue"_hash;     // the conversations (a .dlg; optional)
};

class GameFlow {
public:
    static constexpr uint32_t kMaxTotals = 8;

    // Read the flow table and enter its first screen. NotFound: no flow table in the bundle.
    Status start(App& app, ResourceCache& res, const FlowOptions& opt = {});
    void   update(App& app, scalar dt);
    void   render(App& app);

    // Leave for the screen named `name` (a flow row); false when there is none.
    bool   go(App& app, NameHash name);

    int32_t     row() const { return row_; }                    // the current flow row
    NameHash    screen() const;                                 // its name hash
    bool        in_level() const { return in_level_; }
    bool        paused() const { return paused_; }
    // A counter summed over the finished levels plus the current one ("coins"_hash, "deaths"_hash).
    int32_t     total(NameHash counter) const;
    const TableView& table() const { return table_; }
    Level&      level()      { return level_; }
    Behaviours& behaviours() { return beh_; }
    PhysicsWorld& physics()  { return physics_; }
    DialogueRunner& dialogue() { return dlg_; }
    DialogueVars&   vars()     { return vars_; }       // the totals, as a conversation sees them

private:
    void enter(App& app, int32_t row);
    void leave_level(App& app, bool bank);   // bank: add the level's counters to the totals
    int32_t find_row(NameHash name) const;
    int32_t next_row(int32_t row) const;
    void    text_lines(App& app, const char* s, int y, bool centred);
    int32_t& total_slot(NameHash name);

    // A conversation's variables: the totals (a level's live counters included).
    struct FlowVars final : DialogueVars {
        GameFlow* f = nullptr;
        int32_t get(NameHash v) const override { return f->total(v); }
        void set(NameHash v, int32_t value) override {
            f->total_slot(v) = value - (f->in_level_ ? f->beh_.counter(v) : 0);
        }
    };

    ResourceCache* res_ = nullptr;
    FlowOptions    opt_{};
    TableView      table_{};
    Level          level_;
    PhysicsWorld   physics_;
    Behaviours     beh_;
    UI             ui_;
    BitmapFont     font_{};
    DialogueRunner dlg_;
    FlowVars       vars_;
    int32_t        row_ = -1;
    bool           in_level_ = false;
    bool           paused_ = false;
    uint32_t       ticks_ = 0;               // fixed steps on this screen (blinks, banners)
    NameHash       total_names_[kMaxTotals]{};
    int32_t        total_vals_[kMaxTotals]{};
    int32_t        total_scratch_ = 0;
};

} // namespace phx
#endif // PHX_RUNTIME_FLOW_H
