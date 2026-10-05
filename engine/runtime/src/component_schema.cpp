// engine/runtime/src/component_schema.cpp — write the program's reflected components
// (phx/ecs/reflect.h) as JSON for tools: names, fields, types and the C++ defaults. The desktop
// entry calls it when PHX_DUMP_COMPONENTS names a file (`make game` does, so Phosphorus Studio can
// show a game's components). Host-only: linked by desktop builds, never by a ROM or an EBOOT.
#include "phx/runtime/main.h"
#include "phx/ecs/reflect.h"

#include <cstdio>
#include <cstring>

namespace phx {
namespace {

const char* type_name(FieldType t) {
    switch (t) {
        case FieldType::I8:     return "i8";
        case FieldType::U8:     return "u8";
        case FieldType::I16:    return "i16";
        case FieldType::U16:    return "u16";
        case FieldType::I32:    return "i32";
        case FieldType::U32:    return "u32";
        case FieldType::Bool:   return "bool";
        case FieldType::Scalar: return "scalar";
        case FieldType::Hash:   return "hash";
    }
    return "i32";
}

// The default of one field, read out of a default-constructed instance.
double default_of(const FieldInfo& f, const uint8_t* d) {
    const uint8_t* p = d + f.offset;
    switch (f.type) {
        case FieldType::I8:   { int8_t v;   std::memcpy(&v, p, 1); return v; }
        case FieldType::U8:   { uint8_t v;  std::memcpy(&v, p, 1); return v; }
        case FieldType::I16:  { int16_t v;  std::memcpy(&v, p, 2); return v; }
        case FieldType::U16:  { uint16_t v; std::memcpy(&v, p, 2); return v; }
        case FieldType::I32:  { int32_t v;  std::memcpy(&v, p, 4); return v; }
        case FieldType::U32:  { uint32_t v; std::memcpy(&v, p, 4); return v; }
        case FieldType::Bool: { bool v;     std::memcpy(&v, p, sizeof(v)); return v ? 1 : 0; }
        case FieldType::Scalar: { scalar v; std::memcpy(&v, p, sizeof(v)); return s_to_double(v); }
        case FieldType::Hash: return 0;   // authored as text: no default text to show
    }
    return 0;
}

} // namespace

bool write_component_schema(const char* path) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    alignas(16) uint8_t buf[512];
    std::fprintf(f, "{ \"components\": [");
    for (uint32_t i = 0; i < reflected_count(); ++i) {
        const ComponentInfo& c = *reflected_at(i);
        const bool defaults = c.size <= sizeof(buf);
        if (defaults) c.make_default(buf);
        std::fprintf(f, "%s\n  { \"name\": \"%s\", \"size\": %u, \"fields\": [", i ? "," : "", c.name, unsigned(c.size));
        for (uint8_t k = 0; k < c.field_count; ++k) {
            const FieldInfo& fi = c.fields[k];
            std::fprintf(f, "%s\n    { \"name\": \"%s\", \"type\": \"%s\", \"default\": %.9g }", k ? "," : "",
                         fi.name, type_name(fi.type), defaults ? default_of(fi, buf) : 0.0);
        }
        std::fprintf(f, " ] }");
    }
    std::fprintf(f, "\n] }\n");
    return std::fclose(f) == 0;
}

} // namespace phx
