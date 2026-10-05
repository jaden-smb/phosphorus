// tools/phxstudio/components.h — the game's reflected components (phx/ecs/reflect.h) as Phosphorus
// Studio sees them, and how the table editor's prefab inspector applies one to a prefab record.
// `make game` writes <project>/build/components.json from the built game itself (the engine's
// desktop entry, PHX_DUMP_COMPONENTS); this parses it. Headless, unit-tested in the editors suite.
//
// On a prefab record, component C with field f is: C's name in the record's `components` column
// (space separated, "Enemy Coin"), plus a column `C_f` holding the value (the level loader reads
// the same names; a spawn property `C_f` overrides it per placed spawn).
#ifndef PHX_TOOLS_PHXSTUDIO_COMPONENTS_H
#define PHX_TOOLS_PHXSTUDIO_COMPONENTS_H

#include "json.h"                  // tools/phxpack
#include "../phxentity/editor.h"   // BinDoc

#include <algorithm>
#include <string>
#include <vector>

namespace phxstudio {

struct CompSchema {
    struct Field { std::string name, type; double def = 0; };   // type: i8 u8 i16 u16 i32 u32 bool scalar hash
    struct Comp  { std::string name; std::vector<Field> fields; };
    std::vector<Comp> comps;

    static bool parse(const std::string& json, CompSchema& out, std::string* err = nullptr) {
        out = CompSchema{};
        phxtool::JsonValue root;
        std::string e;
        if (!phxtool::JsonParser::parse(json, root, &e)) { if (err) *err = e; return false; }
        const phxtool::JsonValue* cs = root.find("components");
        if (!cs || !cs->is_arr()) { if (err) *err = "no \"components\" array"; return false; }
        for (const phxtool::JsonValue& c : cs->arr) {
            Comp comp;
            comp.name = c.str_at("name");
            if (comp.name.empty()) continue;
            if (const phxtool::JsonValue* fs = c.find("fields"); fs && fs->is_arr())
                for (const phxtool::JsonValue& f : fs->arr) {
                    Field fl;
                    fl.name = f.str_at("name");
                    fl.type = f.str_at("type");
                    if (const phxtool::JsonValue* d = f.find("default")) fl.def = d->type == phxtool::JsonValue::Bool ? (d->boolean ? 1 : 0) : d->as_num();
                    if (!fl.name.empty()) comp.fields.push_back(fl);
                }
            out.comps.push_back(comp);
        }
        return true;
    }
    const Comp* find(const std::string& name) const {
        for (const Comp& c : comps) if (c.name == name) return &c;
        return nullptr;
    }
};

// The phxbin column type that holds a reflected field (scalar is authored as a decimal, a hash as
// the text it is the hash of; bool as 0/1).
inline std::string comp_column_type(const std::string& field_type) {
    if (field_type == "scalar") return "f32";
    if (field_type == "hash")   return "str16";
    if (field_type == "bool")   return "u8";
    return field_type;                       // i8 u8 i16 u16 i32 u32 are phxbin types already
}
inline std::string comp_column(const std::string& comp, const std::string& field) { return comp + "_" + field; }

// The component names listed in a `components` cell.
inline std::vector<std::string> split_components(const std::string& cell) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : cell + " ") {
        if (c == ' ' || c == ',' || c == '\t') { if (!cur.empty()) out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    return out;
}

// The index of the `components` column (fields.size() when there is none).
inline size_t components_field(const phxtool::BinDoc& d) {
    for (size_t f = 0; f < d.fields.size(); ++f) if (d.fields[f].name == "components") return f;
    return d.fields.size();
}

inline bool record_has_component(const phxtool::BinDoc& d, size_t rec, const std::string& comp) {
    const size_t cf = components_field(d);
    if (cf == d.fields.size() || rec >= d.records.size()) return false;
    for (const std::string& n : split_components(d.str_cell(rec, cf))) if (n == comp) return true;
    return false;
}

// Turn component `c` on or off for record `rec`: edits the `components` cell (adding the column,
// str64, when the table has none) and, when turning it on, makes sure every field has its typed
// column, setting this record's cell to the component's default. Off leaves the columns (other
// records may use them). False when nothing changed.
inline bool set_record_component(phxtool::BinDoc& d, size_t rec, const CompSchema::Comp& c, bool on) {
    if (rec >= d.records.size() || record_has_component(d, rec, c.name) == on) return false;
    size_t cf = components_field(d);
    if (cf == d.fields.size()) {
        if (!on) return false;
        d.add_field("components", "str64");
        cf = components_field(d);
    }
    std::vector<std::string> names = split_components(d.str_cell(rec, cf));
    if (on) names.push_back(c.name);
    else names.erase(std::remove(names.begin(), names.end(), c.name), names.end());
    std::string cell;
    for (const std::string& n : names) cell += (cell.empty() ? "" : " ") + n;
    d.set_str(rec, cf, cell);
    if (on)
        for (const CompSchema::Field& f : c.fields) {
            const std::string col = comp_column(c.name, f.name);
            size_t fi = d.fields.size();
            for (size_t k = 0; k < d.fields.size(); ++k) if (d.fields[k].name == col) fi = k;
            if (fi == d.fields.size()) {
                if (!d.add_field(col, comp_column_type(f.type))) continue;
                fi = d.fields.size() - 1;
            }
            if (!d.field_is_str(fi)) d.set_cell_text(rec, fi, f.type == "scalar" ? phxtool::BinDoc::fmt_float(f.def)
                                                                                 : std::to_string(int64_t(f.def)));
        }
    return true;
}

} // namespace phxstudio
#endif // PHX_TOOLS_PHXSTUDIO_COMPONENTS_H
