#include "flecs_unity.h"
#include "FlecsComponents.hpp"
#include <cmath>
#include <cstdio>
#include <exception>
#include <memory>
#include <stdexcept>
#include <cstring>
#include <unordered_map>
#include <string>
#include <vector>

using namespace flecs_unity;

struct fu_context {
    // Members are destroyed in reverse order: query before world.
    flecs::world world;
    flecs::query<const Position> views;
    ecs_http_server_t* rest_server = nullptr;
    char error[4096]{};
    std::unordered_map<std::string, ecs_query_t*> queries;
    ecs_entity_t shell = 0;
    struct Wait { ecs_script_future_t* future; double remaining; };
    std::vector<Wait> waits;
    uint64_t query_builds = 0, query_hits = 0, binary_reads = 0, binary_writes = 0;
    void clear_queries() { for (auto& q : queries) ecs_query_fini(q.second); queries.clear(); }

    ~fu_context() {
        fu_caps_destroy(this);
        for (auto& wait : waits) { ecs_script_future_cancel(wait.future); ecs_script_future_release(wait.future); }
        waits.clear();
        clear_queries();
        if (rest_server) ecs_rest_server_fini(rest_server);
        if (views) views.destruct(); // Named query; explicitly clear its handle.
    }
};

// All exported calls belong to the Unity main thread. C# never sees C++ objects.
static char create_error[4096]{};
static_assert(sizeof(fu_view) == 80, "Unity snapshot ABI changed");

int32_t fu_abi_version(void) { return 2; }
int32_t fu_view_size(void) { return static_cast<int32_t>(sizeof(fu_view)); }
const char* fu_version(void) { return FLECS_VERSION "+" FU_FLECS_REVISION; }
const char* fu_last_error(fu_context* ctx) { return ctx ? ctx->error : create_error; }
void* fu_world(fu_context* ctx) { return ctx ? ctx->world.c_ptr() : nullptr; }
void fu_free(void* str) { if (str) ecs_os_free(str); }

static void fu_wait(const ecs_function_ctx_t* function, int32_t, const ecs_value_t* args, ecs_script_future_t* future) {
    auto* ctx = static_cast<fu_context*>(function->ctx);
    double seconds = *static_cast<const double*>(args[0].ptr);
    if (!std::isfinite(seconds) || seconds < 0 || seconds > 3600 || ctx->waits.size() >= 4096) {
        ecs_script_future_reject(future, "wait expects 0..3600 simulation seconds and a bounded task count");
        ecs_script_future_release(future); return;
    }
    ctx->waits.push_back({future, seconds});
}
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
        ECS_IMPORT(w, FlecsScriptInput);
        ECS_IMPORT(w, FlecsScriptShell);
        ctx->shell = ecs_shell_session_init(w, nullptr);
        ecs_entity_desc_t engine_desc{}; engine_desc.name = "engine";
        ecs_async_function_desc_t wait_desc{};
        wait_desc.name = "wait"; wait_desc.parent = ecs_entity_init(w, &engine_desc);
        wait_desc.params[0].name = "seconds"; wait_desc.params[0].type = ecs_id(ecs_f64_t);
        wait_desc.return_type = ecs_id(ecs_bool_t); wait_desc.callback = fu_wait; wait_desc.ctx = ctx.get();
        ecs_async_function_init(w, &wait_desc);

        register_core(ctx->world);
        register_application(ctx->world);
        fu_caps_init(ctx.get());
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

void fu_destroy(fu_context* ctx) { fu_caps_destroy(ctx); delete ctx; }

int32_t fu_tick(fu_context* ctx, float dt) {
    if (!ctx || !std::isfinite(dt) || dt < 0 || dt > 10) return -1;
    if (ctx->rest_server) ecs_http_server_dequeue(ctx->rest_server, dt);
    for (size_t i = 0; i < ctx->waits.size();) {
        auto& wait = ctx->waits[i]; wait.remaining -= dt;
        if (ecs_script_future_status(wait.future) != EcsScriptFuturePending || wait.remaining <= 0) {
            if (ecs_script_future_status(wait.future) == EcsScriptFuturePending) {
                bool completed = true; ecs_value_t result{ecs_id(ecs_bool_t), &completed};
                ecs_script_future_resolve(wait.future, &result);
            }
            ecs_script_future_release(wait.future); ctx->waits.erase(ctx->waits.begin() + i);
        } else ++i;
    }
    // Set outside pipeline progress: reactive scripts may make structural edits.
    auto clock = *ctx->world.try_get<Clock>();
    clock.time += dt;
    clock.delta = dt;
    ++clock.frame;
    ctx->world.set<Clock>(clock);
    bool running = ctx->world.progress(dt);
    fu_caps_tick(ctx, dt);
    return running ? 0 : -1;
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
    const auto entity = ecs_entity_init(ctx->world, &entity_desc);
    if (!entity) {
        std::snprintf(ctx->error, sizeof(ctx->error), "Invalid managed script path");
        return -1;
    }
    ctx->clear_queries(); // Sources may redefine runtime types used by cached expressions.
    int rc = 0;
    if (ecs_has(ctx->world, entity, EcsScript)) {
        rc = ecs_script_reload(ctx->world, entity, code);
    } else {
        ecs_script_desc_t desc{};
        desc.entity = entity;
        desc.code = code;
        rc = ecs_script_init(ctx->world, &desc) ? 0 : -1;
    }
    const auto* script = ecs_get(ctx->world, entity, EcsScript);
    if (rc || !script || script->error) {
        std::snprintf(ctx->error, sizeof(ctx->error), "%s",
            script && script->error ? script->error : "Managed Script failed");
        return -1;
    }
    ctx->error[0] = 0;
    return 0;
}

char* fu_managed_script_source(fu_context* ctx, const char* path) {
    if (!ctx || !path) return nullptr;
    const auto entity = ecs_lookup(ctx->world, path);
    const auto* script = entity ? ecs_get(ctx->world, entity, EcsScript) : nullptr;
    return script && script->code ? ecs_os_strdup(script->code) : nullptr;
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

int32_t fu_write_presented(fu_context* ctx, const fu_view* views, int32_t count, int32_t frame) {
    if (!ctx || count < 0 || (count && !views)) return -1;
    try {
        for (int32_t i = 0; i < count; i++) {
            const auto& v = views[i];
            if (!ecs_is_alive(ctx->world, v.entity)) continue;
            ctx->world.entity(v.entity).set<Presented>({
                {v.px, v.py, v.pz}, {v.rx, v.ry, v.rz}, {v.sx, v.sy, v.sz},
                {v.r, v.g, v.b, v.a}, frame, v.primitive, v.has_label});
        }
        return 0;
    } catch (...) { return -1; }
}

char* fu_entity_name(fu_context* ctx, uint64_t entity) {
    if (!ctx || !ecs_is_alive(ctx->world, entity)) return nullptr;
    const char* name = ctx->world.entity(entity).name();
    return ecs_os_strdup(name ? name : "Entity");
}

char* fu_entity_path(fu_context* ctx, uint64_t entity) {
    if(!ctx || !ecs_is_alive(ctx->world, entity)) return nullptr;
    return ecs_get_path_w_sep(ctx->world,0,entity,".",nullptr);
}

char* fu_label(fu_context* ctx, uint64_t entity) {
    if (!ctx || !ecs_is_alive(ctx->world, entity)) return nullptr;
    const auto* label = ctx->world.entity(entity).try_get<Label>();
    return label && label->text ? ecs_os_strdup(label->text) : nullptr;
}

char* fu_query_json(fu_context* ctx, const char* expression) {
    if (!ctx || !expression) return nullptr;
    ecs_query_t* query = nullptr;
    const auto found = ctx->queries.find(expression);
    if (found != ctx->queries.end()) { query = found->second; ++ctx->query_hits; }
    else {
        if (ctx->queries.size() >= 32) ctx->clear_queries();
        ecs_query_desc_t desc{};
        desc.expr = expression;
        desc.cache_kind = EcsQueryCacheAuto;
        desc.flags = EcsQueryDetectChanges;
        query = ecs_query_init(ctx->world, &desc);
        if (!query) {
            std::snprintf(ctx->error, sizeof(ctx->error), "Invalid query: %s", expression);
            return nullptr;
        }
        if (query->cache_kind != EcsQueryCacheNone) ecs_query_changed(query);
        ctx->queries.emplace(expression, query); ++ctx->query_builds;
    }
    auto it = ecs_query_iter(ctx->world, query);
    return ecs_iter_to_json(&it, nullptr);
}

char* fu_query_changes(fu_context* ctx, const char* expression) {
    if (!ctx || !expression) return nullptr;
    const auto found = ctx->queries.find(expression);
    if (found == ctx->queries.end()) return fu_query_json(ctx, expression);
    if (found->second->cache_kind != EcsQueryCacheNone && !ecs_query_changed(found->second)) return nullptr;
    return fu_query_json(ctx, expression);
}
int32_t fu_set_component_json(fu_context* ctx, const char* entity, const char* type, const char* json) {
    auto e = fu_lookup(ctx, entity), t = fu_lookup(ctx, type);
    if (!e || !t || !json) return -1;
    const auto* info = ecs_get_type_info(ctx->world, t);
    if (!info || info->size <= 0) return -1;
    ecs_value_t value = ecs_value_new(ctx->world, t);
    if (!value.ptr) return -1;
    const char* end = ecs_ptr_from_json(ctx->world, t, value.ptr, json, nullptr);
    if (!end) { ecs_value_fini(ctx->world, &value); return -1; }
    ecs_set_id(ctx->world, e, t, ecs_get_type_info(ctx->world, t)->size, value.ptr);
    ecs_value_fini(ctx->world, &value); return 0;
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

// Runtime type discovery and POD transfer: no product-specific C++ component is required.
static bool fu_plain(ecs_world_t* w, ecs_entity_t type, int depth = 0) {
    if (depth > 8) return false;
    if (const auto* p = ecs_get(w, type, EcsPrimitive))
        return p->kind != EcsString && p->kind != EcsUPtr && p->kind != EcsIPtr;
    if (ecs_has(w, type, EcsEnum) || ecs_has(w, type, EcsBitmask)) return true;
    if (const auto* a = ecs_get(w, type, EcsArray)) return fu_plain(w, a->type, depth + 1);
    if (const auto* st = ecs_get(w, type, EcsStruct)) {
        const auto* members = static_cast<const ecs_member_t*>(ecs_vec_first(&st->members));
        for (int i = 0; i < ecs_vec_count(&st->members); ++i)
            if (!fu_plain(w, members[i].type, depth + 1)) return false;
        return true;
    }
    return false;
}
uint64_t fu_lookup(fu_context* ctx, const char* path) {
    return ctx && path ? ecs_lookup(ctx->world, path) : 0;
}
int32_t fu_component_size(fu_context* ctx, uint64_t type) {
    if (!ctx || !ecs_is_alive(ctx->world, type) || !fu_plain(ctx->world, type)) return -1;
    const auto* c = ecs_get(ctx->world, type, EcsComponent);
    return c ? c->size : -1;
}
int32_t fu_read_component(fu_context* ctx, uint64_t entity, uint64_t type, void* out, int32_t size) {
    if (!ctx || !out || size <= 0 || size != fu_component_size(ctx, type) || !ecs_is_alive(ctx->world, entity)) return -1;
    const void* value = ecs_get_id(ctx->world, entity, type);
    if (!value) return 0;
    std::memcpy(out, value, size); ++ctx->binary_reads; return 1;
}
int32_t fu_write_components(fu_context* ctx, uint64_t type, const uint64_t* entities, const void* data, int32_t size, int32_t count) {
    if (!ctx || !entities || !data || count < 0 || count > 100000 || size <= 0 || size != fu_component_size(ctx, type)) return -1;
    // Validate the full batch before applying any value.
    for (int i = 0; i < count; ++i) if (!ecs_is_alive(ctx->world, entities[i])) return -1;
    for (int i = 0; i < count; ++i)
        ecs_set_id(ctx->world, entities[i], type, size, static_cast<const char*>(data) + size * i);
    ctx->binary_writes += count; return 0;
}
int32_t fu_delete_entity(fu_context* ctx, const char* path) {
    if (!ctx || !path) return -1;
    auto e = ecs_lookup(ctx->world, path);
    if (!e) return 0;
    ctx->clear_queries(); ecs_delete(ctx->world, e); return 1;
}
int32_t fu_pointer(fu_context* ctx, uint64_t target, float x, float y, uint32_t buttons, uint32_t modifiers) {
    if (!ctx || (target && !ecs_is_alive(ctx->world, target)) || !std::isfinite(x) || !std::isfinite(y)) return -1;
    EcsScriptMouseEvent event{};
    event.target = target; event.screen_x = event.local_x = x; event.screen_y = event.local_y = y;
    event.buttons = buttons; event.ctrl = modifiers & 1; event.shift = modifiers & 2;
    event.alt = modifiers & 4; event.meta = modifiers & 8;
    return ecs_script_mouse_event(ctx->world, &event).resolved;
}
int32_t fu_keyboard(fu_context* ctx, const char* key, int32_t down, int32_t repeat, uint32_t modifiers) {
    if (!ctx || !key) return -1;
    EcsScriptKeyboardEvent event{}; event.key = const_cast<char*>(key);
    event.down = down != 0; event.repeat = repeat != 0; event.ctrl = modifiers & 1;
    event.shift = modifiers & 2; event.alt = modifiers & 4; event.meta = modifiers & 8;
    return ecs_script_keyboard_event(ctx->world, &event).resolved;
}
int32_t fu_script_capture(fu_context* ctx, const char* path) {
    auto e = fu_lookup(ctx, path); return e ? ecs_script_from_scene(ctx->world, e) : -1;
}
int32_t fu_script_undo(fu_context* ctx, const char* path) {
    auto e = fu_lookup(ctx, path); if (!e) return -1;
    ctx->clear_queries(); return ecs_script_undo(ctx->world, e);
}
int32_t fu_script_edit(fu_context* ctx, const char* path, const char* entity, const char* type, const char* expr) {
    if (!ctx || !expr) return -1;
    auto s = fu_lookup(ctx, path), e = fu_lookup(ctx, entity), t = fu_lookup(ctx, type);
    const auto* sc = s ? ecs_get(ctx->world, s, EcsScript) : nullptr;
    if (!sc || !e || !t) return -1;
    auto* edits = ecs_script_edits_new(sc->script);
    int rc = ecs_script_edits_set_expr(edits, e, t, expr);
    char* code = rc ? nullptr : ecs_script_edits_apply(edits);
    ecs_script_edits_free(edits);
    if (!code) { std::snprintf(ctx->error, sizeof(ctx->error), "Source edit could not be applied"); return -1; }
    rc = fu_set_managed_script(ctx, path, code); ecs_os_free(code); return rc;
}
char* fu_shell_json(fu_context* ctx, const char* command) {
    if (!ctx || !command) return nullptr;
    auto result = ecs_shell_run(ctx->world, ctx->shell, command);
    ecs_strbuf_t buf{};
    ecs_strbuf_appendstr(&buf, "{\"results\":[");
    auto* values = static_cast<ecs_value_t*>(ecs_vec_first(&result.results));
    for (int i = 0; i < ecs_vec_count(&result.results); ++i) {
        if (i) ecs_strbuf_appendch(&buf, ',');
        char* json = values[i].ptr ? ecs_ptr_to_json(ctx->world, values[i].type, values[i].ptr) : nullptr;
        ecs_strbuf_appendstr(&buf, json ? json : "null"); ecs_os_free(json);
    }
    ecs_strbuf_appendstr(&buf, "],\"errors\":[");
    auto** errors = static_cast<char**>(ecs_vec_first(&result.errors));
    for (int i = 0; i < ecs_vec_count(&result.errors); ++i) {
        if (i) ecs_strbuf_appendch(&buf, ',');
        char* json = ecs_ptr_to_json(ctx->world, ecs_id(ecs_string_t), &errors[i]);
        ecs_strbuf_appendstr(&buf, json ? json : "null"); ecs_os_free(json);
    }
    ecs_strbuf_appendstr(&buf, "]}"); ecs_shell_result_fini(ctx->world, &result);
    return ecs_strbuf_get(&buf);
}
char* fu_bridge_diagnostics(fu_context* ctx) {
    if (!ctx) return nullptr;
    char result[512];
    std::snprintf(result, sizeof(result), "{\"queryBuilds\":%llu,\"queryHits\":%llu,\"cachedQueries\":%zu,\"binaryReads\":%llu,\"binaryWrites\":%llu}",
        static_cast<unsigned long long>(ctx->query_builds), static_cast<unsigned long long>(ctx->query_hits), ctx->queries.size(),
        static_cast<unsigned long long>(ctx->binary_reads), static_cast<unsigned long long>(ctx->binary_writes));
    return ecs_os_strdup(result);
}




char* fu_component_json(fu_context* ctx, uint64_t entity, const char* type) {
    auto t = fu_lookup(ctx, type);
    if (!t || !ecs_is_alive(ctx->world, entity)) return nullptr;
    const void* value = ecs_get_id(ctx->world, entity, t);
    return value ? ecs_ptr_to_json(ctx->world, t, value) : nullptr;
}
int32_t fu_read_query_data(fu_context* ctx, const char* expression, const char* variable,
    uint64_t* entities, uint64_t* targets, void* values, int32_t size, int32_t capacity) {
    if (!ctx || !expression || size <= 0 || capacity < 0 || (capacity && (!entities || !values))) return -1;
    auto found = ctx->queries.find(expression);
    if (found == ctx->queries.end()) {
        char* json = fu_query_json(ctx, expression); if (!json) return -1; ecs_os_free(json);
        found = ctx->queries.find(expression);
    }
    auto* q = found->second;
    int var = variable && *variable ? ecs_query_find_var(q, variable) : -1;
    if (variable && *variable && var < 0) return -1;
    auto it = ecs_query_iter(ctx->world, q); int total = 0;
    while (ecs_query_next(&it)) {
        ecs_entity_t type = ecs_get_typeid(ctx->world, it.ids[0]);
        if (!ecs_field_is_set(&it, 0) || fu_component_size(ctx, type) != size) { ecs_iter_fini(&it); return -1; }
        const char* data = (it.row_fields & 1) ? nullptr : static_cast<const char*>(ecs_field_w_size(&it, size, 0));
        auto target = var >= 0 ? ecs_iter_get_var(&it, var) : 0;
        for (int i = 0; i < it.count; ++i) {
            int index = total++; if (index >= capacity) continue;
            entities[index] = it.entities[i]; if (targets) targets[index] = target;
            const void* ptr = (it.row_fields & 1) ? ecs_field_at_w_size(&it, size, 0, i) : data + (ecs_field_is_self(&it, 0) ? i * size : 0);
            std::memcpy(static_cast<char*>(values) + index * size, ptr, size);
        }
    }
    ctx->binary_reads += total; return total;
}

static ecs_entity_t fu_named(fu_context* ctx, const char* name) {
    ecs_entity_desc_t desc{}; desc.name = name; desc.sep = "."; desc.root_sep = ".";
    return ecs_entity_init(ctx->world, &desc);
}
uint64_t fu_define_metric(fu_context* ctx, const char* name, const char* component, const char* member, int32_t counter) {
    auto type = fu_lookup(ctx, component); if (!type || !name || !member) return 0;
    ecs_metric_desc_t desc{}; desc.entity = fu_named(ctx, name);
    desc.id = type; desc.dotmember = member; desc.kind = counter ? EcsCounter : EcsGauge;
    return ecs_metric_init(ctx->world, &desc);
}
uint64_t fu_define_alert(fu_context* ctx, const char* name, const char* expression, const char* message) {
    if (!ctx || !name || !expression || !message) return 0;
    ecs_alert_desc_t desc{}; desc.entity = fu_named(ctx, name); desc.query.expr = expression;
    desc.message = message; desc.severity = EcsAlertWarning; desc.retain_period = 1;
    return ecs_alert_init(ctx->world, &desc);
}
int32_t fu_document(fu_context* ctx, const char* path, const char* brief) {
    auto entity = fu_lookup(ctx, path); if (!entity || !brief) return -1;
    ecs_doc_set_brief(ctx->world, entity, brief); return 0;
}
