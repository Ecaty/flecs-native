# Runtime composition design notes

Discussion only; the folder/package restructuring does not implement the catalog, broad core redesign, networking or proposed visual effects below.

## Native engine split

This is an architectural proposal, not an implementation change.

| Layer | Owns |
|---|---|
| Compiled native core | stable reflected data/contracts; entity/definition identity; ownership/dependencies; activation/lifetime; event/time scheduling; shared resource references; reusable hot simulation/query algorithms; validation/metrics interfaces |
| Shipped versioned packs | Flecs modules, prefabs/templates, reusable rules/behaviors, interaction patterns, registered asset/plugin capability recipes, defaults and migrations |
| Runtime experiences | instances, relationships, parameter overrides, goals/rules, user-defined reflected components/templates and selected saved state; authored by the same engine tools whether human or AI |
| Unity and optional adapters | rendering, input sampling, camera presentation, audio, physics/animation/plugin execution and measured outputs; one transform/simulation owner per controlled target |

Flecs Script already executes in the native Flecs runtime. It is not a second C# ECS. Compile reusable, measured hot loops/algorithms; use Script for composition, parameters, templates, reactivity and higher-level orchestration. New compiled algorithms still need registered native systems or supported adapters. Avoid a distinct generic application scripting VM or arbitrary Inspector reflection.

Performance may improve from typed native systems, cached queries, explicit activation, batched ABI transfers and fewer JSON/structural changes. Native compilation alone is not proof: rendering, bridge traffic, reactive fan-out and plugin work may dominate. Benchmark the same representative scenarios before/after. Do not move every rule into C++ just because it is native; Script is already native and its reactive dependency tracking can skip redundant work.

Current bridge: Shell sessions, reflected functions, queries/change detection, managed reactive Script, prefabs, metrics and selected native algorithms are integrated. Templates/interfaces/async per-instance state were proven in focused checks. Full product use is incomplete: the gallery still has C# orchestration, handwritten Script construction and a fixed primitive snapshot vocabulary. The next substantive runtime step should be a queryable definition/capability catalog, dependency-aware activation and scoped source/state persistence, not a cosmetic folder rename pretending those systems exist.

## What players can create

Casual players should mostly compose: choose/place objects, connect recognizable interactions, change parameters and apply reusable behaviors. Shipped packs could supply characters, triggers, inventory, timers, objectives, dialogue, cameras and environment patterns. Existing world data and relationships remain inspectable/editable in Flecs.

Advanced users and AI can create new definitions/templates, schemas and rule combinations at runtime using supported capabilities. This can support puzzles, interactive stories, simulations and games with controls, state, objectives and progression; the product is not constrained to idle dioramas. Each genre still needs the appropriate shipped mechanisms and presentation. Script cannot conjure an unsupported solver or algorithm.

Patch shipped definitions as versioned packages. Saves reference definition IDs/versions and retain user overrides/instance state; migrations handle schema changes. Native algorithms update through application/native-library patches. Avoid flattening every shipped definition into an opaque save or overwriting user-authored changes when updating a pack.

Multiplayer would share compatible definitions/scripts/assets, but that is only the content layer. Running sessions also need authority, replicated state/events, stable IDs, ownership/permissions, ordering/time synchronization and version compatibility. Flecs is the state/logic foundation, not a complete networking protocol. No networking implementation is authorized by this discussion.

## Proposed visual/navigation pass

1. Leaving a world first aligns its camera to the current preview viewpoint.
2. Pull up toward a stable overhead view, then zoom out only far enough to frame the collection.
3. Reveal neighboring previews by distance from the returning world, expanding outward. This is ordering, not a visible radar graphic. Keep unrevealed/faded previews non-interactive.
4. Give the returned world a brief quiet recall rim, distinct from the strong two-world swap indication. It must not bypass occlusion picking.
5. Trial rings as soft volumetric dust belts: broad low-density particles, varied sizes/drift and a few traveling highlights, colored by the theme. Preserve the layer structure without a hard torus edge. Start with existing particle/rendering capability; use VFX Graph only if its measured benefit warrants it.
6. Screen-space controls can be screen-anchored rendered glass spheres with symbols, using a small 3D presentation layer. Settings' content stays 2D.
7. Crossing the glass can use a brief low-amplitude refraction/wobble, strongest around the crossing and quickly settling. Keep it optional in the presentation config and avoid moving the actual camera solely to fake distortion.

Native intent owns return phases, target/recall identity and reveal progression. Unity interpolates the camera, draws the wave/glass and realizes Flexalon targets. New worlds insert at native order zero, shifting the existing collection; Flexalon presents the rearrangement. This pass is separate from the folder migration and core architecture proposal.

## Sources checked

- Official Flecs Script documentation: https://www.flecs.dev/flecs/FlecsScript.html
- Actual pinned bridge/native code and the existing FLECS-ARCHITECTURE-AUDIT, RUNTIME-CAPABILITIES and RUNTIME-STACK documents.
- Codex supports independent chats/threads and parallel workflows: https://developers.openai.com/blog/run-long-horizon-tasks-with-codex

No new chats, subagents, networking, package purchases or uploads were started for this proposal.
