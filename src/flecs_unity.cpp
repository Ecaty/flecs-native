#include "flecs_unity.h"
#include "FlecsComponents.hpp"
#include <cmath>
#include <cstdio>
#include <exception>
#include <memory>
#include <stdexcept>

using namespace flecs_unity;

struct fu_context {
    // Members are destroyed in reverse order: query before world.
    flecs::world world;
    flecs::query<const Position> views;
    ecs_http_server_t* rest_server = nullptr;
    char error[4096]{};

    ~fu_context() {
        if (rest_server) ecs_rest_server_fini(rest_server);
        if (views) views.destruct(); // Named query; explicitly clear its handle.
    }
};

// All exported calls belong to the Unity main thread. C# never sees C++ objects.
static char create_error[4096]{};
static_assert(sizeof(fu_view) == 80, "Unity snapshot ABI changed");

int32_t fu_abi_version(void) { return 1; }
int32_t fu_view_size(void) { return static_cast<int32_t>(sizeof(fu_view)); }
const char* fu_version(void) { return FLECS_VERSION; }
const char* fu_last_error(fu_context* ctx) { return ctx ? ctx->error : create_error; }
void* fu_world(fu_context* ctx) { return ctx ? ctx->world.c_ptr() : nullptr; }
void fu_free(void* str) { if (str) ecs_os_free(str); }

fu_context* fu_create(uint16_t port) {
    create_error[0] = 0;
    try {
        auto ctx = std::make_unique<fu_context>();
        auto* w = ctx->world.c_ptr();
        ecs_doc_set_name(w, EcsWorld, "Ecaty");
        // Units must precede statistics reflection, also on repeated world creation.
        ECS_IMPORT(w, FlecsUnits);
        ECS_IMPORT(w, FlecsMetrics);
        ECS_IMPORT(w, FlecsAlerts);
        ECS_IMPORT(w, FlecsScriptMath);
        ECS_IMPORT(w, FlecsStats);

        register_core(ctx->world);
        register_application(ctx->world);
        ctx->views = ctx->world.query_builder<const Position>("UnityViews")
            .cache_kind(flecs::QueryCacheAuto).build();
        if (!ctx->views) throw std::runtime_error("Could not create UnityViews query");

        if (port) {
            // REST structural edits run outside progress, not in a system callback.
            ecs_http_server_desc_t desc{};
            desc.port = port;
            desc.ipaddr = "127.0.0.1";
            ctx->rest_server = ecs_rest_server_init(w, &desc);
            if (!ctx->rest_server || ecs_http_server_start(ctx->rest_server)) {
                std::snprintf(create_error, sizeof(create_error),
                    "Could not start REST on 127.0.0.1:%u; is the port in use?", port);
                return nullptr; // unique_ptr tears down any partially started world.
            }
        }
        return ctx.release();
    } catch (const std::exception& error) {
        std::snprintf(create_error, sizeof(create_error), "%s", error.what());
    } catch (...) {
        std::snprintf(create_error, sizeof(create_error), "Native initialization failed");
    }
    return nullptr;
}

void fu_destroy(fu_context* ctx) { delete ctx; }

int32_t fu_tick(fu_context* ctx, float dt) {
    if (!ctx || !std::isfinite(dt) || dt < 0 || dt > 10) return -1;
    if (ctx->rest_server) ecs_http_server_dequeue(ctx->rest_server, dt);
    return ctx->world.progress(dt) ? 0 : -1;
}

int32_t fu_run_script(fu_context* ctx, const char* code) {
    if (!ctx || !code) return -1;
    ecs_script_eval_result_t result{};
    const int rc = ecs_script_run(ctx->world, "Unity command", code, &result);
    std::snprintf(ctx->error, sizeof(ctx->error), "%s", result.error ? result.error : "");
    if (result.error) ecs_os_free(result.error);
    return rc;
}

int32_t fu_set_managed_script(fu_context* ctx, const char* path, const char* code) {
    if (!ctx || !path || !code) return -1;
    ecs_entity_desc_t entity_desc{};
    entity_desc.name = path;
    entity_desc.sep = ".";
    entity_desc.root_sep = ".";
    ecs_script_desc_t script_desc{};
    script_desc.entity = ecs_entity_init(ctx->world, &entity_desc);
    script_desc.code = code;
    const auto result = ecs_script_init(ctx->world, &script_desc);
    const auto* script = result ? ecs_get(ctx->world, result, EcsScript) : nullptr;
    if (!script || script->error) {
        std::snprintf(ctx->error, sizeof(ctx->error),
            "Managed Script failed; inspect the Unity Console / Flecs log.");
        return -1;
    }
    ctx->error[0] = 0;
    return 0;
}

int32_t fu_read_views(fu_context* ctx, fu_view* out, int32_t capacity) {
    if (!ctx || capacity < 0) return -1;
    int32_t total = 0;
    ctx->views.each([&](flecs::entity entity, const Position& position) {
        const auto index = total++;
        if (!out || index >= capacity) return;
        const auto* rotation = entity.try_get<Rotation>();
        const auto* scale = entity.try_get<Scale>();
        const auto* color = entity.try_get<Color>();
        const auto* shape = entity.try_get<Primitive>();
        fu_view view{};
        view.entity = entity.id();
        view.parent = entity.target(flecs::ChildOf).id();
        view.px = position.x; view.py = position.y; view.pz = position.z;
        if (rotation) { view.rx = rotation->x; view.ry = rotation->y; view.rz = rotation->z; }
        view.sx = scale ? scale->x : 1; view.sy = scale ? scale->y : 1; view.sz = scale ? scale->z : 1;
        view.r = color ? color->r : 1; view.g = color ? color->g : 1;
        view.b = color ? color->b : 1; view.a = color ? color->a : 1;
        view.primitive = shape ? shape->kind : -1;
        view.has_label = entity.has<Label>() ? 1 : 0;
        out[index] = view;
    });
    return total; // May exceed capacity: caller grows the buffer and reads again.
}

char* fu_entity_name(fu_context* ctx, uint64_t entity) {
    if (!ctx || !ecs_is_alive(ctx->world, entity)) return nullptr;
    const char* name = ctx->world.entity(entity).name();
    return ecs_os_strdup(name ? name : "Entity");
}

char* fu_label(fu_context* ctx, uint64_t entity) {
    if (!ctx || !ecs_is_alive(ctx->world, entity)) return nullptr;
    const auto* label = ctx->world.entity(entity).try_get<Label>();
    return label && label->text ? ecs_os_strdup(label->text) : nullptr;
}

char* fu_query_json(fu_context* ctx, const char* expression) {
    if (!ctx || !expression) return nullptr;
    ecs_query_desc_t desc{};
    desc.expr = expression;
    auto* query = ecs_query_init(ctx->world, &desc);
    if (!query) {
        std::snprintf(ctx->error, sizeof(ctx->error), "Invalid query: %s", expression);
        return nullptr;
    }
    auto it = ecs_query_iter(ctx->world, query);
    auto* json = ecs_iter_to_json(&it, nullptr);
    ecs_query_fini(query);
    return json;
}

char* fu_entity_json(fu_context* ctx, const char* path) {
    if (!ctx || !path) return nullptr;
    const auto entity = ecs_lookup(ctx->world, path);
    if (!entity) {
        std::snprintf(ctx->error, sizeof(ctx->error), "Entity not found: %s", path);
        return nullptr;
    }
    ecs_entity_to_json_desc_t desc = ECS_ENTITY_TO_JSON_INIT;
    desc.serialize_entity_id = true;
    desc.serialize_type_info = true;
    return ecs_entity_to_json(ctx->world, entity, &desc);
}
