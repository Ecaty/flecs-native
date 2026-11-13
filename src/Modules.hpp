#pragma once
#include "flecs.h"
#include "FlecsComponents.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// Native modules of the shared core.
//
// Names: public components keep their root-scope names (Script, JSON and saves refer to them by that name). Systems,
// observers and module state live inside the module, so the Explorer groups them (core.cap, core.tree, core.geometry).
//
// Phase map, used by every native module (docs/ENGINE-PLAN.md section 3.2):
//   OnLoad      facts from Unity: input, contacts, arrivals; watching for types defined at runtime
//   PreUpdate   intents and timers
//   OnUpdate    simulation: motion, forces, rules, behaviour trees
//   PostUpdate  derived state
//   PreStore    snapshots for Unity
// A reaction to a changed component (capability readiness, footprints) is an observer, not a per-frame system:
// an idle world must not pay for content it is not changing.
namespace flecs_unity {

// Binding contract shared with Script (CapabilitySchema.flecs repeats these layouts for older saves; the type guard
// refuses a script whose declaration differs).
struct CapBinding {
    flecs::entity_t target = 0;
    bool enabled = false;
    std::int32_t revision = 0;
};
// phase: 1 applied, 0 waiting or disabled, -1 failed.
struct CapState {
    std::int32_t phase = 0, appliedRevision = 0, generation = 0;
    char* message = nullptr;
    CapState() = default;
    CapState(const CapState& other)
        : phase(other.phase), appliedRevision(other.appliedRevision), generation(other.generation),
          message(other.message ? ecs_os_strdup(other.message) : nullptr) {}
    CapState(CapState&& other) noexcept
        : phase(other.phase), appliedRevision(other.appliedRevision), generation(other.generation), message(other.message) {
        other.message = nullptr;
    }
    CapState& operator=(const CapState& other) {
        if (this != &other) {
            auto* copy = other.message ? ecs_os_strdup(other.message) : nullptr;
            ecs_os_free(message);
            phase = other.phase; appliedRevision = other.appliedRevision; generation = other.generation; message = copy;
        }
        return *this;
    }
    CapState& operator=(CapState&& other) noexcept {
        if (this != &other) {
            ecs_os_free(message);
            phase = other.phase; appliedRevision = other.appliedRevision; generation = other.generation;
            message = other.message; other.message = nullptr;
        }
        return *this;
    }
    ~CapState() { ecs_os_free(message); }
};

// Dependency readiness and dirty propagation between capability bindings.
struct CapabilityModule {
    explicit CapabilityModule(flecs::world& world);
    flecs::entity_t binding = 0, state = 0, depends_on = 0, dirty = 0;
    // Last (generation, phase) seen per prerequisite: consumers are marked dirty only when a result really changes.
    std::unordered_map<flecs::entity_t, std::uint64_t> results;
    // Noted by observers, applied once per frame: bindings whose prerequisites changed, and prerequisites whose
    // result changed.
    std::vector<flecs::entity_t> changed_consumers, changed_results;
};

// Outlines and filled areas (Clipper2) for spline configurations.
struct GeometryModule {
    explicit GeometryModule(flecs::world& world);
    flecs::entity_t spline = 0, observer = 0;
    std::unordered_map<flecs::entity_t, std::string> inputs; // last input per entity; unchanged input is not recomputed
};

// Behaviour trees (BehaviorTree.CPP) defined as component data.
struct TreeModule {
    explicit TreeModule(flecs::world& world);
    ~TreeModule();
    TreeModule(TreeModule&&) noexcept;
    TreeModule& operator=(TreeModule&&) noexcept;
    struct State;
    std::unique_ptr<State> state;
};

// The world behind a stage: observers and immediate systems read it directly.
inline ecs_world_t* real_world(const ecs_world_t* stage) { return const_cast<ecs_world_t*>(ecs_get_world(stage)); }

// Imports the core modules in dependency order.
void import_modules(flecs::world& world);

// Sets a reflected component from JSON by entity and type path. 0 on success.
int set_component_json(ecs_world_t* world, const char* entity, const char* type, const char* json);

} // namespace flecs_unity
