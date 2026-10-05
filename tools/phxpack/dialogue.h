// tools/phxpack/dialogue.h — HOST-ONLY dialogue model: the `.dlg` author format (JSON), its
// validation, and the compiler to the baked Dialogue asset (phx/resource/bundle.h: DialogueHeader
// and friends; played by phx/runtime/dialogue.h). builders.h build_dialogue bakes it; Phosphorus
// Studio's dialogue editor edits it and plays it through DlgSim, which walks the COMPILED tables
// with the runtime's rules (the dialogue suite checks the two agree step for step).
//
// .dlg:
//   { "dialogue": 1,
//     "speakers": [ { "name": "Sage", "portrait": "sage_face" } ],        // portrait: a texture asset
//     "conversations": [
//       { "name": "sage", "nodes": [
//           { "id": "hi", "speaker": "Sage", "text": "Hello!", "next": "ask" },
//           { "id": "ask", "speaker": "Sage", "text": "A key for 5 coins?", "choices": [
//               { "text": "Buy", "if": "coins >= 5", "do": "coins -= 5, key = 1", "next": "sold" },
//               { "text": "No thanks", "next": "end" } ] },
//           { "id": "sold", "text": "Here you go.", "if": "key" } ] } ] }
//
//   next     a node id; "end" ends; empty = the node after this one (the last one ends)
//   if       `var op number` (== != < <= > >=), `var` (not 0) or `!var` (0). A node whose test
//            fails is skipped; a choice whose test fails is hidden.
//   do       effects, comma-separated: `var = n`, `var += n`, `var -= n`, or `var` (= 1)
//   speaker  a name (new names become speakers without a portrait)
#ifndef PHX_TOOLS_DIALOGUE_H
#define PHX_TOOLS_DIALOGUE_H

#include "json.h"
#include "phx/core/types.h"
#include "phx/resource/bundle.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace phxtool {

struct DlgChoice { std::string text, next, cond, effects; };
struct DlgNode {
    std::string id, speaker, text, next, cond, effects;
    std::vector<DlgChoice> choices;
};
struct DlgConversation { std::string name; std::vector<DlgNode> nodes; };
struct DlgSpeaker { std::string name, portrait; };

struct DlgDoc {
    std::vector<DlgSpeaker> speakers;
    std::vector<DlgConversation> convs;

    int find_conv(const std::string& n) const {
        for (size_t i = 0; i < convs.size(); ++i) if (convs[i].name == n) return int(i);
        return -1;
    }
    int find_speaker(const std::string& n) const {
        for (size_t i = 0; i < speakers.size(); ++i) if (speakers[i].name == n) return int(i);
        return -1;
    }
    std::string fresh_conv_name(const std::string& base = "talk") const {
        if (find_conv(base) < 0) return base;
        for (int k = 2;; ++k) if (find_conv(base + std::to_string(k)) < 0) return base + std::to_string(k);
    }
    // Rename a node id everywhere its conversation points at it.
    static void rename_node(DlgConversation& c, size_t i, const std::string& to) {
        if (i >= c.nodes.size()) return;
        const std::string from = c.nodes[i].id;
        c.nodes[i].id = to;
        if (from.empty()) return;
        for (DlgNode& n : c.nodes) {
            if (n.next == from) n.next = to;
            for (DlgChoice& ch : n.choices) if (ch.next == from) ch.next = to;
        }
    }
    static std::string fresh_node_id(const DlgConversation& c, const std::string& base = "n") {
        auto taken = [&](const std::string& id) { for (const DlgNode& n : c.nodes) if (n.id == id) return true; return false; };
        for (int k = int(c.nodes.size()) + 1;; ++k) if (!taken(base + std::to_string(k))) return base + std::to_string(k);
    }
};

// ---- expressions ----------------------------------------------------------------------------
inline bool dlg_ident(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) if (!(std::isalnum(uint8_t(c)) || c == '_')) return false;
    return true;
}
inline std::string dlg_trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}
inline bool dlg_int(const std::string& s, int& v) {
    const std::string t = dlg_trim(s);
    if (t.empty()) return false;
    char* end = nullptr;
    const long x = std::strtol(t.c_str(), &end, 10);
    if (!end || *end || x < -32768 || x > 32767) return false;
    v = int(x);
    return true;
}
// A condition -> a DlgOp (op kDlgNone for an empty string).
inline bool dlg_parse_cond(const std::string& text, phx::DlgOp& out, std::string* err = nullptr) {
    auto fail = [&](const std::string& why) { if (err) *err = why; return false; };
    out = phx::DlgOp{};
    const std::string s = dlg_trim(text);
    if (s.empty()) return true;
    static const struct { const char* sym; uint8_t op; } kOps[] = {
        { "==", phx::kDlgEq }, { "!=", phx::kDlgNe }, { "<=", phx::kDlgLe }, { ">=", phx::kDlgGe },
        { "<", phx::kDlgLt }, { ">", phx::kDlgGt } };
    for (const auto& o : kOps) {
        const size_t p = s.find(o.sym);
        if (p == std::string::npos) continue;
        const std::string var = dlg_trim(s.substr(0, p));
        int v = 0;
        if (!dlg_ident(var)) return fail("'" + var + "' is not a variable name");
        if (!dlg_int(s.substr(p + std::strlen(o.sym)), v)) return fail("'" + s + "': expected a whole number -32768..32767 after " + o.sym);
        out.var = phx::fnv1a(var.c_str()); out.op = o.op; out.value = int16_t(v);
        return true;
    }
    if (s[0] == '!' && dlg_ident(dlg_trim(s.substr(1)))) { out.var = phx::fnv1a(dlg_trim(s.substr(1)).c_str()); out.op = phx::kDlgEq; out.value = 0; return true; }
    if (dlg_ident(s)) { out.var = phx::fnv1a(s.c_str()); out.op = phx::kDlgNe; out.value = 0; return true; }
    return fail("'" + s + "' is not a condition (e.g. coins >= 5, key, !key)");
}
// Effects -> DlgOps.
inline bool dlg_parse_effects(const std::string& text, std::vector<phx::DlgOp>& out, std::string* err = nullptr) {
    auto fail = [&](const std::string& why) { if (err) *err = why; return false; };
    out.clear();
    size_t p = 0;
    while (p <= text.size()) {
        size_t e = text.find(',', p);
        if (e == std::string::npos) e = text.size();
        const std::string s = dlg_trim(text.substr(p, e - p));
        p = e + 1;
        if (s.empty()) continue;
        phx::DlgOp op{};
        size_t at = std::string::npos;
        if ((at = s.find("+=")) != std::string::npos) op.op = phx::kDlgAdd;
        else if ((at = s.find("-=")) != std::string::npos) op.op = phx::kDlgSub;
        else if ((at = s.find('=')) != std::string::npos) op.op = phx::kDlgSet;
        std::string var;
        int v = 1;
        if (at == std::string::npos) { var = s; op.op = phx::kDlgSet; }
        else {
            var = dlg_trim(s.substr(0, at));
            if (!dlg_int(s.substr(at + (op.op == phx::kDlgSet ? 1 : 2)), v)) return fail("'" + s + "': expected a whole number -32768..32767");
        }
        if (!dlg_ident(var)) return fail("'" + s + "' is not an effect (e.g. key = 1, coins -= 5)");
        op.var = phx::fnv1a(var.c_str()); op.value = int16_t(v);
        out.push_back(op);
    }
    return true;
}

// ---- JSON -----------------------------------------------------------------------------------
inline bool dlg_from_json(const std::string& text, DlgDoc& out, std::string* err = nullptr) {
    auto fail = [&](const std::string& why) { if (err) *err = why; return false; };
    JsonValue root;
    std::string jerr;
    if (!JsonParser::parse(text, root, &jerr)) return fail("malformed JSON: " + jerr);
    if (!root.is_obj()) return fail("top level is not a JSON object");
    out = DlgDoc{};
    if (const JsonValue* sp = root.find("speakers"); sp && sp->is_arr())
        for (const JsonValue& s : sp->arr) out.speakers.push_back(DlgSpeaker{ s.str_at("name"), s.str_at("portrait") });
    if (const JsonValue* cs = root.find("conversations"); cs && cs->is_arr())
        for (const JsonValue& c : cs->arr) {
            DlgConversation conv;
            conv.name = c.str_at("name");
            if (const JsonValue* ns = c.find("nodes"); ns && ns->is_arr())
                for (const JsonValue& n : ns->arr) {
                    DlgNode node{ n.str_at("id"), n.str_at("speaker"), n.str_at("text"), n.str_at("next"), n.str_at("if"), n.str_at("do"), {} };
                    if (const JsonValue* ch = n.find("choices"); ch && ch->is_arr())
                        for (const JsonValue& x : ch->arr)
                            node.choices.push_back(DlgChoice{ x.str_at("text"), x.str_at("next"), x.str_at("if"), x.str_at("do") });
                    conv.nodes.push_back(node);
                }
            out.convs.push_back(conv);
        }
    return true;
}

inline std::string dlg_json_str(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (c == '\n') o += "\\n";
        else o += c;
    }
    return o + "\"";
}
inline std::string dlg_to_json(const DlgDoc& d) {
    std::string o = "{ \"dialogue\": 1,\n  \"speakers\": [";
    for (size_t i = 0; i < d.speakers.size(); ++i) {
        o += i ? ",\n    " : "\n    ";
        o += "{ \"name\": " + dlg_json_str(d.speakers[i].name);
        if (!d.speakers[i].portrait.empty()) o += ", \"portrait\": " + dlg_json_str(d.speakers[i].portrait);
        o += " }";
    }
    o += d.speakers.empty() ? "],\n" : "\n  ],\n";
    o += "  \"conversations\": [";
    auto field = [](std::string& s, const char* k, const std::string& v) { if (!v.empty()) s += ", \"" + std::string(k) + "\": " + dlg_json_str(v); };
    for (size_t c = 0; c < d.convs.size(); ++c) {
        const DlgConversation& cv = d.convs[c];
        o += c ? ",\n    " : "\n    ";
        o += "{ \"name\": " + dlg_json_str(cv.name) + ", \"nodes\": [";
        for (size_t i = 0; i < cv.nodes.size(); ++i) {
            const DlgNode& n = cv.nodes[i];
            o += i ? ",\n        " : "\n        ";
            o += "{ \"id\": " + dlg_json_str(n.id);
            field(o, "speaker", n.speaker);
            o += ", \"text\": " + dlg_json_str(n.text);
            field(o, "next", n.next); field(o, "if", n.cond); field(o, "do", n.effects);
            if (!n.choices.empty()) {
                o += ", \"choices\": [";
                for (size_t k = 0; k < n.choices.size(); ++k) {
                    const DlgChoice& ch = n.choices[k];
                    o += k ? ",\n            " : "\n            ";
                    o += "{ \"text\": " + dlg_json_str(ch.text);
                    field(o, "next", ch.next); field(o, "if", ch.cond); field(o, "do", ch.effects);
                    o += " }";
                }
                o += " ]";
            }
            o += " }";
        }
        o += cv.nodes.empty() ? "] }" : "\n      ] }";
    }
    o += d.convs.empty() ? "]\n}\n" : "\n  ]\n}\n";
    return o;
}
inline bool is_dialogue_json(const std::string& head) {
    return head.find("\"dialogue\"") != std::string::npos && head.find("\"conversations\"") != std::string::npos;
}

// ---- validation -----------------------------------------------------------------------------
// Errors (the bake refuses) and warnings (it bakes: an unreachable node, a line with no text).
struct DlgProblem { bool error; int conv, node; std::string what; };

// Index of the node a `next` names in `c`, from node i: -1 = end, -2 = unknown id.
inline int dlg_resolve(const DlgConversation& c, size_t i, const std::string& next) {
    if (next == "end") return -1;
    if (next.empty()) return i + 1 < c.nodes.size() ? int(i + 1) : -1;
    for (size_t k = 0; k < c.nodes.size(); ++k) if (c.nodes[k].id == next) return int(k);
    return -2;
}

inline std::vector<DlgProblem> dlg_validate(const DlgDoc& d) {
    std::vector<DlgProblem> out;
    auto add = [&](bool e, int c, int n, const std::string& w) { out.push_back(DlgProblem{ e, c, n, w }); };
    for (size_t s = 0; s < d.speakers.size(); ++s)
        if (d.speakers[s].name.empty()) add(true, -1, -1, "a speaker has no name");
    for (size_t c = 0; c < d.convs.size(); ++c) {
        const DlgConversation& cv = d.convs[c];
        if (cv.name.empty()) add(true, int(c), -1, "a conversation has no name");
        else if (!dlg_ident(cv.name)) add(true, int(c), -1, "conversation '" + cv.name + "': use letters, digits and _ only");
        for (size_t k = 0; k < c; ++k) if (d.convs[k].name == cv.name && !cv.name.empty()) add(true, int(c), -1, "two conversations are named '" + cv.name + "'");
        if (cv.nodes.empty()) add(false, int(c), -1, "'" + cv.name + "' has no lines");
        std::vector<bool> reach(cv.nodes.size(), false);
        if (!cv.nodes.empty()) reach[0] = true;
        for (size_t i = 0; i < cv.nodes.size(); ++i) {
            const DlgNode& n = cv.nodes[i];
            const std::string where = "'" + cv.name + "' line " + std::to_string(i + 1) + (n.id.empty() ? "" : " (" + n.id + ")");
            for (size_t k = 0; k < i; ++k) if (!n.id.empty() && cv.nodes[k].id == n.id) add(true, int(c), int(i), where + ": the id is used twice");
            if (n.id == "end") add(true, int(c), int(i), where + ": 'end' is reserved");
            if (n.text.empty()) add(false, int(c), int(i), where + ": no text");
            std::string err;
            phx::DlgOp op{};
            std::vector<phx::DlgOp> ops;
            if (!dlg_parse_cond(n.cond, op, &err)) add(true, int(c), int(i), where + ": if " + err);
            if (!dlg_parse_effects(n.effects, ops, &err)) add(true, int(c), int(i), where + ": do " + err);
            const int nx = dlg_resolve(cv, i, n.next);
            if (nx == -2) add(true, int(c), int(i), where + ": next '" + n.next + "' is not a line id");
            if (n.choices.size() > 255) add(true, int(c), int(i), where + ": too many choices");
            if (n.choices.size() > 8) add(false, int(c), int(i), where + ": more than 8 choices (the runtime shows 8)");
            // what this node leads to (a node with choices leads through them; else to next)
            if (n.choices.empty()) { if (nx >= 0) reach[size_t(nx)] = true; }
            else if (!n.cond.empty() && nx >= 0) reach[size_t(nx)] = true;          // skipped -> next
            for (size_t k = 0; k < n.choices.size(); ++k) {
                const DlgChoice& ch = n.choices[k];
                const std::string cw = where + " choice " + std::to_string(k + 1);
                if (ch.text.empty()) add(true, int(c), int(i), cw + ": no text");
                if (!dlg_parse_cond(ch.cond, op, &err)) add(true, int(c), int(i), cw + ": if " + err);
                if (!dlg_parse_effects(ch.effects, ops, &err)) add(true, int(c), int(i), cw + ": do " + err);
                const int cx = dlg_resolve(cv, i, ch.next);
                if (cx == -2) add(true, int(c), int(i), cw + ": next '" + ch.next + "' is not a line id");
                if (cx >= 0) reach[size_t(cx)] = true;
            }
        }
        for (size_t i = 1; i < cv.nodes.size(); ++i)
            if (!reach[i]) add(false, int(c), int(i), "'" + cv.name + "' line " + std::to_string(i + 1) + " can never be reached");
    }
    return out;
}
inline bool dlg_has_errors(const std::vector<DlgProblem>& p) {
    for (const DlgProblem& x : p) if (x.error) return true;
    return false;
}

// ---- compile --------------------------------------------------------------------------------
// The baked tables (host copies), exactly what the Dialogue asset holds.
struct DlgCompiled {
    std::vector<phx::DlgConvDef> convs;
    std::vector<phx::DlgSpeakerDef> speakers;
    std::vector<phx::DlgNodeDef> nodes;
    std::vector<phx::DlgChoiceDef> choices;
    std::vector<phx::DlgOp> ops;
    std::string strings;
    std::vector<std::string> speaker_names;

    const char* str(uint32_t off) const { return off < strings.size() ? strings.c_str() + off : ""; }
    int find(const std::string& conv) const {
        const phx::NameHash h = phx::fnv1a(conv.c_str());
        for (size_t i = 0; i < convs.size(); ++i) if (convs[i].name == h) return int(i);
        return -1;
    }
    std::vector<uint8_t> blob() const {
        phx::DialogueHeader h{};
        h.magic = phx::kDialogueMagic;
        h.conv_count = uint16_t(convs.size()); h.speaker_count = uint16_t(speakers.size());
        h.node_count = uint16_t(nodes.size()); h.choice_count = uint16_t(choices.size());
        h.op_count = uint16_t(ops.size());
        std::string s = strings;
        while (s.size() % 4) s += '\0';
        h.strings_size = uint32_t(s.size());
        std::vector<uint8_t> b;
        auto put = [&](const void* p, size_t n) { const uint8_t* q = static_cast<const uint8_t*>(p); b.insert(b.end(), q, q + n); };
        put(&h, sizeof(h));
        if (!convs.empty())    put(convs.data(), convs.size() * sizeof(convs[0]));
        if (!speakers.empty()) put(speakers.data(), speakers.size() * sizeof(speakers[0]));
        if (!nodes.empty())    put(nodes.data(), nodes.size() * sizeof(nodes[0]));
        if (!choices.empty())  put(choices.data(), choices.size() * sizeof(choices[0]));
        if (!ops.empty())      put(ops.data(), ops.size() * sizeof(ops[0]));
        put(s.data(), s.size());
        return b;
    }
};

inline bool dlg_compile(const DlgDoc& d, DlgCompiled& out, std::string* err = nullptr) {
    const std::vector<DlgProblem> probs = dlg_validate(d);
    for (const DlgProblem& p : probs) if (p.error) { if (err) *err = p.what; return false; }
    out = DlgCompiled{};
    std::map<std::string, uint32_t> interned;
    auto intern = [&](const std::string& s) -> uint32_t {
        auto it = interned.find(s);
        if (it != interned.end()) return it->second;
        const uint32_t off = uint32_t(out.strings.size());
        out.strings += s;
        out.strings += '\0';
        interned[s] = off;
        return off;
    };
    intern("");
    std::vector<DlgSpeaker> speakers = d.speakers;
    for (const DlgConversation& c : d.convs)
        for (const DlgNode& n : c.nodes)
            if (!n.speaker.empty() && std::none_of(speakers.begin(), speakers.end(), [&](const DlgSpeaker& s) { return s.name == n.speaker; }))
                speakers.push_back(DlgSpeaker{ n.speaker, "" });
    for (const DlgSpeaker& s : speakers) {
        out.speakers.push_back(phx::DlgSpeakerDef{ intern(s.name), s.portrait.empty() ? 0u : phx::fnv1a(s.portrait.c_str()) });
        out.speaker_names.push_back(s.name);
    }
    size_t total_nodes = 0;
    for (const DlgConversation& c : d.convs) total_nodes += c.nodes.size();
    if (total_nodes >= phx::kDlgEnd) { if (err) *err = "more than 65534 lines"; return false; }
    for (const DlgConversation& c : d.convs) {
        const uint16_t base = uint16_t(out.nodes.size());
        out.convs.push_back(phx::DlgConvDef{ phx::fnv1a(c.name.c_str()), base, uint16_t(c.nodes.size()) });
        auto target = [&](size_t i, const std::string& next) -> uint16_t {
            const int k = dlg_resolve(c, i, next);
            return k < 0 ? phx::kDlgEnd : uint16_t(base + k);
        };
        auto effects = [&](const std::string& text, uint16_t& first, uint8_t& count) {
            std::vector<phx::DlgOp> ops;
            dlg_parse_effects(text, ops);
            first = uint16_t(out.ops.size());
            count = uint8_t(std::min<size_t>(255, ops.size()));
            out.ops.insert(out.ops.end(), ops.begin(), ops.begin() + count);
        };
        for (size_t i = 0; i < c.nodes.size(); ++i) {
            const DlgNode& n = c.nodes[i];
            phx::DlgNodeDef nd{};
            nd.text = intern(n.text);
            nd.speaker = phx::kDlgNoSpeaker;
            for (size_t s = 0; s < speakers.size(); ++s) if (!n.speaker.empty() && speakers[s].name == n.speaker) nd.speaker = uint16_t(s);
            nd.next = target(i, n.next);
            dlg_parse_cond(n.cond, nd.cond);
            effects(n.effects, nd.first_op, nd.op_count);
            nd.first_choice = uint16_t(out.choices.size());
            nd.choice_count = uint8_t(std::min<size_t>(255, n.choices.size()));
            for (size_t k = 0; k < nd.choice_count; ++k) {
                const DlgChoice& ch = n.choices[k];
                phx::DlgChoiceDef cd{};
                cd.text = intern(ch.text);
                cd.next = target(i, ch.next);
                dlg_parse_cond(ch.cond, cd.cond);
                effects(ch.effects, cd.first_op, cd.op_count);
                out.choices.push_back(cd);
            }
            out.nodes.push_back(nd);
        }
    }
    if (out.choices.size() >= 65535 || out.ops.size() >= 65535) { if (err) *err = "too many choices or effects"; return false; }
    return true;
}

// ---- simulation (the Studio's play-through) --------------------------------------------------
// Walks the compiled tables with the runtime's rules (phx/runtime/dialogue.cpp): skip nodes whose
// test fails, hide choices whose test fails, apply effects on showing / picking.
struct DlgSim {
    const DlgCompiled* d = nullptr;
    std::map<phx::NameHash, int> vars;
    uint16_t node = phx::kDlgEnd;
    std::vector<uint16_t> visible;               // choice indices
    int shown = 0;

    int get(phx::NameHash v) const { auto it = vars.find(v); return it == vars.end() ? 0 : it->second; }
    bool test(const phx::DlgOp& op) const {
        const int v = get(op.var), k = op.value;
        switch (op.op) {
        case phx::kDlgEq: return v == k;  case phx::kDlgNe: return v != k;
        case phx::kDlgLt: return v < k;   case phx::kDlgLe: return v <= k;
        case phx::kDlgGt: return v > k;   case phx::kDlgGe: return v >= k;
        default: return true;
        }
    }
    void apply(uint16_t first, uint8_t count) {
        for (uint32_t i = 0; i < count; ++i) {
            const phx::DlgOp& op = d->ops[first + i];
            if (op.op == phx::kDlgSet) vars[op.var] = op.value;
            else if (op.op == phx::kDlgAdd) vars[op.var] = get(op.var) + op.value;
            else if (op.op == phx::kDlgSub) vars[op.var] = get(op.var) - op.value;
        }
    }
    void enter(uint16_t n) {
        size_t guard = d->nodes.size() + 1;
        while (n != phx::kDlgEnd && n < d->nodes.size() && guard--) {
            const phx::DlgNodeDef& nd = d->nodes[n];
            if (nd.cond.op != phx::kDlgNone && !test(nd.cond)) { n = nd.next; continue; }
            apply(nd.first_op, nd.op_count);
            node = n; ++shown;
            visible.clear();
            for (uint32_t i = 0; i < nd.choice_count && visible.size() < 8; ++i) {
                const phx::DlgChoiceDef& c = d->choices[nd.first_choice + i];
                if (c.cond.op == phx::kDlgNone || test(c.cond)) visible.push_back(uint16_t(nd.first_choice + i));
            }
            return;
        }
        node = phx::kDlgEnd;
        visible.clear();
    }
    bool start(const DlgCompiled& c, const std::string& conv) {
        d = &c;
        node = phx::kDlgEnd; shown = 0;
        const int k = c.find(conv);
        if (k < 0 || !c.convs[size_t(k)].node_count) return false;
        enter(c.convs[size_t(k)].first_node);
        return active();
    }
    bool active() const { return node != phx::kDlgEnd; }
    const char* text() const { return active() ? d->str(d->nodes[node].text) : ""; }
    const char* speaker() const {
        if (!active() || d->nodes[node].speaker == phx::kDlgNoSpeaker) return "";
        return d->speaker_names[d->nodes[node].speaker].c_str();
    }
    // Go on: pick visible choice `pick` (when the line has choices), else the line's next.
    void advance(int pick = 0) {
        if (!active()) return;
        if (!visible.empty()) {
            const phx::DlgChoiceDef& c = d->choices[visible[size_t(std::max(0, std::min(pick, int(visible.size()) - 1)))]];
            apply(c.first_op, c.op_count);
            enter(c.next);
        } else {
            enter(d->nodes[node].next);
        }
    }
};

// Every variable a dialogue's conditions and effects name, sorted (the Studio's play panel).
inline std::vector<std::string> dlg_var_names(const DlgDoc& d) {
    std::vector<std::string> out;
    auto scan = [&](const std::string& s) {
        std::string cur;
        for (size_t i = 0; i <= s.size(); ++i) {
            const char c = i < s.size() ? s[i] : ' ';
            if (std::isalnum(uint8_t(c)) || c == '_') { cur += c; continue; }
            if (!cur.empty() && !std::isdigit(uint8_t(cur[0])) && std::find(out.begin(), out.end(), cur) == out.end())
                out.push_back(cur);
            cur.clear();
        }
    };
    for (const DlgConversation& c : d.convs)
        for (const DlgNode& n : c.nodes) {
            scan(n.cond); scan(n.effects);
            for (const DlgChoice& ch : n.choices) { scan(ch.cond); scan(ch.effects); }
        }
    std::sort(out.begin(), out.end());
    return out;
}

// A starter file: one conversation with a choice and a condition.
inline DlgDoc dlg_starter() {
    DlgDoc d;
    d.speakers = { DlgSpeaker{ "Sign", "" } };
    DlgConversation c;
    c.name = "sign";
    c.nodes = {
        DlgNode{ "hi", "Sign", "Welcome, traveller! Coins are yours to keep; the door leads on.", "", "", "", {} },
        DlgNode{ "ask", "Sign", "Want some advice?", "", "", "", {
            DlgChoice{ "Yes, please", "tip", "", "" },
            DlgChoice{ "Trade 2 coins for luck", "luck", "coins >= 2", "coins -= 2, lucky = 1" },
            DlgChoice{ "No thanks", "end", "", "" } } },
        DlgNode{ "tip", "Sign", "Jump over the slime. It bites.", "end", "", "", {} },
        DlgNode{ "luck", "Sign", "Luck is with you now.", "end", "lucky", "", {} },
    };
    d.convs = { c };
    return d;
}

} // namespace phxtool
#endif // PHX_TOOLS_DIALOGUE_H
