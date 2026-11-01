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
} fu_view;
FU_API int32_t fu_abi_version(void);
FU_API int32_t fu_view_size(void);
FU_API const char *fu_version(void);
FU_API fu_context *fu_create(uint16_t port); /* 0 disables REST for tests */
FU_API void fu_destroy(fu_context *ctx);
FU_API int32_t fu_tick(fu_context *ctx, float dt);
FU_API int32_t fu_run_script(fu_context *ctx, const char *code);
FU_API int32_t fu_set_managed_script(fu_context *ctx, const char *path, const char *code);
FU_API const char *fu_last_error(fu_context *ctx);
FU_API int32_t fu_read_views(fu_context *ctx, fu_view *out, int32_t capacity);
FU_API char *fu_entity_name(fu_context *ctx, uint64_t entity);
FU_API char *fu_label(fu_context *ctx, uint64_t entity);
FU_API char *fu_query_json(fu_context *ctx, const char *expression);
FU_API char *fu_entity_json(fu_context *ctx, const char *path);
FU_API void fu_free(void *str);
FU_API void *fu_world(fu_context *ctx); /* Advanced native extension point; main thread only. */
#ifdef __cplusplus
}
#endif
#endif
