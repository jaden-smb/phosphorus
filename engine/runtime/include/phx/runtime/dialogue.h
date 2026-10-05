// phx/runtime/dialogue.h — conversations, run from data. A `.dlg` (Phosphorus Studio's dialogue
// editor; baked to a Dialogue asset) holds named conversations: each NODE is a speaker's line that
// leads on to the `next` node or offers CHOICES, and nodes / choices can be gated by a condition
// (`coins >= 5`) and change variables (`key = 1`, `coins -= 5`). The runner plays one:
//
//     dlg.load(app.render(), *res, "dialogue"_hash);          // on_start (portraits upload here)
//     dlg.start("sign"_hash, vars);                            // e.g. when the player talks
//     if (dlg.active()) dlg.update(app.input(), vars);         // on_fixed_update (A / Up / Down)
//     dlg.render(ui, font, w, h, origin);                      // on_render, between ui.begin/end
//
// Text types out (chars_per_sec); A shows the rest at once, then goes on. With choices, Up/Down
// pick and A confirms. A node whose condition is false is skipped (the conversation goes on at its
// `next`); a choice whose condition is false is hidden. Variables live wherever the game keeps its
// numbers: a DialogueVars (the game flow's counter totals, so `coins` is the coins collected).
// Fixed-step and integer: the same conversation reveals the same character on the same tick on
// every tier.
#ifndef PHX_RUNTIME_DIALOGUE_H
#define PHX_RUNTIME_DIALOGUE_H

#include "phx/input/input.h"
#include "phx/render/renderer.h"
#include "phx/resource/cache.h"
#include "phx/ui/ui.h"

namespace phx {

// Where a conversation reads and writes its variables.
class DialogueVars {
public:
    virtual int32_t get(NameHash var) const = 0;
    virtual void    set(NameHash var, int32_t value) = 0;
protected:
    ~DialogueVars() = default;
};

// A small standalone store (16 variables; unknown ones read 0), for games without a GameFlow.
class DialogueVarTable final : public DialogueVars {
public:
    static constexpr uint32_t kMax = 16;
    int32_t get(NameHash var) const override {
        for (uint32_t i = 0; i < kMax; ++i) if (names_[i] == var) return vals_[i];
        return 0;
    }
    void set(NameHash var, int32_t v) override {
        for (uint32_t i = 0; i < kMax; ++i) if (names_[i] == var) { vals_[i] = v; return; }
        for (uint32_t i = 0; i < kMax; ++i) if (!names_[i]) { names_[i] = var; vals_[i] = v; return; }
    }
    void clear() { for (uint32_t i = 0; i < kMax; ++i) { names_[i] = 0; vals_[i] = 0; } }
private:
    NameHash names_[kMax]{};
    int32_t  vals_[kMax]{};
};

class DialogueRunner {
public:
    static constexpr uint32_t kMaxSpeakers = 16;   // portraits uploaded per dialogue asset
    static constexpr uint32_t kMaxChoices  = 8;    // choices shown at once

    uint8_t chars_per_sec = 40;                    // typewriter speed (0 = whole lines at once)

    // Mount the dialogue asset `name` and upload its speakers' portraits. False when it is not in
    // the bundle (then start() does nothing).
    bool load(Renderer& r, ResourceCache& res, NameHash name);
    bool loaded() const { return data_.nodes != nullptr; }
    bool has(NameHash conversation) const { return data_.find(conversation) >= 0; }

    // Begin a conversation (its entry node, skipping nodes whose condition fails). False when there
    // is no such conversation or every node is skipped.
    bool start(NameHash conversation, DialogueVars& vars);
    void stop() { node_ = kDlgEnd; }
    bool active() const { return node_ != kDlgEnd; }

    // One fixed step: reveal more text; A completes the line, then advances (or confirms the
    // highlighted choice); Up/Down move the highlight.
    void update(const InputState& in, DialogueVars& vars);

    // The box along the bottom of a w x h screen whose top-left is `origin` (the camera position
    // in a level): the portrait, the speaker's name, the line, and the choices once it has shown.
    void render(UI& ui, const BitmapFont& font, int w, int h, vec2 origin = vec2{}) const;

    // The state, for games and tests.
    uint16_t    node() const { return node_; }
    const char* text() const;
    const char* speaker() const;                   // "" when the node has no speaker
    bool        revealed() const;                  // the whole line is showing
    uint32_t    choice_count() const { return nvis_; }   // the visible choices (after revealed())
    const char* choice(uint32_t i) const;
    uint32_t    selected() const { return sel_; }
    uint32_t    lines_shown() const { return shown_; }   // nodes shown since start()

private:
    void enter(uint16_t node, DialogueVars& vars);   // show `node` (or the first one after it that passes)
    bool test(const DlgOp& op, const DialogueVars& vars) const;
    void apply(uint16_t first, uint8_t count, DialogueVars& vars) const;
    uint32_t text_len() const;

    DialogueData data_{};
    TextureId    portraits_[kMaxSpeakers]{};
    uint8_t      portrait_w_[kMaxSpeakers]{}, portrait_h_[kMaxSpeakers]{};
    uint16_t     node_ = kDlgEnd;
    uint32_t     ticks_ = 0;                        // fixed steps on this line
    uint16_t     vis_[kMaxChoices]{};               // the visible choices (choice indices)
    uint32_t     nvis_ = 0, sel_ = 0, shown_ = 0;
};

} // namespace phx
#endif // PHX_RUNTIME_DIALOGUE_H
