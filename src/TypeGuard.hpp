#pragma once
#include "flecs.h"
#include <cstdint>
#include <string>

// A Flecs Script that redeclares a type which entities already use, with a different layout, changes the component
// size under live data: Flecs reports "component is already in use" and continues, and stored values are corrupted.
// These helpers refuse such a script before it is evaluated, and count the faults Flecs reports for anything that
// still gets through (nested declarations, member entities, REST edits).
namespace flecs_unity {

struct TypeCheck {
    bool declares_types = false; // the script contains struct, enum or template declarations
    std::string problem;         // empty when the script may be evaluated
};

// `owner` is the managed script about to be reloaded (its own types are deleted before evaluation), or 0.
TypeCheck check_type_redeclaration(ecs_world_t* world, const char* code, ecs_entity_t owner);

// Call once before the first world is created.
void install_layout_fault_hook();
// Number of "component is already in use" faults Flecs has reported in this process.
std::uint64_t layout_faults();

} // namespace flecs_unity
