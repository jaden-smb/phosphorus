// phx/ecs/reflect.h — component REFLECTION: a game declares a component once, with the fields
// that are data, and the engine can then build it from data and tools can show it:
//
//     struct Enemy { int16_t range = 24; scalar speed = s_from_int(30); bool angry = false; };
//     PHX_COMPONENT(Enemy, PHX_FIELD(Enemy, range), PHX_FIELD(Enemy, speed), PHX_FIELD(Enemy, angry));
//
// What that buys:
//   * the level loader (phx/runtime/level.h) attaches the components a prefab lists in its
//     `components` column ("Enemy Coin") and fills each field from the `Enemy_range` column or
//     the spawn's `Enemy_range` property, else keeps the C++ default above;
//   * `make game` asks the built game for its registry (write_component_schema) and Phosphorus
//     Studio's table editor offers those components, fields and defaults on each prefab record.
//
// Field types are the plain-data ones a component holds: 8/16/32-bit ints, bool, scalar (float on
// PC/PSP, fixed16 on GBA — authored as a decimal), and NameHash for names (PHX_FIELD_HASH;
// authored as text and hashed at load). The registry is a fixed table filled by static
// initialisers (no heap), sized kMaxReflected; the field tables are constexpr data in ROM.
#ifndef PHX_ECS_REFLECT_H
#define PHX_ECS_REFLECT_H

#include "phx/core/types.h"
#include "phx/core/math.h"
#include "phx/ecs/world.h"

#include <cstddef>
#include <type_traits>

namespace phx {

enum class FieldType : uint8_t { I8, U8, I16, U16, I32, U32, Bool, Scalar, Hash };

struct FieldInfo {
    const char* name;
    FieldType   type;
    uint16_t    offset;
};

struct ComponentInfo {
    const char*      name;
    NameHash         hash;          // fnv1a(name): what a prefab's `components` column names
    uint16_t         size;
    const FieldInfo* fields;
    uint8_t          field_count;
    // Add the component (default-constructed) to an entity and return its storage.
    void* (*add)(ecs::World&, ecs::Entity);
    // The entity's instance, or nullptr when it has none (tools: the live inspector).
    void* (*get)(ecs::World&, ecs::Entity);
    // Write a default-constructed instance into `out` (size bytes): the defaults tools show.
    void  (*make_default)(void* out);
};

// The component types a PHX_FIELD can describe.
template <class T> constexpr FieldType field_type_of() {
    static_assert(std::is_same<T, int8_t>::value  || std::is_same<T, uint8_t>::value  ||
                  std::is_same<T, int16_t>::value || std::is_same<T, uint16_t>::value ||
                  std::is_same<T, int32_t>::value || std::is_same<T, uint32_t>::value ||
                  std::is_same<T, bool>::value    || std::is_same<T, scalar>::value,
                  "PHX_FIELD: a field must be an 8/16/32-bit int, bool or scalar");
    return std::is_same<T, int8_t>::value  ? FieldType::I8  : std::is_same<T, uint8_t>::value  ? FieldType::U8  :
           std::is_same<T, int16_t>::value ? FieldType::I16 : std::is_same<T, uint16_t>::value ? FieldType::U16 :
           std::is_same<T, int32_t>::value ? FieldType::I32 : std::is_same<T, uint32_t>::value ? FieldType::U32 :
           std::is_same<T, bool>::value    ? FieldType::Bool : FieldType::Scalar;
}

namespace reflect_detail {
constexpr uint32_t kMaxReflected = 32;
struct Registry { const ComponentInfo* items[kMaxReflected]; uint32_t count; };
inline Registry& registry() { static Registry r{ {}, 0 }; return r; }
inline bool add(const ComponentInfo* c) {
    Registry& r = registry();
    for (uint32_t i = 0; i < r.count; ++i) if (r.items[i]->hash == c->hash) return true;
    if (r.count < kMaxReflected) r.items[r.count++] = c;
    return true;
}
} // namespace reflect_detail

// Every component declared with PHX_COMPONENT in this program.
inline uint32_t reflected_count() { return reflect_detail::registry().count; }
inline const ComponentInfo* reflected_at(uint32_t i) {
    return i < reflected_count() ? reflect_detail::registry().items[i] : nullptr;
}
inline const ComponentInfo* find_reflected(NameHash name) {
    for (uint32_t i = 0; i < reflected_count(); ++i)
        if (reflected_at(i)->hash == name) return reflected_at(i);
    return nullptr;
}

} // namespace phx

// A data field of component type T (an int, bool or scalar member).
#define PHX_FIELD(T, member) \
    ::phx::FieldInfo{ #member, ::phx::field_type_of<decltype(T::member)>(), uint16_t(offsetof(T, member)) }
// A NameHash member, authored as text ("door_b") and hashed when the level is built.
#define PHX_FIELD_HASH(T, member) \
    ::phx::FieldInfo{ #member, ::phx::FieldType::Hash, uint16_t(offsetof(T, member)) }

// Declare component T (at namespace scope, once per program) with its data fields.
#define PHX_COMPONENT(T, ...)                                                                      \
    namespace phx_reflect_##T {                                                                    \
        const ::phx::FieldInfo kFields[] = { __VA_ARGS__ };                                        \
        const ::phx::ComponentInfo kInfo = {                                                       \
            #T, ::phx::fnv1a(#T), uint16_t(sizeof(T)), kFields,                                    \
            uint8_t(sizeof(kFields) / sizeof(kFields[0])),                                         \
            [](::phx::ecs::World& w, ::phx::ecs::Entity e) -> void* { return &w.add<T>(e, T{}); }, \
            [](::phx::ecs::World& w, ::phx::ecs::Entity e) -> void* { return w.get<T>(e); },        \
            [](void* out) { *static_cast<T*>(out) = T{}; } };                                      \
        const bool kRegistered = ::phx::reflect_detail::add(&kInfo);                               \
    }                                                                                              \
    static_assert(std::is_trivially_copyable<T>::value, "PHX_COMPONENT: components are plain data")

#endif // PHX_ECS_REFLECT_H
