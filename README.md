# Flecs native runtime

Shared C++17 runtime for Ecaty-compatible frontends: reflected components, world lifecycle, runtime Flecs Script/queries, loopback Explorer/REST and a stable C ABI. Upstream Flecs v4.1.6 is vendored unmodified as `vendor/flecs.c`, `vendor/flecs.h` and its MIT license.

Build this module with an application that implements `flecs_unity::register_application(flecs::world&)`; Ecaty's `Native` build composes it with `ecaty-native` into one plugin. No Unity SDK is required in this source.

Canonical project intent, ownership and continuation instructions: [Ecaty AGENTS](https://github.com/Ecaty/Ecaty/blob/main/AGENTS.md), [PROJECT](https://github.com/Ecaty/Ecaty/blob/main/docs/PROJECT.md), [DEVELOPMENT](https://github.com/Ecaty/Ecaty/blob/main/docs/DEVELOPMENT.md), [STATUS](https://github.com/Ecaty/Ecaty/blob/main/docs/STATUS.md).
