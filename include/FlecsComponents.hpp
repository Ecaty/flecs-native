#pragma once
#include "flecs.h"
#include <cstdint>

namespace flecs_unity {

struct Position { float x, y, z; };
struct Rotation { float x, y, z; };
struct Scale { float x, y, z; };
struct Color { float r, g, b, a; };
struct Presented {
    Position position; Rotation rotation; Scale scale; Color color;
    std::int32_t frame, primitive, has_label;
};
struct Primitive { std::int32_t kind; };
// C++ lifecycle hooks own the reflected string, including prefab copies.
// Do not assign borrowed string literals to this field.
struct Label {
    char* text = nullptr;
    Label() = default;
    Label(const Label& other) : text(other.text ? ecs_os_strdup(other.text) : nullptr) {}
    Label(Label&& other) noexcept : text(other.text) { other.text = nullptr; }
    Label& operator=(const Label& other) {
        if (this != &other) { auto* copy = other.text ? ecs_os_strdup(other.text) : nullptr; ecs_os_free(text); text = copy; }
        return *this;
    }
    Label& operator=(Label&& other) noexcept {
        if (this != &other) { ecs_os_free(text); text = other.text; other.text = nullptr; }
        return *this;
    }
    ~Label() { ecs_os_free(text); }
};

// Engine-independent components, reflection and behavior. No Unity dependencies.
void register_core(flecs::world& world);

// Implemented by the application module linked into the same native plugin.
void register_application(flecs::world& world);

} // namespace flecs_unity
