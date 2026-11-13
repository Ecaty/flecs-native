#include "flecs_unity.h"
#include "Modules.hpp"
#include <cstring>
#include <vector>

// Capability coordination. A binding is ready when it is enabled and every prerequisite (CapDependsOn) is applied.
// A consumer is marked CapDirty when it is created, when its set of prerequisites changes, and when a prerequisite's
// result (generation or phase) changes. Observers only note what changed (Flecs locks tables while it reports an add
// or remove); the marks are applied once per frame in PostUpdate, which costs nothing when nothing changed.
namespace flecs_unity {
namespace {

// The module instance of this world (a singleton component, so each world has its own).
CapabilityModule* module_of(ecs_world_t* world) {
    return flecs::world(world).try_get_mut<CapabilityModule>();
}

std::uint64_t result_key(const CapState& state) {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(state.generation)) << 32) |
        static_cast<std::uint32_t>(state.phase);
}

// 1 ready, 0 waiting, -1 failed or cyclic.
int ready(ecs_world_t* world, const CapabilityModule& caps, ecs_entity_t entity, int depth) {
    if (depth > 32) return -1;
    if (const auto* binding = static_cast<const CapBinding*>(ecs_get_id(world, entity, caps.binding)))
        if (!binding->enabled) return 0;
    const ecs_type_t* type = ecs_get_type(world, entity);
    if (!type) return 0;
    for (int i = 0; i < type->count; ++i) {
        const ecs_id_t id = type->array[i];
        if (!ECS_IS_PAIR(id) || ecs_pair_first(world, id) != caps.depends_on) continue;
        const ecs_entity_t target = ecs_pair_second(world, id);
        if (!ecs_is_alive(world, target) || target == entity) return -1;
        const int prerequisite = ready(world, caps, target, depth + 1);
        if (prerequisite <= 0) return prerequisite;
        const auto* result = static_cast<const CapState*>(ecs_get_id(world, target, caps.state));
        if (!result || result->phase == 0) return 0;
        if (result->phase < 0) return -1;
    }
    return 1;
}

// A binding gained or lost a prerequisite.
void dependencies_changed(ecs_iter_t* it) {
    auto* caps = static_cast<CapabilityModule*>(it->ctx);
    for (int i = 0; i < it->count; ++i) caps->changed_consumers.push_back(it->entities[i]);
}

// A prerequisite reported a different result: every binding that depends on it must be applied again.
void result_changed(ecs_iter_t* it) {
    auto* caps = static_cast<CapabilityModule*>(it->ctx);
    const auto* states = static_cast<const CapState*>(ecs_field_w_size(it, sizeof(CapState), 0));
    for (int i = 0; i < it->count; ++i) {
        const ecs_entity_t prerequisite = it->entities[i];
        const std::uint64_t key = result_key(states[i]);
        const auto found = caps->results.find(prerequisite);
        if ((found == caps->results.end() ? 0 : found->second) == key) continue;
        caps->results[prerequisite] = key;
        caps->changed_results.push_back(prerequisite);
    }
}

void mark(ecs_world_t* world, const CapabilityModule& caps, ecs_entity_t consumer) {
    if (ecs_is_alive(world, consumer) && ecs_has_id(world, consumer, caps.binding)) ecs_add_id(world, consumer, caps.dirty);
}

void apply_marks(ecs_world_t* world, CapabilityModule& caps) {
    if (caps.changed_consumers.empty() && caps.changed_results.empty()) return;
    std::vector<ecs_entity_t> consumers;
    consumers.swap(caps.changed_consumers);
    for (const ecs_entity_t prerequisite : caps.changed_results) {
        if (!ecs_is_alive(world, prerequisite)) continue;
        ecs_iter_t each = ecs_each_id(world, ecs_pair(caps.depends_on, prerequisite));
        while (ecs_each_next(&each))
            for (int n = 0; n < each.count; ++n) consumers.push_back(each.entities[n]);
    }
    caps.changed_results.clear();
    for (const ecs_entity_t consumer : consumers) mark(world, caps, consumer);
}

void result_removed(ecs_iter_t* it) {
    auto* caps = static_cast<CapabilityModule*>(it->ctx);
    for (int i = 0; i < it->count; ++i) caps->results.erase(it->entities[i]);
}

} // namespace

CapabilityModule::CapabilityModule(flecs::world& world) {
    // Public contracts live at the root scope; see Modules.hpp.
    const ecs_entity_t scope = ecs_set_scope(world, 0);
    binding = world.component<CapBinding>("CapBinding")
        .member(flecs::Entity, "target")
        .member(&CapBinding::enabled, "enabled")
        .member(&CapBinding::revision, "revision");
    state = world.component<CapState>("CapState")
        .member(&CapState::phase, "phase")
        .member(&CapState::appliedRevision, "appliedRevision")
        .member(&CapState::generation, "generation")
        .member(flecs::String, "message");
    depends_on = world.entity("CapDependsOn");
    dirty = world.entity("CapDirty");
    // Every binding reports a state and starts dirty: added together with the binding, so authors cannot forget
    // either and the first report does not move the entity.
    ecs_add_pair(world, binding, EcsWith, state);
    ecs_add_pair(world, binding, EcsWith, dirty);
    ecs_set_scope(world, scope);
    world.module<CapabilityModule>("::core::cap");

    ecs_observer_desc_t dependencies{};
    dependencies.entity = world.entity("DependenciesChanged");
    dependencies.query.terms[0].id = ecs_pair(depends_on, EcsWildcard);
    dependencies.events[0] = EcsOnAdd;
    dependencies.events[1] = EcsOnRemove;
    dependencies.callback = dependencies_changed;
    dependencies.ctx = this;
    ecs_observer_init(world, &dependencies);

    ecs_observer_desc_t result{};
    result.entity = world.entity("ResultChanged");
    result.query.terms[0].id = state;
    result.events[0] = EcsOnSet;
    result.callback = result_changed;
    result.ctx = this;
    ecs_observer_init(world, &result);

    ecs_observer_desc_t removed{};
    removed.entity = world.entity("ResultRemoved");
    removed.query.terms[0].id = state;
    removed.events[0] = EcsOnRemove;
    removed.callback = result_removed;
    removed.ctx = this;
    ecs_observer_init(world, &removed);

    world.system("MarkDirty")
        .kind(flecs::PostUpdate)
        .immediate()
        .run([this](flecs::iter& it) { apply_marks(real_world(it.world()), *this); });
}

} // namespace flecs_unity

using namespace flecs_unity;

extern "C" {

// The entry points below apply pending marks first, so a caller that reads and then acknowledges a binding sees
// every change noted so far and is not asked to apply it a second time on the next frame.
FU_API int fu_cap_has(fu_context* ctx, uint64_t entity, const char* type) {
    auto* world = static_cast<ecs_world_t*>(fu_world(ctx));
    if (!world || !type || !ecs_is_alive(world, entity)) return 0;
    auto* caps = module_of(world);
    if (caps) apply_marks(world, *caps);
    const ecs_entity_t id = caps && std::strcmp(type, "CapDirty") == 0 ? caps->dirty : ecs_lookup(world, type);
    return id && ecs_has_id(world, entity, id);
}

FU_API void fu_cap_ack(fu_context* ctx, uint64_t entity) {
    auto* world = static_cast<ecs_world_t*>(fu_world(ctx));
    auto* caps = world ? module_of(world) : nullptr;
    if (!caps) return;
    apply_marks(world, *caps);
    if (ecs_is_alive(world, entity)) ecs_remove_id(world, entity, caps->dirty);
}

FU_API int fu_cap_ready(fu_context* ctx, uint64_t entity) {
    auto* world = static_cast<ecs_world_t*>(fu_world(ctx));
    auto* caps = world ? module_of(world) : nullptr;
    if (!caps) return 0;
    apply_marks(world, *caps);
    return ecs_is_alive(world, entity) ? ready(world, *caps, entity, 0) : 0;
}

}
