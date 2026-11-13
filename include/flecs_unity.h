#ifndef FLECS_UNITY_H
#define FLECS_UNITY_H
#include <stdint.h>
#ifdef _WIN32
#define FU_API __declspec(dllexport)
#else
#define FU_API __attribute__((visibility("default")))
#endif
#ifdef __cplusplus
extern "C" {
#endif
typedef struct fu_context fu_context;
/* Fixed blittable ABI. No Flecs internals or owned strings cross this boundary. */
typedef struct fu_view {
    uint64_t entity;
    uint64_t parent;
    float px, py, pz;
    float rx, ry, rz;
    float sx, sy, sz;
    float r, g, b, a;
    int32_t primitive; /* -1 = empty transform; 0..5 = cube/sphere/capsule/cylinder/plane/quad */
    int32_t has_label;
    float qx, qy, qz, qw; /* Orientation quaternion; use it instead of the Euler rotation when FU_VIEW_ORIENTED is set */
    uint32_t flags;       /* FU_VIEW_* */
} fu_view;
#define FU_VIEW_ORIENTED 1u /* the entity has an Orientation: qx..qw are its rotation */
#define FU_VIEW_POSE_OWNED 2u /* (TransformOwner, owner): the owner moves the view; do not write its pose */
#define FU_VIEW_APPEARANCE_OWNED 4u /* (AppearanceOwner, owner): the owner draws the view; do not write its colour */
/* A fact Unity reports to the world, delivered as a Flecs event on `entity` (observers read it as their parameter). */
typedef struct fu_event {
    uint64_t event;  /* the event entity, for example one found with fu_lookup */
    uint64_t entity; /* the entity the fact is about */
    uint64_t other;  /* a second entity (what was touched, where it arrived), or 0 */
    float x, y, z;   /* a point or direction, when the fact has one */
    float value;
} fu_event;
/* Runtime capability extension; world calls stay on the simulation thread. */
FU_API int fu_cap_has(fu_context*,uint64_t entity,const char* type);
FU_API void fu_cap_ack(fu_context*,uint64_t entity);
FU_API int fu_cap_ready(fu_context*,uint64_t entity);
FU_API void* fu_audio_create(const void* bank, int bytes, int sample_rate);
FU_API void fu_audio_destroy(void* renderer);
FU_API int fu_audio_note(void* renderer,int kind,int note,float velocity,int program,uint64_t sample);
FU_API void fu_audio_render(void* renderer,float* output,int count);
FU_API int fu_path_outline(const double* points,int count,double radius,double* output,int capacity,int* starts,int rings_capacity,int* ring_count);
FU_API int32_t fu_abi_version(void);
FU_API int32_t fu_view_size(void);
FU_API const char *fu_version(void);
FU_API fu_context *fu_create(uint16_t port); /* 0 disables REST for tests */
FU_API void fu_destroy(fu_context *ctx);
FU_API int32_t fu_tick(fu_context *ctx, float dt);
FU_API int32_t fu_run_script(fu_context *ctx, const char *code);
FU_API int32_t fu_set_managed_script(fu_context *ctx, const char *path, const char *code);
/* Owned UTF-8 source; NULL when the named managed module does not exist. */
FU_API char *fu_managed_script_source(fu_context *ctx, const char *path);
FU_API const char *fu_last_error(fu_context *ctx);
FU_API int32_t fu_read_views(fu_context *ctx, fu_view *out, int32_t capacity);
/* Presentation acknowledgements; never writes authoritative transform/color. */
FU_API int32_t fu_write_presented(fu_context *ctx, const fu_view *views, int32_t count, int32_t frame);
FU_API char *fu_entity_path(fu_context *ctx, uint64_t entity);
FU_API char *fu_entity_name(fu_context *ctx, uint64_t entity);
FU_API char *fu_label(fu_context *ctx, uint64_t entity);
FU_API char *fu_query_json(fu_context *ctx, const char *expression);
FU_API char *fu_entity_json(fu_context *ctx, const char *path);
FU_API void fu_free(void *str);
FU_API void *fu_world(fu_context *ctx); /* Advanced native extension point; main thread only. */
FU_API char* fu_query_changes(fu_context*, const char* expression);
FU_API int32_t fu_set_component_json(fu_context*, const char* entity, const char* type, const char* json);
FU_API char* fu_component_json(fu_context*, uint64_t entity, const char* type);
FU_API int32_t fu_read_query_data(fu_context*, const char* expression, const char* variable, uint64_t* entities, uint64_t* targets, void* values, int32_t size, int32_t capacity);
FU_API uint64_t fu_define_metric(fu_context*, const char* name, const char* component, const char* member, int32_t counter);
FU_API uint64_t fu_define_alert(fu_context*, const char* name, const char* expression, const char* message);
FU_API int32_t fu_document(fu_context*, const char* path, const char* brief);
FU_API uint64_t fu_lookup(fu_context*, const char* path);
FU_API int32_t fu_component_size(fu_context*, uint64_t type);
FU_API int32_t fu_read_component(fu_context*, uint64_t entity, uint64_t type, void* out, int32_t size);
FU_API int32_t fu_write_components(fu_context*, uint64_t type, const uint64_t* entities, const void* data, int32_t size, int32_t count);
FU_API int32_t fu_delete_entity(fu_context*, const char* path);
FU_API int32_t fu_pointer(fu_context*, uint64_t target, float x, float y, uint32_t buttons, uint32_t modifiers);
FU_API int32_t fu_keyboard(fu_context*, const char* key, int32_t down, int32_t repeat, uint32_t modifiers);
FU_API int32_t fu_script_capture(fu_context*, const char* path);
FU_API int32_t fu_script_undo(fu_context*, const char* path);
FU_API int32_t fu_script_edit(fu_context*, const char* path, const char* entity, const char* type, const char* expr);
FU_API char* fu_shell_json(fu_context*, const char* command);
FU_API char* fu_bridge_diagnostics(fu_context*);

/* ABI 3 additions. */
#define FU_REST_READ_ONLY 1u /* REST refuses requests that modify the world */
/* Like fu_create, with flags. A busy REST port no longer fails creation; see fu_rest_status. */
FU_API fu_context *fu_create_ex(uint16_t port, uint32_t flags);
/* 1 REST running, 0 not requested, -1 could not start (port in use; fu_last_error has details). */
FU_API int32_t fu_rest_status(fu_context*);
/* Stable query handles: valid until destroyed; rebuilt internally only when a referenced component is redefined. */
FU_API int32_t fu_query_create(fu_context*, const char* expression); /* > 0 handle, -1 invalid expression */
FU_API void fu_query_destroy(fu_context*, int32_t handle);
FU_API char* fu_query_json_h(fu_context*, int32_t handle);
FU_API int32_t fu_query_changed(fu_context*, int32_t handle); /* 1 changed since the last read of this handle, 0 unchanged, -1 error */
FU_API int32_t fu_query_read_data(fu_context*, int32_t handle, const char* variable, uint64_t* entities, uint64_t* targets, void* values, int32_t size, int32_t capacity);
/* Copy UTF-8 into a caller buffer (no terminator). Return the full length, which may exceed capacity, or -1. */
FU_API int32_t fu_component_json_into(fu_context*, uint64_t entity, uint64_t type, char* out, int32_t capacity);
FU_API int32_t fu_label_into(fu_context*, uint64_t entity, char* out, int32_t capacity);
FU_API int32_t fu_name_into(fu_context*, uint64_t entity, char* out, int32_t capacity);
/* ABI 4. fu_view carries an orientation quaternion and flags (96 bytes). */
/* Views that changed since the previous call (a whole table is returned when any of its data changed), and the
   entities that stopped being views. The returned arrays belong to the library and stay valid until the next call
   for this context. Returns NULL only on error; the first call returns every view. */
FU_API const fu_view* fu_view_changes(fu_context*, int32_t* count, const uint64_t** removed, int32_t* removed_count);
/* Layout of a reflected type as JSON: {"size","alignment","fields":[{"name","offset","size","kind"}]} with nested
   structs and arrays flattened to their leaves. Copies UTF-8 into the caller buffer like fu_name_into. */
FU_API int32_t fu_type_layout_into(fu_context*, uint64_t type, char* out, int32_t capacity);
/* Delivers each fact as a Flecs event. Returns how many were delivered (dead entities are skipped), or -1. */
FU_API int32_t fu_emit_events(fu_context*, const fu_event* events, int32_t count);
/* Ownership as an exclusive relationship in the world, for example (TransformOwner, binding) on a view: one owner
   per entity and relation, visible to queries. fu_claim returns 1 when `owner` holds the relation afterwards (it was
   free or already theirs), 0 when another owner holds it, -1 for bad arguments. fu_release removes it only when
   `owner` holds it and returns 1 if it did. */
FU_API int32_t fu_claim(fu_context*, uint64_t entity, uint64_t relation, uint64_t owner);
FU_API int32_t fu_release(fu_context*, uint64_t entity, uint64_t relation, uint64_t owner);
/* One-off query for tools: no cache, no query slot. limit > 0 returns at most that many rows. Owned JSON, or NULL
   when the expression is invalid (fu_last_error has details). */
FU_API char* fu_query_once_json(fu_context*, const char* expression, int32_t limit);
#ifdef __cplusplus
}
#endif
#endif
