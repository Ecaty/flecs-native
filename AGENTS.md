# Native runtime core: start here

Shared C++17 Flecs runtime behind a stable C ABI (`fu_*`). It is the core of the runtime engine that every product builds on, so it must work for any product type and contain no product assumptions. This repo stands alone: this file, [BACKLOG](BACKLOG.md) and [docs/](docs/) are enough to work here. In a product checkout it is the `Native/Core` submodule; the product repo's `docs/README.md` is the cross-repo overview and links every repo's backlog.

## Read on demand

- [BACKLOG](BACKLOG.md): open engine tasks and decisions for this repo.
- [README](README.md): what the module contains and how it builds.
- [docs/FLECS-NOTES](docs/FLECS-NOTES.md): Flecs 4.2 features by topic, with recommendations and their status.
- [docs/FLECS-ARCHITECTURE-AUDIT](docs/FLECS-ARCHITECTURE-AUDIT.md): measured native/bridge audit and the decided native vs Script split.
- [docs/RUNTIME-STACK](docs/RUNTIME-STACK.md): selected runtime stack per area. [docs/RUNTIME-COMPOSITION-DESIGN](docs/RUNTIME-COMPOSITION-DESIGN.md): composition design notes.
- [docs/THIRD-PARTY](docs/THIRD-PARTY.md): vendor submodules, checkout, weekly upstream check and the update procedure.
- `include/FlecsComponents.hpp`: module convention and phase map.

## Rules

- Flecs C++17 for shared components and systems. Systems and observers live in `engine.<feature>` modules; components stay at the root (Script, C# and JSON use bare names).
- Third-party code is in `vendor/` as submodules of Ecaty forks; never edit it in place (see THIRD-PARTY).
- Preserve the `fu_*` C ABI and the owned UTF-8 string contract; new exports are additive. Core has no Unity dependency.
- Product-specific behavior belongs in the product's application module, never here. No product names in new identifiers.
- World/query/Script operations run on the owning thread; REST edits are dequeued outside pipeline progress. Keep addon import order and repeated-world lifetime tests.
- Do not add a second simulation runtime or implement Jolt, networking or another provider without an agreed task. Never store credentials in an ECS world or repository.

## Working across repos

- Engine tasks go in this repo's BACKLOG; product tasks in the product's `docs/BACKLOG.md`. Keep both short and mark work done only with current evidence.
- Commit and push a feature branch here first, then update the pointer in the product repo.
- Verify through the host product: its native build (`Native/Build.ps1`), its Python native smokes (`Verification/*_smoke.py`) and, for ABI or behavior changes, its full Unity suite.
- Links to `https://github.com/Ecaty/Ecaty/blob/main/...` are the first product's docs; in a composed checkout they are at `../../`.
