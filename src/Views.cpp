#include "flecs_unity.h"
#include "Modules.hpp"

// Presentation snapshots for Unity. An entity with Position is a view. fu_read_views returns every view;
// fu_view_changes returns only the views in tables that changed since the previous call, plus the entities that
// stopped being views, so a frame costs what changed and static scenery costs nothing.
namespace flecs_unity {
namespace {

enum Field { PositionField, RotationField, OrientationField, ScaleField, ColorField, PrimitiveField, LabelField, FieldCount };

ecs_query_t* view_query(flecs::world& world, const char* name, bool detect_changes) {
    const ecs_id_t ids[FieldCount] = {world.id<Position>(), world.id<Rotation>(), world.id<Orientation>(),
        world.id<Scale>(), world.id<Color>(), world.id<Primitive>(), world.id<Label>()};
    ecs_query_desc_t desc{};
    desc.entity = world.entity(name);
    for (int i = 0; i < FieldCount; ++i) {
        desc.terms[i].id = ids[i];
        desc.terms[i].inout = EcsIn; // reading must not mark the data as changed
        if (i != PositionField) desc.terms[i].oper = EcsOptional;
    }
    desc.cache_kind = EcsQueryCacheAuto;
    if (detect_changes) desc.flags = EcsQueryDetectChanges;
    return ecs_query_init(world, &desc);
}

// Calls sink(view) for every entity of the iterator's current table. Fields are read once per table; an inherited
// (shared) field holds one value for the whole table.
template <typename Sink>
void read_rows(ecs_world_t* world, const ViewModule& views, ecs_iter_t& it, Sink&& sink) {
    // Ownership pairs are part of the table's type, so they are looked up once per table.
    uint32_t owned = 0;
    if (ecs_table_has_id(world, it.table, ecs_pair(views.transform_owner, EcsWildcard))) owned |= FU_VIEW_POSE_OWNED;
    if (ecs_table_has_id(world, it.table, ecs_pair(views.appearance_owner, EcsWildcard))) owned |= FU_VIEW_APPEARANCE_OWNED;
    auto column = [&](int field, std::size_t size) -> const void* {
        return ecs_field_is_set(&it, static_cast<int8_t>(field)) ? ecs_field_w_size(&it, size, static_cast<int8_t>(field)) : nullptr;
    };
    auto row = [&](int field, int i) { return ecs_field_is_self(&it, static_cast<int8_t>(field)) ? i : 0; };
    const auto* position = static_cast<const Position*>(column(PositionField, sizeof(Position)));
    const auto* rotation = static_cast<const Rotation*>(column(RotationField, sizeof(Rotation)));
    const auto* orientation = static_cast<const Orientation*>(column(OrientationField, sizeof(Orientation)));
    const auto* scale = static_cast<const Scale*>(column(ScaleField, sizeof(Scale)));
    const auto* color = static_cast<const Color*>(column(ColorField, sizeof(Color)));
    const auto* shape = static_cast<const Primitive*>(column(PrimitiveField, sizeof(Primitive)));
    const bool labeled = ecs_field_is_set(&it, LabelField);
    for (int i = 0; i < it.count; ++i) {
        fu_view view{};
        view.flags = owned;
        view.entity = it.entities[i];
        view.parent = ecs_get_parent(world, view.entity);
        const Position& p = position[row(PositionField, i)];
        view.px = p.x; view.py = p.y; view.pz = p.z;
        if (rotation) { const Rotation& r = rotation[row(RotationField, i)]; view.rx = r.x; view.ry = r.y; view.rz = r.z; }
        if (orientation) {
            const Orientation& q = orientation[row(OrientationField, i)];
            view.qx = q.x; view.qy = q.y; view.qz = q.z; view.qw = q.w;
            view.flags |= FU_VIEW_ORIENTED;
        } else {
            view.qw = 1;
        }
        if (scale) { const Scale& s = scale[row(ScaleField, i)]; view.sx = s.x; view.sy = s.y; view.sz = s.z; }
        else { view.sx = view.sy = view.sz = 1; }
        if (color) { const Color& c = color[row(ColorField, i)]; view.r = c.r; view.g = c.g; view.b = c.b; view.a = c.a; }
        else { view.r = view.g = view.b = view.a = 1; }
        view.primitive = shape ? shape[row(PrimitiveField, i)].kind : -1;
        view.has_label = labeled ? 1 : 0;
        sink(view);
    }
}

// Flecs locks the table while it reports a removal, so the observer only notes the entity.
void view_removed(ecs_iter_t* it) {
    auto* views = static_cast<ViewModule*>(it->ctx);
    for (int i = 0; i < it->count; ++i) views->removed.push_back(it->entities[i]);
}

ViewModule* module_of(fu_context* ctx) {
    auto* world = static_cast<ecs_world_t*>(fu_world(ctx));
    return world ? flecs::world(world).try_get_mut<ViewModule>() : nullptr;
}

} // namespace

ViewModule::ViewModule(flecs::world& world) {
    // Public relationships: one owner per entity. Deleting the owner removes the pair (the Flecs default), which
    // moves the view to another table, so it is reported as changed and Unity takes the pose back.
    const ecs_entity_t scope = ecs_set_scope(world, 0);
    transform_owner = world.entity("TransformOwner").add(flecs::Exclusive);
    appearance_owner = world.entity("AppearanceOwner").add(flecs::Exclusive);
    ecs_set_scope(world, scope);
    world.module<ViewModule>("::core::view");
    all = view_query(world, "All", false);
    changes = view_query(world, "Changes", true);
    ecs_observer_desc_t removal{};
    removal.entity = world.entity("Removed");
    removal.query.terms[0].id = world.id<Position>();
    removal.events[0] = EcsOnRemove;
    removal.callback = view_removed;
    removal.ctx = this;
    ecs_observer_init(world, &removal);
}

} // namespace flecs_unity

using namespace flecs_unity;

extern "C" {

int32_t fu_read_views(fu_context* ctx, fu_view* out, int32_t capacity) {
    auto* views = module_of(ctx);
    if (!views || !views->all || capacity < 0) return -1;
    auto* world = static_cast<ecs_world_t*>(fu_world(ctx));
    int32_t total = 0;
    ecs_iter_t it = ecs_query_iter(world, views->all);
    while (ecs_query_next(&it))
        read_rows(world, *views, it, [&](const fu_view& view) {
            const int32_t index = total++;
            if (out && index < capacity) out[index] = view;
        });
    return total; // May exceed capacity: caller grows the buffer and reads again.
}

const fu_view* fu_view_changes(fu_context* ctx, int32_t* count, const uint64_t** removed, int32_t* removed_count) {
    auto* views = module_of(ctx);
    if (!views || !views->changes || !count || !removed || !removed_count) return nullptr;
    auto* world = static_cast<ecs_world_t*>(fu_world(ctx));
    views->changed.clear();
    ecs_iter_t it = ecs_query_iter(world, views->changes);
    while (ecs_query_next(&it)) {
        if (!ecs_iter_changed(&it)) continue;
        read_rows(world, *views, it, [&](const fu_view& view) { views->changed.push_back(view); });
    }
    views->removed_out.clear();
    views->removed_out.swap(views->removed);
    *count = static_cast<int32_t>(views->changed.size());
    *removed = views->removed_out.data();
    *removed_count = static_cast<int32_t>(views->removed_out.size());
    // Never NULL on success, also when nothing changed: NULL means the call failed.
    static const fu_view none{};
    return views->changed.empty() ? &none : views->changed.data();
}

int32_t fu_claim(fu_context* ctx, uint64_t entity, uint64_t relation, uint64_t owner) {
    auto* world = static_cast<ecs_world_t*>(fu_world(ctx));
    if (!world || !entity || !relation || !owner || !ecs_is_alive(world, entity) || !ecs_is_alive(world, relation) ||
        !ecs_is_alive(world, owner))
        return -1;
    const ecs_entity_t current = ecs_get_target(world, entity, relation, 0);
    if (current && current != owner) return 0;
    if (!current) ecs_add_pair(world, entity, relation, owner);
    return 1;
}

int32_t fu_release(fu_context* ctx, uint64_t entity, uint64_t relation, uint64_t owner) {
    auto* world = static_cast<ecs_world_t*>(fu_world(ctx));
    if (!world || !entity || !relation || !owner || !ecs_is_alive(world, entity) || !ecs_is_alive(world, relation)) return -1;
    if (ecs_get_target(world, entity, relation, 0) != owner) return 0;
    ecs_remove_pair(world, entity, relation, owner);
    return 1;
}

}
