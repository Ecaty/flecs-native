#pragma once
#include "flecs.h"
#include <cstdint>

namespace flecs_unity {

struct Position { float x, y, z; };
struct Rotation { float x, y, z; };
struct Scale { float x, y, z; };
struct Color { float r, g, b, a; };
struct Primitive { std::int32_t kind; };
// The reflected Flecs string type supplies allocation/copy/destruction hooks.
// Do not assign borrowed string literals to this owned field in native code.
struct Label { char* text; };

// Engine-independent components, reflection and behavior. No Unity dependencies.
void register_core(flecs::world& world);

// Implemented by the application module linked into the same native plugin.
void register_application(flecs::world& world);

} // namespace flecs_unity
