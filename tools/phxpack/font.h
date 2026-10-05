// tools/phxpack/font.h — HOST-ONLY font model: a `.font` grid sheet (proportional widths measured
// from its pixels) or an imported BMFont text `.fnt`, turned into the glyph table of a Font asset
// (phx/resource/bundle.h FontBlobHeader + FontGlyphDef; phx/runtime/font.h loads it for phx::UI).
// builders.h build_font bakes it; Phosphorus Studio's font editor edits `.font` files with the same
// code, so its preview measures exactly what the bake will.
#ifndef PHX_TOOLS_FONT_H
#define PHX_TOOLS_FONT_H

#include "json.h"
#include "phx/resource/bundle.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace phxtool {

// A sheet named in a def: a bare file name sits next to the def; a path with '/' is used as-is.
inline std::string font_image_path(const std::string& def_path, const std::string& image) {
    if (image.empty() || image[0] == '/' || image.find('/') != std::string::npos || image.find(':') != std::string::npos)
        return image;
    const size_t sl = def_path.find_last_of("/\\");
    return sl == std::string::npos ? image : def_path.substr(0, sl + 1) + image;
}

// ---- fonts: a .font grid sheet (proportional widths measured from the pixels) or a BMFont .fnt ----
// .font (JSON; every key but "image" optional):
//   { "font": 1, "image": "font.png", "cell_w": 8, "cell_h": 8, "first": 32, "count": 0,
//     "proportional": true, "spacing": 1, "space": 3, "advance": 0, "line_h": 0 }
// The sheet is a grid of cell_w x cell_h cells, row by row, cell 0 = character `first`; `count`
// cells (0 = every cell in the sheet). Proportional: each glyph is trimmed to its opaque columns
// and advances by its width + `spacing` (an empty cell, like space, by `space`). Fixed: every
// glyph is its whole cell and advances by `advance` (0 = cell_w). line_h 0 = cell_h + 1.
struct FontDef {
    std::string image;                 // the sheet PNG, as written (font_image_path resolves it)
    int cell_w = 8, cell_h = 8, first = 32, count = 0;
    bool proportional = true;
    int spacing = 1, space = 0, advance = 0, line_h = 0;
};

inline bool load_fontdef(const std::string& text, const std::string& path, FontDef& out, std::string* err = nullptr) {
    auto fail = [&](const std::string& why) { if (err) *err = why; return false; };
    JsonValue root;
    std::string jerr;
    if (!JsonParser::parse(text, root, &jerr)) return fail("malformed JSON: " + jerr);
    if (!root.is_obj()) return fail("top level is not a JSON object");
    out = FontDef{};
    out.image = root.str_at("image");
    if (out.image.empty()) return fail("missing \"image\" (the font sheet PNG)");
    out.cell_w = root.int_at("cell_w", 8);
    out.cell_h = root.int_at("cell_h", 8);
    out.first = root.int_at("first", 32);
    out.count = root.int_at("count", 0);
    if (const JsonValue* p = root.find("proportional")) out.proportional = p->as_bool(true);
    out.spacing = root.int_at("spacing", 1);
    out.space = root.int_at("space", 0);
    out.advance = root.int_at("advance", 0);
    out.line_h = root.int_at("line_h", 0);
    if (out.cell_w < 1 || out.cell_h < 1 || out.cell_w > 255 || out.cell_h > 255) return fail("cell_w / cell_h must be 1..255");
    if (out.first < 0 || out.first > 255) return fail("first must be a character code 0..255");
    if (out.count < 0 || out.first + out.count > 256) return fail("count runs past character 255");
    (void)path;
    return true;
}

// The glyph table of a grid sheet (px: w x h RGBA). Deterministic: pure pixel measurement.
inline bool font_glyphs_from_grid(const FontDef& d, const std::vector<uint32_t>& px, int w, int h,
                                  phx::FontBlobHeader& hdr, std::vector<phx::FontGlyphDef>& glyphs, std::string* err = nullptr) {
    const int cols = w / d.cell_w, rows = h / d.cell_h;
    if (cols < 1 || rows < 1) { if (err) *err = "the sheet is smaller than one cell"; return false; }
    int n = d.count ? d.count : cols * rows;
    n = std::min(n, std::min(cols * rows, 256 - d.first));
    glyphs.assign(size_t(n), phx::FontGlyphDef{});
    const int fixed_adv = d.advance > 0 ? d.advance : d.cell_w;
    const int space = d.space > 0 ? d.space : std::max(1, d.cell_w / 2);
    int widest = 1;
    for (int i = 0; i < n; ++i) {
        const int cx = (i % cols) * d.cell_w, cy = (i / cols) * d.cell_h;
        int lo = d.cell_w, hi = -1;
        for (int y = 0; y < d.cell_h; ++y)
            for (int x = 0; x < d.cell_w; ++x)
                if (px[size_t(cy + y) * size_t(w) + size_t(cx + x)] >> 24) { lo = std::min(lo, x); hi = std::max(hi, x); }
        phx::FontGlyphDef& g = glyphs[size_t(i)];
        if (!d.proportional) {
            g.sx = uint16_t(cx); g.sy = uint16_t(cy); g.w = uint8_t(d.cell_w); g.h = uint8_t(d.cell_h);
            g.advance = uint8_t(std::min(255, fixed_adv));
            if (hi < 0) g.w = g.h = 0;
        } else if (hi < 0) {                                   // an empty cell (space)
            g.advance = uint8_t(std::min(255, space));
        } else {
            g.sx = uint16_t(cx + lo); g.sy = uint16_t(cy);
            g.w = uint8_t(hi - lo + 1); g.h = uint8_t(d.cell_h);
            g.advance = uint8_t(std::min(255, int(g.w) + d.spacing));
        }
        widest = std::max(widest, int(g.advance));
    }
    hdr = phx::FontBlobHeader{};
    hdr.glyph_count = uint16_t(n);
    hdr.first_char = uint8_t(d.first);
    hdr.line_h = uint8_t(std::min(255, d.line_h > 0 ? d.line_h : d.cell_h + 1));
    hdr.cell_w = uint8_t(d.cell_w); hdr.cell_h = uint8_t(d.cell_h);
    hdr.advance = uint8_t(d.proportional ? widest : std::min(255, fixed_adv));
    hdr.flags = d.proportional ? phx::kFontProportional : 0;
    return true;
}

// A BMFont text descriptor (.fnt from BMFont, Hiero, Littera...): one page, characters 0..255.
// Each char's rect, offset and xadvance go into the table from the lowest id (at least 32).
struct BmFont {
    std::string page;                   // the page PNG, as written (font_image_path resolves it)
    int line_h = 0, base = 0;
    struct Char { int id, x, y, w, h, xoff, yoff, adv; };
    std::vector<Char> chars;
};
inline int bmf_int(const std::string& line, const char* key, int def = 0) {
    const std::string k = std::string(" ") + key + "=";
    const size_t p = (" " + line).find(k);
    if (p == std::string::npos) return def;
    return std::atoi(line.c_str() + p + k.size() - 1);
}
inline std::string bmf_str(const std::string& line, const char* key) {
    const std::string k = std::string(" ") + key + "=\"";
    const size_t p = (" " + line).find(k);
    if (p == std::string::npos) return "";
    const size_t a = p + k.size() - 1, b = line.find('"', a);
    return b == std::string::npos ? line.substr(a) : line.substr(a, b - a);
}
inline bool load_bmfont(const std::string& text, const std::string& path, BmFont& out, std::string* err = nullptr) {
    auto fail = [&](const std::string& why) { if (err) *err = why; return false; };
    out = BmFont{};
    int pages = 1;
    size_t p = 0;
    while (p < text.size()) {
        size_t e = text.find('\n', p);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(p, e - p);
        p = e + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.compare(0, 7, "common ") == 0) {
            out.line_h = bmf_int(line, "lineHeight"); out.base = bmf_int(line, "base"); pages = bmf_int(line, "pages", 1);
        } else if (line.compare(0, 5, "page ") == 0) {
            if (bmf_int(line, "id") == 0) out.page = bmf_str(line, "file");
        } else if (line.compare(0, 5, "char ") == 0) {
            if (bmf_int(line, "page") != 0) continue;
            out.chars.push_back(BmFont::Char{ bmf_int(line, "id"), bmf_int(line, "x"), bmf_int(line, "y"),
                                              bmf_int(line, "width"), bmf_int(line, "height"),
                                              bmf_int(line, "xoffset"), bmf_int(line, "yoffset"), bmf_int(line, "xadvance") });
        }
    }
    if (text.compare(0, 3, "BMF") == 0 || text.find("<font>") != std::string::npos)
        return fail("only the BMFont TEXT format is read (export .fnt as text, not binary/XML)");
    if (out.page.empty()) return fail("no 'page id=0 file=...' line");
    if (out.chars.empty()) return fail("no 'char' lines");
    if (pages > 1) std::fprintf(stderr, "phx: font '%s' has %d pages; only page 0 is baked\n", path.c_str(), pages);
    return true;
}
inline bool font_glyphs_from_bmfont(const BmFont& f, phx::FontBlobHeader& hdr, std::vector<phx::FontGlyphDef>& glyphs,
                                    std::string* err = nullptr) {
    int lo = 256, hi = -1;
    for (const auto& c : f.chars) if (c.id >= 32 && c.id <= 255) { lo = std::min(lo, c.id); hi = std::max(hi, c.id); }
    if (hi < 0) { if (err) *err = "no characters in 32..255"; return false; }
    glyphs.assign(size_t(hi - lo + 1), phx::FontGlyphDef{});
    int widest = 1, cell_w = 1, cell_h = 1;
    for (const auto& c : f.chars) {
        if (c.id < lo || c.id > hi) continue;
        phx::FontGlyphDef& g = glyphs[size_t(c.id - lo)];
        g.sx = uint16_t(std::max(0, c.x)); g.sy = uint16_t(std::max(0, c.y));
        g.w = uint8_t(std::min(255, std::max(0, c.w))); g.h = uint8_t(std::min(255, std::max(0, c.h)));
        g.xoff = int8_t(std::max(-128, std::min(127, c.xoff)));
        g.yoff = int8_t(std::max(-128, std::min(127, c.yoff)));
        g.advance = uint8_t(std::min(255, std::max(0, c.adv)));
        widest = std::max(widest, int(g.advance));
        cell_w = std::max(cell_w, int(g.w)); cell_h = std::max(cell_h, int(g.h));
    }
    hdr = phx::FontBlobHeader{};
    hdr.glyph_count = uint16_t(glyphs.size());
    hdr.first_char = uint8_t(lo);
    hdr.line_h = uint8_t(std::min(255, std::max(1, f.line_h ? f.line_h : cell_h + 1)));
    hdr.cell_w = uint8_t(cell_w); hdr.cell_h = uint8_t(cell_h);
    hdr.advance = uint8_t(widest);
    hdr.flags = phx::kFontProportional;
    return true;
}

// The .font JSON for `d` (what the Studio's font editor saves).
inline std::string fontdef_to_json(const FontDef& d) {
    std::string img;
    for (char c : d.image) { if (c == '"' || c == '\\') img += '\\'; img += c; }
    return "{ \"font\": 1, \"image\": \"" + img + "\", \"cell_w\": " + std::to_string(d.cell_w) +
           ", \"cell_h\": " + std::to_string(d.cell_h) + ",\n  \"first\": " + std::to_string(d.first) +
           ", \"count\": " + std::to_string(d.count) + ", \"proportional\": " + (d.proportional ? "true" : "false") +
           ",\n  \"spacing\": " + std::to_string(d.spacing) + ", \"space\": " + std::to_string(d.space) +
           ", \"advance\": " + std::to_string(d.advance) + ", \"line_h\": " + std::to_string(d.line_h) + " }\n";
}
inline bool is_font_json(const std::string& head) { return head.find("\"font\"") != std::string::npos && head.find("\"image\"") != std::string::npos; }

} // namespace phxtool
#endif // PHX_TOOLS_FONT_H
