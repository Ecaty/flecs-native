# Native runtime instructions

Read README.md and Ecaty's canonical [PROJECT](https://github.com/Ecaty/Ecaty/blob/main/docs/PROJECT.md), [DEVELOPMENT](https://github.com/Ecaty/Ecaty/blob/main/docs/DEVELOPMENT.md) and [STATUS](https://github.com/Ecaty/Ecaty/blob/main/docs/STATUS.md). In the composed checkout they are at `../../docs/`.

Use Flecs C++17 for shared components/systems; keep the tagged upstream C sources unmodified and paired. Preserve the `fu_*` C ABI and owned UTF-8 string contract. Core has no Unity dependency. Product-specific behavior belongs in the application module. World/query/Script operations run on the owning thread; REST edits are dequeued outside pipeline progress. Keep addon import order and repeated-world lifetime tests. Do not add a second simulation runtime or implement Jolt implicitly. Never store credentials in an ECS world or repository. Run the relevant root verification scripts after boundary changes.
