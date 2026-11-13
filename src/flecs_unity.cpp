#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include "flecs_unity.h"
#include "FlecsComponents.hpp"
#include "TypeGuard.hpp"
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
    int32_t rest_status = 0; // 1 running, 0 off, -1 could not start (port in use)
    char error[4096]{};
    // Stable query slots. A slot is rebuilt from its expression only when a component it references is no longer alive,
    // so managed-script reloads do not flush every cached query.
    struct QuerySlot {
        ecs_query_t* query = nullptr;
        ecs_entity_t entity = 0;
        std::string expression;
        std::vector<ecs_id_t> ids;
        uint16_t generation = 1;
        bool used = false, named = false, fresh = false, rebuilt = false;
    };
    std::vector<QuerySlot> slots;
    std::unordered_map<std::string, int32_t> named_queries;
    std::unordered_map<ecs_entity_t, int32_t> type_sizes;
    std::unordered_map<std::string, ecs_script_t*> parsed;
    ecs_entity_t shell = 0;
    struct Wait { ecs_script_future_t* future; double remaining; };
    std::vector<Wait> waits;
    uint64_t query_builds = 0, query_hits = 0, query_rebuilds = 0, binary_reads = 0, binary_writes = 0;
    uint64_t script_parses = 0, script_reuses = 0;

    void release_query(QuerySlot& slot) {
        // A cached query lives on an entity; if Flecs already deleted that entity, the query is gone too.
        if (slot.query && (!slot.entity || ecs_is_alive(world, slot.entity))) ecs_query_fini(slot.query);
        slot.query = nullptr;
        slot.entity = 0;
        slot.ids.clear();
    }
    void clear_parsed() {
        for (auto& entry : parsed) ecs_script_free(entry.second);
        parsed.clear();
    }
    // Sources may redefine runtime types: drop derived caches. Query slots validate themselves on use.
    void sources_changed() {
        type_sizes.clear();
        clear_parsed();
        // Keep the queries, but report every one as changed once: consumers of change detection (for example model
        // reconciliation) relied on a full refresh after each source change when all queries were flushed.
        for (auto& slot : slots) if (slot.used) slot.rebuilt = true;
    }

    ~fu_context() {
        fu_caps_destroy(this);
        for (auto& wait : waits) { ecs_script_future_cancel(wait.future); ecs_script_future_release(wait.future); }
        waits.clear();
        clear_parsed();
        for (auto& slot : slots) release_query(slot);
        slots.clear();
        if (rest_server) ecs_rest_server_fini(rest_server);
        if (views) views.destruct(); // Named query; explicitly clear its handle.
    }
};

// All exported calls belong to the Unity main thread. C# never sees C++ objects.
static char create_error[4096]{};
static_assert(sizeof(fu_view) == 80, "Unity snapshot ABI changed");

int32_t fu_abi_version(void) { return 3; }
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
// Flecs binds on its server thread, so a busy port is only logged there. Probe it first so the caller can know.
static bool fu_port_available(uint16_t port) {
#ifdef _WIN32
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return true;
    SOCKET probe = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    bool available = true;
    if (probe != INVALID_SOCKET) {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        available = bind(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
        closesocket(probe);
    }
    WSACleanup();
    return available;
#else
    int probe = socket(AF_INET, SOCK_STREAM, 0);
    if (probe < 0) return true;
    int reuse = 1; // ignore TIME_WAIT leftovers from a previous world; an active listener still blocks bind
    setsockopt(probe, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const bool available = bind(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
    close(probe);
    return available;
#endif
}

fu_context* fu_create_ex(uint16_t port, uint32_t flags) {
    create_error[0] = 0;
    try {
        install_layout_fault_hook(); // before the first world: the OS API is fixed once a world exists
        auto ctx = std::make_unique<fu_context>();
        auto* w = ctx->world.c_ptr();
        ecs_doc_set_name(w, EcsWorld, "World");
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
        // Presentation snapshot: one cached query reads every field per table instead of per-entity lookups.
        ctx->views = ctx->world.query_builder<const Position>("UnityViews")
            .with<Rotation>().optional().in()
            .with<Scale>().optional().in()
            .with<Color>().optional().in()
            .with<Primitive>().optional().in()
            .with<Label>().optional().in()
            .cache_kind(flecs::QueryCacheAuto).build();
        if (!ctx->views) throw std::runtime_error("Could not create UnityViews query");

        if (port) {
            // REST structural edits run outside progress, not in a system callback.
            ecs_http_server_desc_t desc{};
            desc.port = port;
            desc.ipaddr = "127.0.0.1";
            ctx->rest_server = fu_port_available(port) ? ecs_rest_server_init(w, &desc) : nullptr;
            if (ctx->rest_server && (flags & FU_REST_READ_ONLY)) {
                auto* rest = static_cast<ecs_rest_ctx_t*>(ecs_http_server_ctx(ctx->rest_server));
                if (rest) rest->readonly = true;
            }
            if (!ctx->rest_server || ecs_http_server_start(ctx->rest_server)) {
                // A busy port (another instance or Flecs app) only disables inspection; the world still runs.
                if (ctx->rest_server) ecs_rest_server_fini(ctx->rest_server);
                ctx->rest_server = nullptr;
                ctx->rest_status = -1;
                std::snprintf(ctx->error, sizeof(ctx->error),
                    "Could not start REST on 127.0.0.1:%u; is the port in use?", port);
            } else ctx->rest_status = 1;
        }
        return ctx.release();
    } catch (const std::exception& error) {
        std::snprintf(create_error, sizeof(create_error), "%s", error.what());
    } catch (...) {
        std::snprintf(create_error, sizeof(create_error), "Native initialization failed");
    }
    return nullptr;
}

fu_context* fu_create(uint16_t port) { return fu_create_ex(port, 0); }
int32_t fu_rest_status(fu_context* ctx) { return ctx ? ctx->rest_status : 0; }

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

// Refuses a script that would change the layout of a type entities already use (TypeGuard.hpp).
// Returns false with ctx->error set. `owner` is the managed script being reloaded, or 0.
static bool fu_types_allowed(fu_context* ctx, const char* code, ecs_entity_t owner, bool* declares_types = nullptr) {
    const TypeCheck check = check_type_redeclaration(ctx->world, code, owner);
    if (declares_types) *declares_types = check.declares_types;
    if (check.problem.empty()) return true;
    std::snprintf(ctx->error, sizeof(ctx->error), "%s", check.problem.c_str());
    return false;
}
// Second line of defence for what the check above cannot see (nested declarations, member entities): Flecs reported
// that a component in use changed. The script ran, so the caller is told the world may be inconsistent.
static int fu_layout_fault(fu_context* ctx, uint64_t faults_before, int rc) {
    if (layout_faults() == faults_before) return rc;
    std::snprintf(ctx->error, sizeof(ctx->error),
        "A component that entities use changed layout during this script. Stored values may be wrong; reload the world.");
    return -1;
}

// Repeated commands reuse their parsed script. The cache is dropped whenever managed sources change,
// because a parse resolves names that a source reload may redefine.
int32_t fu_run_script(fu_context* ctx, const char* code) {
    if (!ctx || !code) return -1;
    ecs_script_eval_result_t result{};
    const size_t length = std::strlen(code);
    const uint64_t faults = layout_faults();
    int rc = -1;
    if (length > 4096) {
        if (!fu_types_allowed(ctx, code, 0)) return -1;
        rc = ecs_script_run(ctx->world, "Unity command", code, &result);
    } else {
        std::string key(code, length);
        ecs_script_t* script = nullptr;
        const auto found = ctx->parsed.find(key);
        if (found != ctx->parsed.end()) {
            script = found->second;
            ++ctx->script_reuses;
        } else {
            // Checked once per text: a script that declares types is never cached, so it is checked on every run.
            bool declares_types = false;
            if (!fu_types_allowed(ctx, code, 0, &declares_types)) return -1;
            if (declares_types) {
                rc = ecs_script_run(ctx->world, "Unity command", code, &result);
            } else {
                script = ecs_script_parse(ctx->world, "Unity command", code, nullptr, &result);
                ++ctx->script_parses;
                if (script) {
                    if (ctx->parsed.size() >= 64) ctx->clear_parsed();
                    ctx->parsed.emplace(std::move(key), script);
                }
            }
        }
        if (script) rc = ecs_script_eval(script, nullptr, &result);
    }
    std::snprintf(ctx->error, sizeof(ctx->error), "%s", result.error ? result.error : (rc ? "Script failed" : ""));
    if (result.error) ecs_os_free(result.error);
    return fu_layout_fault(ctx, faults, rc);
}

int32_t fu_set_managed_script(fu_context* ctx, const char* path, const char* code) {
    if (!ctx || !path || !code) return -1;
    // A reload deletes the types this script itself declared before evaluating; any other type in use must keep its layout.
    const ecs_entity_t existing = ecs_lookup(ctx->world, path);
    const ecs_entity_t owner = existing && ecs_has(ctx->world, existing, EcsScript) ? existing : 0;
    if (!fu_types_allowed(ctx, code, owner)) return -1;
    const uint64_t faults = layout_faults();
    ecs_entity_desc_t entity_desc{};
    entity_desc.name = path;
    entity_desc.sep = ".";
    entity_desc.root_sep = ".";
    const auto entity = ecs_entity_init(ctx->world, &entity_desc);
    if (!entity) {
        std::snprintf(ctx->error, sizeof(ctx->error), "Invalid managed script path");
        return -1;
    }
    ctx->sources_changed();
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
    return fu_layout_fault(ctx, faults, 0);
}

char* fu_managed_script_source(fu_context* ctx, const char* path) {
    if (!ctx || !path) return nullptr;
    const auto entity = ecs_lookup(ctx->world, path);
    const auto* script = entity ? ecs_get(ctx->world, entity, EcsScript) : nullptr;
    return script && script->code ? ecs_os_strdup(script->code) : nullptr;
}

int32_t fu_read_views(fu_context* ctx, fu_view* out, int32_t capacity) {
    if (!ctx || capacity < 0) return -1;
    ecs_world_t* w = ctx->world.c_ptr();
    int32_t total = 0;
    ecs_iter_t it = ecs_query_iter(w, ctx->views.c_ptr());
    while (ecs_query_next(&it)) {
        const auto* position = static_cast<const Position*>(ecs_field_w_size(&it, sizeof(Position), 0));
        const auto* rotation = ecs_field_is_set(&it, 1) ? static_cast<const Rotation*>(ecs_field_w_size(&it, sizeof(Rotation), 1)) : nullptr;
        const auto* scale = ecs_field_is_set(&it, 2) ? static_cast<const Scale*>(ecs_field_w_size(&it, sizeof(Scale), 2)) : nullptr;
        const auto* color = ecs_field_is_set(&it, 3) ? static_cast<const Color*>(ecs_field_w_size(&it, sizeof(Color), 3)) : nullptr;
        const auto* shape = ecs_field_is_set(&it, 4) ? static_cast<const Primitive*>(ecs_field_w_size(&it, sizeof(Primitive), 4)) : nullptr;
        const bool labeled = ecs_field_is_set(&it, 5);
        // Inherited (shared) fields hold one value for the whole table.
        const bool ps = ecs_field_is_self(&it, 0);
        const bool rs = rotation && ecs_field_is_self(&it, 1), ss = scale && ecs_field_is_self(&it, 2);
        const bool cs = color && ecs_field_is_self(&it, 3), ks = shape && ecs_field_is_self(&it, 4);
        for (int32_t i = 0; i < it.count; ++i) {
            const auto index = total++;
            if (!out || index >= capacity) continue;
            const auto entity = it.entities[i];
            const auto& p = position[ps ? i : 0];
            fu_view view{};
            view.entity = entity;
            view.parent = ecs_get_parent(w, entity);
            view.px = p.x; view.py = p.y; view.pz = p.z;
            if (rotation) { const auto& r = rotation[rs ? i : 0]; view.rx = r.x; view.ry = r.y; view.rz = r.z; }
            if (scale) { const auto& c = scale[ss ? i : 0]; view.sx = c.x; view.sy = c.y; view.sz = c.z; }
            else { view.sx = view.sy = view.sz = 1; }
            if (color) { const auto& c = color[cs ? i : 0]; view.r = c.r; view.g = c.g; view.b = c.b; view.a = c.a; }
            else { view.r = view.g = view.b = view.a = 1; }
            view.primitive = shape ? shape[ks ? i : 0].kind : -1;
            view.has_label = labeled ? 1 : 0;
            out[index] = view;
        }
    }
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

// Copies UTF-8 into a caller buffer, so no owned string crosses the ABI. Returns the byte length without terminator
// (may exceed capacity: the caller grows its buffer and calls again), or -1 when there is no text.
static int32_t fu_copy(const char* text, char* out, int32_t capacity) {
    if (!text) return -1;
    const auto length = static_cast<int32_t>(std::strlen(text));
    if (out && capacity > 0) std::memcpy(out, text, static_cast<size_t>(length < capacity ? length : capacity));
    return length;
}
int32_t fu_label_into(fu_context* ctx, uint64_t entity, char* out, int32_t capacity) {
    if (!ctx || !ecs_is_alive(ctx->world, entity)) return -1;
    const auto* label = ctx->world.entity(entity).try_get<Label>();
    return fu_copy(label ? label->text : nullptr, out, capacity);
}
int32_t fu_name_into(fu_context* ctx, uint64_t entity, char* out, int32_t capacity) {
    if (!ctx || !ecs_is_alive(ctx->world, entity)) return -1;
    return fu_copy(ecs_get_name(ctx->world, entity), out, capacity);
}
int32_t fu_component_json_into(fu_context* ctx, uint64_t entity, uint64_t type, char* out, int32_t capacity) {
    if (!ctx || !type || !ecs_is_alive(ctx->world, entity) || !ecs_is_alive(ctx->world, type)) return -1;
    const void* value = ecs_get_id(ctx->world, entity, type);
    if (!value) return -1;
    char* json = ecs_ptr_to_json(ctx->world, type, value);
    const int32_t length = fu_copy(json, out, capacity);
    ecs_os_free(json);
    return length;
}

// Stable query slots ---------------------------------------------------------------------------------------------------
static int32_t fu_handle(int32_t index, uint16_t generation) {
    return (static_cast<int32_t>(generation & 0x7fff) << 16) | (index + 1);
}
static fu_context::QuerySlot* fu_slot(fu_context* ctx, int32_t handle) {
    const int32_t index = (handle & 0xffff) - 1;
    if (!ctx || handle <= 0 || index < 0 || index >= static_cast<int32_t>(ctx->slots.size())) return nullptr;
    auto& slot = ctx->slots[index];
    return slot.used && (slot.generation & 0x7fff) == ((handle >> 16) & 0x7fff) ? &slot : nullptr;
}
static bool fu_part_alive(ecs_world_t* w, ecs_entity_t e) {
    return e == EcsWildcard || e == EcsAny || e == EcsThis || e == EcsVariable || ecs_get_alive(w, e) != 0;
}
static bool fu_id_alive(ecs_world_t* w, ecs_id_t id) {
    if (ECS_IS_PAIR(id)) return fu_part_alive(w, ECS_PAIR_FIRST(id)) && fu_part_alive(w, ECS_PAIR_SECOND(id));
    return id == EcsWildcard || id == EcsAny || ecs_is_alive(w, id);
}
// Returns a usable query. It is rebuilt from its expression only when a component it references is no longer alive
// (for example a struct redefined by a source reload), instead of flushing every query on each managed-script set.
static ecs_query_t* fu_slot_query(fu_context* ctx, fu_context::QuerySlot& slot) {
    ecs_world_t* w = ctx->world.c_ptr();
    if (slot.query) {
        bool valid = !slot.entity || ecs_is_alive(w, slot.entity);
        for (size_t i = 0; valid && i < slot.ids.size(); ++i) valid = fu_id_alive(w, slot.ids[i]);
        if (valid) {
            ++ctx->query_hits;
            return slot.query;
        }
        ctx->release_query(slot);
        slot.rebuilt = true;
        ++ctx->query_rebuilds;
    }
    ecs_query_desc_t desc{};
    desc.expr = slot.expression.c_str();
    desc.cache_kind = EcsQueryCacheAuto;
    desc.flags = EcsQueryDetectChanges;
    slot.query = ecs_query_init(w, &desc);
    if (!slot.query) {
        std::snprintf(ctx->error, sizeof(ctx->error), "Invalid query: %s", slot.expression.c_str());
        return nullptr;
    }
    slot.entity = slot.query->entity;
    for (int8_t i = 0; i < slot.query->field_count; ++i) slot.ids.push_back(slot.query->ids[i]);
    // Fixed entities in terms (for example a source in `Model(path)`) must also still exist: a recreated entity gets a
    // new id, and the old query would silently match nothing.
    for (int8_t i = 0; i < slot.query->term_count; ++i) {
        const ecs_term_t& term = slot.query->terms[i];
        for (const ecs_term_ref_t* ref : {&term.src, &term.first, &term.second}) {
            if (!(ref->id & EcsIsEntity)) continue;
            const ecs_entity_t e = ECS_TERM_REF_ID(ref);
            if (!e || e == EcsWildcard || e == EcsAny || e == EcsThis || e == EcsVariable) continue;
            const ecs_entity_t alive = ecs_get_alive(w, static_cast<uint32_t>(e));
            if (alive) slot.ids.push_back(alive);
        }
    }
    if (slot.query->cache_kind != EcsQueryCacheNone) ecs_query_changed(slot.query); // change tracking starts here
    ++ctx->query_builds;
    return slot.query;
}
static int32_t fu_slot_create(fu_context* ctx, const char* expression, bool named) {
    int32_t index = -1;
    for (int32_t i = 0; i < static_cast<int32_t>(ctx->slots.size()); ++i)
        if (!ctx->slots[i].used) { index = i; break; }
    if (index < 0) {
        if (ctx->slots.size() >= 0xffff) return -1;
        ctx->slots.emplace_back();
        index = static_cast<int32_t>(ctx->slots.size()) - 1;
    }
    auto& slot = ctx->slots[index];
    slot.used = true;
    slot.named = named;
    slot.fresh = !named; // explicit handles report a first change; named ones keep the previous semantics
    slot.expression = expression;
    if (!fu_slot_query(ctx, slot)) {
        slot.used = false;
        ++slot.generation;
        return -1;
    }
    return fu_handle(index, slot.generation);
}
static void fu_slot_destroy(fu_context* ctx, fu_context::QuerySlot& slot) {
    ctx->release_query(slot);
    slot.used = false;
    slot.named = false;
    slot.expression.clear();
    ++slot.generation;
}
int32_t fu_query_create(fu_context* ctx, const char* expression) {
    return ctx && expression ? fu_slot_create(ctx, expression, false) : -1;
}
void fu_query_destroy(fu_context* ctx, int32_t handle) {
    auto* slot = fu_slot(ctx, handle);
    if (slot && !slot->named) fu_slot_destroy(ctx, *slot);
}
// String-keyed path for tools: each expression maps to a slot that survives source reloads.
static int32_t fu_named_query(fu_context* ctx, const char* expression) {
    const auto found = ctx->named_queries.find(expression);
    if (found != ctx->named_queries.end() && fu_slot(ctx, found->second)) return found->second;
    if (ctx->named_queries.size() >= 32) {
        for (auto& entry : ctx->named_queries)
            if (auto* slot = fu_slot(ctx, entry.second)) fu_slot_destroy(ctx, *slot);
        ctx->named_queries.clear();
    }
    const int32_t handle = fu_slot_create(ctx, expression, true);
    if (handle > 0) ctx->named_queries[expression] = handle;
    return handle;
}
char* fu_query_json_h(fu_context* ctx, int32_t handle) {
    auto* slot = fu_slot(ctx, handle);
    auto* query = slot ? fu_slot_query(ctx, *slot) : nullptr;
    if (!query) return nullptr;
    slot->fresh = slot->rebuilt = false; // a read consumes the pending change report
    auto it = ecs_query_iter(ctx->world, query);
    return ecs_iter_to_json(&it, nullptr);
}
// 1 when matched data changed since the previous check (always 1 for a new or rebuilt query), 0 when not, -1 on error.
int32_t fu_query_changed(fu_context* ctx, int32_t handle) {
    auto* slot = fu_slot(ctx, handle);
    if (!slot) return -1;
    auto* query = fu_slot_query(ctx, *slot);
    if (!query) return -1;
    if (slot->fresh || slot->rebuilt || query->cache_kind == EcsQueryCacheNone) {
        slot->fresh = slot->rebuilt = false;
        return 1;
    }
    return ecs_query_changed(query) ? 1 : 0;
}

char* fu_query_json(fu_context* ctx, const char* expression) {
    if (!ctx || !expression) return nullptr;
    const int32_t handle = fu_named_query(ctx, expression);
    return handle > 0 ? fu_query_json_h(ctx, handle) : nullptr;
}

char* fu_query_changes(fu_context* ctx, const char* expression) {
    if (!ctx || !expression) return nullptr;
    const auto found = ctx->named_queries.find(expression);
    const bool existed = found != ctx->named_queries.end() && fu_slot(ctx, found->second);
    const int32_t handle = fu_named_query(ctx, expression);
    if (handle <= 0) return nullptr;
    if (!existed) return fu_query_json_h(ctx, handle);
    return fu_query_changed(ctx, handle) == 0 ? nullptr : fu_query_json_h(ctx, handle);
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
// Cached per type and cleared when sources change; the reflection walk used to run on every typed read.
int32_t fu_component_size(fu_context* ctx, uint64_t type) {
    if (!ctx || !ecs_is_alive(ctx->world, type)) return -1;
    const auto found = ctx->type_sizes.find(type);
    if (found != ctx->type_sizes.end()) return found->second;
    const auto* c = fu_plain(ctx->world, type) ? ecs_get(ctx->world, type, EcsComponent) : nullptr;
    const int32_t size = c ? c->size : -1;
    ctx->type_sizes.emplace(type, size);
    return size;
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
    ctx->sources_changed(); ecs_delete(ctx->world, e); return 1;
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
    ctx->sources_changed(); return ecs_script_undo(ctx->world, e);
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
    size_t live = 0;
    for (const auto& slot : ctx->slots) live += slot.used ? 1 : 0;
    char result[512];
    std::snprintf(result, sizeof(result),
        "{\"queryBuilds\":%llu,\"queryHits\":%llu,\"queryRebuilds\":%llu,\"cachedQueries\":%zu,\"binaryReads\":%llu,"
        "\"binaryWrites\":%llu,\"scriptParses\":%llu,\"scriptReuses\":%llu,\"layoutFaults\":%llu,\"rest\":%d}",
        static_cast<unsigned long long>(ctx->query_builds), static_cast<unsigned long long>(ctx->query_hits),
        static_cast<unsigned long long>(ctx->query_rebuilds), live,
        static_cast<unsigned long long>(ctx->binary_reads), static_cast<unsigned long long>(ctx->binary_writes),
        static_cast<unsigned long long>(ctx->script_parses), static_cast<unsigned long long>(ctx->script_reuses),
        static_cast<unsigned long long>(layout_faults()),
        static_cast<int>(ctx->rest_status));
    return ecs_os_strdup(result);
}

char* fu_component_json(fu_context* ctx, uint64_t entity, const char* type) {
    auto t = fu_lookup(ctx, type);
    if (!t || !ecs_is_alive(ctx->world, entity)) return nullptr;
    const void* value = ecs_get_id(ctx->world, entity, t);
    return value ? ecs_ptr_to_json(ctx->world, t, value) : nullptr;
}
int32_t fu_query_read_data(fu_context* ctx, int32_t handle, const char* variable,
    uint64_t* entities, uint64_t* targets, void* values, int32_t size, int32_t capacity) {
    if (!ctx || size <= 0 || capacity < 0 || (capacity && (!entities || !values))) return -1;
    auto* slot = fu_slot(ctx, handle);
    auto* q = slot ? fu_slot_query(ctx, *slot) : nullptr;
    if (!q) return -1;
    slot->fresh = slot->rebuilt = false; // a read consumes the pending change report
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
int32_t fu_read_query_data(fu_context* ctx, const char* expression, const char* variable,
    uint64_t* entities, uint64_t* targets, void* values, int32_t size, int32_t capacity) {
    if (!ctx || !expression) return -1;
    const int32_t handle = fu_named_query(ctx, expression);
    return handle > 0 ? fu_query_read_data(ctx, handle, variable, entities, targets, values, size, capacity) : -1;
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
