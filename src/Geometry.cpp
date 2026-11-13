#include "flecs_unity.h"
#include "Modules.hpp"
#include <behaviortree_cpp/contrib/json.hpp>
#include <clipper2/clipper.h>
#include <cmath>
#include <stdexcept>
#include <vector>

// Footprints: the outline of a spline stroke, or the union of a filled area, as NativeFootprint JSON on the same
// entity. Recomputed when a CapSpline value is set and its input really changed; nothing runs for unchanged splines.
namespace flecs_unity {
namespace {

struct NativeFootprint : Label {};

struct SplineInput {
    std::string points;
    double width = 0;
    bool closed = false, fill = false;
};

// CapSpline is defined by the generated capability schema (Script), so its members are read through reflection.
SplineInput read_spline(ecs_world_t* world, ecs_entity_t type, const void* data) {
    SplineInput input;
    ecs_meta_cursor_t cursor = ecs_meta_cursor(world, type, const_cast<void*>(data));
    ecs_meta_push(&cursor);
    if (ecs_meta_member(&cursor, "points") == 0)
        if (const char* points = ecs_meta_get_string(&cursor)) input.points = points;
    if (ecs_meta_member(&cursor, "width") == 0) input.width = ecs_meta_get_float(&cursor);
    if (ecs_meta_member(&cursor, "closed") == 0) input.closed = ecs_meta_get_bool(&cursor);
    if (ecs_meta_member(&cursor, "fill") == 0) input.fill = ecs_meta_get_bool(&cursor);
    return input;
}

std::string footprint_json(const SplineInput& input) {
    try {
        const auto points = nlohmann::json::parse(input.points);
        if (!points.is_array() || points.size() < 2 || points.size() > 256 || !std::isfinite(input.width) ||
            (!input.fill && (input.width <= 0 || input.width > 10000)))
            throw std::runtime_error("Invalid footprint points/width");
        Clipper2Lib::PathD path;
        for (const auto& point : points) {
            const double x = point.at(0).get<double>(), z = point.at(point.size() > 2 ? 2 : 1).get<double>();
            if (!std::isfinite(x) || !std::isfinite(z)) throw std::runtime_error("Nonfinite footprint point");
            path.emplace_back(x, z);
        }
        if ((input.closed || input.fill) && path.size() < 3)
            throw std::runtime_error("Closed footprints need at least three points");
        const auto rings = input.fill
            ? Clipper2Lib::Union(Clipper2Lib::PathsD{path}, Clipper2Lib::FillRule::NonZero, 3)
            : Clipper2Lib::InflatePaths(Clipper2Lib::PathsD{path}, input.width * .5, Clipper2Lib::JoinType::Round,
                input.closed ? Clipper2Lib::EndType::Joined : Clipper2Lib::EndType::Round, 2, 3);
        auto json = nlohmann::json::array();
        for (const auto& ring : rings) {
            auto ring_points = nlohmann::json::array();
            for (const auto& point : ring) ring_points.push_back({point.x, point.y});
            json.push_back(ring_points);
        }
        return json.dump();
    } catch (const std::exception& error) {
        return nlohmann::json{{"error", error.what()}}.dump();
    }
}

void spline_set(ecs_iter_t* it) {
    auto* geometry = static_cast<GeometryModule*>(it->ctx);
    flecs::world world(it->world);
    for (int i = 0; i < it->count; ++i) {
        const ecs_entity_t entity = it->entities[i];
        const void* data = ecs_get_id(real_world(it->world), entity, geometry->spline);
        if (!data) continue;
        const SplineInput input = read_spline(real_world(it->world), geometry->spline, data);
        const std::string key = input.points + '|' + std::to_string(input.width) + (input.closed ? "|closed" : "|open") +
            (input.fill ? "|fill" : "|stroke");
        auto& last = geometry->inputs[entity];
        if (last == key) continue;
        last = key;
        NativeFootprint footprint;
        footprint.text = ecs_os_strdup(footprint_json(input).c_str());
        world.entity(entity).set<NativeFootprint>(footprint);
    }
}

void spline_removed(ecs_iter_t* it) {
    auto* geometry = static_cast<GeometryModule*>(it->ctx);
    for (int i = 0; i < it->count; ++i) geometry->inputs.erase(it->entities[i]);
}

// The spline type appears (and can be replaced) when capability schemas load, so the observers follow it.
void watch_spline_type(ecs_world_t* world, GeometryModule& geometry) {
    const ecs_entity_t type = ecs_lookup(world, "CapSpline");
    if (type == geometry.spline && (!type || ecs_is_alive(world, geometry.observer))) return;
    if (geometry.observer && ecs_is_alive(world, geometry.observer)) ecs_delete(world, geometry.observer);
    geometry.spline = type;
    geometry.observer = 0;
    geometry.inputs.clear();
    if (!type) return;
    geometry.observer = ecs_new(world);
    ecs_observer_desc_t set{};
    set.entity = ecs_new_w_pair(world, EcsChildOf, geometry.observer);
    set.query.terms[0].id = type;
    set.events[0] = EcsOnSet;
    set.yield_existing = true; // splines that existed before their type was seen
    set.callback = spline_set;
    set.ctx = &geometry;
    ecs_observer_init(world, &set);
    ecs_observer_desc_t removed{};
    removed.entity = ecs_new_w_pair(world, EcsChildOf, geometry.observer);
    removed.query.terms[0].id = type;
    removed.events[0] = EcsOnRemove;
    removed.callback = spline_removed;
    removed.ctx = &geometry;
    ecs_observer_init(world, &removed);
}

} // namespace

GeometryModule::GeometryModule(flecs::world& world) {
    const ecs_entity_t scope = ecs_set_scope(world, 0);
    world.component<NativeFootprint>("NativeFootprint").member(flecs::String, "json");
    ecs_set_scope(world, scope);
    world.module<GeometryModule>("::core::geometry");
    world.system("WatchSplineType")
        .kind(flecs::OnLoad)
        .immediate()
        .run([this](flecs::iter& it) { watch_spline_type(real_world(it.world()), *this); });
}

} // namespace flecs_unity

extern "C" {

FU_API int fu_path_outline(const double* points, int count, double radius, double* output, int capacity, int* starts,
    int rings_capacity, int* ring_count) {
    if (!points || count < 2 || count > 4096 || !std::isfinite(radius) || radius <= 0 || radius > 100000) return -1;
    Clipper2Lib::PathD path;
    for (int i = 0; i < count; ++i) {
        if (!std::isfinite(points[i * 2]) || !std::isfinite(points[i * 2 + 1])) return -1;
        path.emplace_back(points[i * 2], points[i * 2 + 1]);
    }
    try {
        const auto rings = Clipper2Lib::InflatePaths(Clipper2Lib::PathsD{path}, radius, Clipper2Lib::JoinType::Round,
            Clipper2Lib::EndType::Round, 2, 3);
        int total = 0;
        for (const auto& ring : rings) total += static_cast<int>(ring.size());
        if (ring_count) *ring_count = static_cast<int>(rings.size());
        if (output && starts && capacity >= total && rings_capacity >= static_cast<int>(rings.size()) + 1) {
            int vertex = 0, ring_index = 0;
            for (const auto& ring : rings) {
                starts[ring_index++] = vertex;
                for (const auto& point : ring) {
                    output[vertex * 2] = point.x;
                    output[vertex * 2 + 1] = point.y;
                    ++vertex;
                }
            }
            starts[ring_index] = vertex;
        }
        return total;
    } catch (...) {
        return -1;
    }
}

}
