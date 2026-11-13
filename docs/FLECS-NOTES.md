# Flecs notes: what helps us build the runtime engine

Reference sweep of official Flecs resources, judged against what Ecaty does today. Companion to [FLECS-ARCHITECTURE-AUDIT](FLECS-ARCHITECTURE-AUDIT.md), which holds the measurements and the native/Script split. This file holds the API reference and the recommendations. Date: 2026-11-12.

**Version.** We vendor Flecs **4.2.0**, master commit `b718658` from 2026-11-11 (submodule `Native/Core/vendor/flecs`, `vendor/flecs/distr/flecs.h:30-32`). No release newer than 4.2.0 was found. Notes marked **[4.2]** are new or changed in 4.2.0. Claims marked **[tested]** were checked in a scratch world against our own DLL (`4.2.0+6879066`, the previous pin; `b718658` only adds the C++ change below; Python ctypes, outside the repo).

**Sources.** Unless a topic gives another link, the source is the Flecs manual at `https://www.flecs.dev/flecs/<Page>.html`. The old `md_docs_2*.html` links redirect there. Release notes: <https://github.com/SanderMertens/flecs/releases>. Medium returns 403 to fetchers, so Sander's blog posts were not read directly. Discord was not used.

**Code references.** `file:line` references point at the submodule commits recorded on `main`: Native/Core `0515cdc`, Native/App `087a01a`, flecs-unity `e52267e`.

## The engine picture

The goal is a general runtime engine, not one feature. Sander's own games show the shape (Mossvale in section 2, the flecs-hub demos in section 16), and it matches our split.

- **Native code provides a small set of generic runtimes.** Each one is driven entirely by reflected components:
  - presentation, which Unity provides for us;
  - physics and collision, motion, animation curves, particles, audio and synth, layout and UI, terrain and meshing, input, spatial queries, timers.
  - For us, each of these is a capability with a reflected config component and a native system, hook or observer (https://github.com/Ecaty/capabilities-unity/blob/main/Documentation~/RUNTIME-CAPABILITIES.md).
- **Script holds every piece of content.**
  - *Prefab libraries* define families of things at every level: parts, materials, objects, creatures, buildings, vehicles, effects, sounds, UI widgets.
  - *Templates* make procedural shapes and scatter.
  - *Relationships* wire things together.
  - Event-rate *rules* (async, intents) drive behaviour.
  - Users, the AI and our shipped content all write the same Script, through the same edit and undo path.
- **The world can be inspected and is self-describing.** Doc briefs, alerts, member ranges and units on every type let the AI and Explorer understand the world without hand-kept prompt text (section 13).

Every example below is an instance of this split. "Vehicle → Plane" is just one family; the same mechanics apply to any family of composed objects.

---

## 1. Prefabs and inheritance

**What Flecs offers** ([PrefabsManual](https://www.flecs.dev/flecs/PrefabsManual.html), [ComponentTraits](https://www.flecs.dev/flecs/ComponentTraits.html))

- **Prefabs and instances.**
  - C++: a prefab is `world.prefab("Vehicle")`, and an instance is `world.entity().is_a(Vehicle)`, which adds `(IsA, Vehicle)`.
  - Script: `prefab Vehicle { ... }` declares a prefab, and `plane1 : Plane { ... }` creates an instance.
  - Queries skip prefabs unless the query uses `EcsQueryMatchPrefab` or includes an optional `Prefab` term.
- **Variants and chains.** A prefab can inherit from another prefab: `prefab Plane : Vehicle { ... }`. IsA chains of any depth work, and a variant overrides only what differs.
- **Per-component instantiation trait `(OnInstantiate, X)`**:
  - `Copy` is the default. The instance gets its own copy of the value.
  - `Inherit` shares the value from the prefab. The instance owns nothing until it overrides.
  - `DontInherit` means the component is not passed to instances.
  - **[4.2]** `Override` was renamed `Copy` (`flecs.h:773`, `:6756`).
- **Override.**
  - Setting a component on an instance overrides it, for example `inst.set(Speed{250})` or `plane1 : Plane { Speed: {250} }`.
  - Removing the override shows the prefab's value again.
  - `owns<T>()` is true only for the entity's own copy. `target_for<T>(IsA)` tells you which base supplies the value.
- **Auto-override.**
  - Every instance gets its own copy of a component even though the prefab normally shares it.
  - C++: `prefab.set_auto_override(T{...})`. Script: `auto_override | Wheels: {4}` (`vendor/flecs/distr/flecs.c:70848`).
  - Auto-override also works on prefab children.
- **Prefab children.**
  - Instantiating copies the prefab's subtree.
  - **[4.2]** Children inside `prefab`/`template` scopes default to the non-fragmenting `Parent` storage. Use `@tree ChildOf` to get named `ChildOf` children.
  - 4.1.6 and later raise an error if you change the children of a prefab that already has instances.
- **Slots.** **[4.2]** `SlotOf` was removed; it is not in our `flecs.h`. The manual now maps a prefab child to the instance's copy through the `Parent` storage, with `inst.target(Cockpit)`.
- **Speed.** 4.1 made instantiation about 3× faster. Prefab trees use a cached `TreeSpawner` (visible in JSON as `flecs.core.TreeSpawner`).

**How overrides serialize** [tested]. The script was `prefab Vehicle { Speed: {10}; auto_override | Wheels: {4} }`, then `prefab Plane : Vehicle { Speed: {200}; Wings: {30}; engine { Speed: {1} } }`, then `plane1 : Plane { Speed: {250} }` and `plane2 : Plane {}`.

| Setup | `plane2` entity JSON (no override) | `plane1.engine` |
|---|---|---|
| Default trait (`Copy`) | `IsA: Plane` and **every** value copied: `Speed 200, Wheels 4, Wings 30` | **not found by path**: the child is anonymous (`#1158`), because the default `Parent` tree does not copy names |
| `Speed`/`Wings` set to `(OnInstantiate, Inherit)`, plus `@tree ChildOf` | `IsA: Plane` and only `Wheels 4` (auto-override) | named `engine`, found by path |

- With `Inherit`, `plane1` serializes only `Speed 250`, its real difference.
- Query JSON marks a shared field with `"sources":["Plane"]`.
- So **"saves keep only differences" works only for components whose trait is `Inherit`**. The default `Copy` stores everything on every instance.
- Entity JSON omits inherited values unless `serialize_inherited` is set.

**What we do today**

- Prefabs are barely used. The only one is a C# string, `prefab GallerySharedShape {...}`, at `Assets/_Features/Gallery/Runtime/GalleryExperienceStore.cs:31`.
- No `.flecs` file uses `prefab`, `:` inheritance or `auto_override`.
- Templates are our only instancing mechanism, for example `WorkshopChain.flecs:9`, `ActorBehavior.flecs:3` and `RuntimeLibrary.flecs:3`.
- `fu_read_views` already handles shared (inherited) fields once per table (`Native/Core/src/flecs_unity.cpp:310-313`).

**Recommendation: adopt** as the engine's general kind system for every family of composed objects, not only creation kinds (BACKLOG 6).

- **Every kind is a prefab in a chain.** `Vehicle` → `Plane`/`Boat`/`Motorbike` is one example; Sander confirmed this shape to the user. Others follow the same pattern:
  - `Creature` → `Bird`/`Fish`
  - `Light` → `Lamp`/`Candle`
  - `Weather` → `Rain`/`Snow`
  - `Widget` → `Button`
- **A user's or the AI's creation is an instance that holds only its edits**, for example `my_plane : Plane { ... }`. Shared parts and materials are prefabs too (section 16).
- **Mark kind-level components `(OnInstantiate, Inherit)`.** This covers looks, tuning and capability defaults. Changing a kind then updates every instance, and saves hold only overrides plus the `IsA` pair.
- **Use `auto_override` only for per-instance state.** That means pose, motion state and anything a system writes every frame.
- **Use `@tree ChildOf` for kind prefabs whose parts we address by path.** Our bridge, C# and AI tools all look entities up by dotted path (`flecs_unity.cpp:358`). Otherwise keep `Parent` for small fixed sub-trees that never need names (section 5).
- **Keep templates for procedural geometry** (section 2). A kind prefab can contain template instances.
- **Before adopting, test that a reloaded managed script keeps instance overrides** (see Top changes, item 2).

## 2. Templates vs prefabs in Script

**What Flecs offers** ([FlecsScript](https://www.flecs.dev/flecs/FlecsScript.html))

- **Prefab.** A static entity tree that instances share or copy through `IsA`. It is cheap to instantiate, it is understood by queries and by JSON, and changes to it propagate through inheritance.
- **Template.** A *reactive parameterised script*: `template Tree { prop seed: f32 = 0 ... }`. Its body runs once per instance and re-runs when a `prop` it depends on changes.
  - Instantiate with `Tree t1(seed: 3)` or `t1 { Tree: {seed: 3} }`.
  - Templates can contain `mut` state, `const`, `for`, `if`, `match` and calls to `fn`.
  - **[4.2]** Template inheritance (`template Derived : Base`), parent constraints and `async` bodies.
  - Native code can set props as a component: give a C/C++ struct the template's name and members, then `set()` it.
- **When to use which.**
  - Use a **prefab** when instances share a fixed shape and differ by overrides. It also keeps saves small (section 1).
  - Use a **template** when the output is *computed* from parameters: procedural geometry, scatter, layouts, count-dependent children.
  - They combine: template bodies can create prefab instances, and prefabs can contain template instances.

**Sander's own example: Mossvale** (<https://www.flecs.dev/games/mossvale/>). It is a small RPG on Sander's newer, non-public "box" engine. Its models, terrain, UI, music, sound and quests are all written in Flecs Script, which you can read at `.../mossvale/etc/scenes/mossvale/*.flecs`.

- **Models.** Procedural primitives (Lathe, Sweep, Sphere and others) live inside templates: `Tree`, `House`, `Fence`. Materials are prefabs used as bases, as in `trunk : MvBark {...}`.
- **Terrain.** An ASCII tile map plus `fn` helpers, scattered with nested `for` loops of template instances.
- **UI.** Reactive layout templates.
- **Audio.** A Script-defined synthesizer, with instrument prefabs and note-clip prefabs.
- **Logic.** `async`/`await` coroutines.
- **Organisation.** One `include` per concern (state, geo, materials, props, world, hud, music), with prefixed names.
- **Version caveat.** Mossvale declares and sends script events (`event Advance()`, `emit Advance()`). Those keywords are **not in our 4.2.0 parser, and not in any public Flecs branch** (checked 2026-11-12: master is one commit past our vendored `6879066`, and none of its 52 branches has the tokens). So the feature is unreleased and cannot be pulled from GitHub yet. Until it ships, the 4.2 equivalent is a native custom event (`ecs_emit` plus an observer) or an intent component that a script `await`s. Its rendering, animation, synth and layout runtimes are native code we cannot see.
- **The lesson for us.** Native code provides generic primitives and runtimes. Script composes all content from them. That is our intended split.
- **Other games on the same engine** (2026-11-12): [Helix](https://www.flecs.dev/games/helix/) is a 2 km city with traffic, people, drones and a monorail. The others are [Bunny Hopper](https://www.flecs.dev/games/bunny_hopper/), [Lettuce Pray](https://www.flecs.dev/games/lettuce_pray/), [Ship Happens](https://www.flecs.dev/games/ship_happens/) and [Ore Else](https://www.flecs.dev/games/ore_else/). Unlike Mossvale, their Script is compiled into the `.wasm`, so there are no readable source files; they were not studied. To learn from them, ask Sander for the Script sources.

**What we do today**

- Eight templates with `prop`/`mut`/`const`/`match`/`async`: `WorkshopChain.flecs:9,48,63,76`, `WorkshopSeedMotion.flecs:6`, `ActorBehavior.flecs:3` and `RuntimeLibrary.flecs:3`.
- Instances look like `WorkshopChainLinks.flecs:1-4`.
- C# builds instance strings in `Assets/_Features/Creation/Runtime/WorkshopExamples.cs:38`.

**Recommendation: adopt** the prefab/template rule above.

- **Kinds and shared looks become prefabs.** Procedural parts stay templates.
- **Drive template props by writing the component**, not by re-sending source. The audit measured 18 µs for a prop change against a full re-evaluation for a reload.
- **Copy Mossvale's file organisation** for Workshop and creation content.

## 3. Script reactivity and parse-once

**What Flecs offers** ([FlecsScript](https://www.flecs.dev/flecs/FlecsScript.html), releases)

- **Managed scripts** are entities with `EcsScript`, created with `ecs_script_init` and updated with `ecs_script_update`/`ecs_script_reload`. They are reactive.
  - **[4.2]** Reactivity is fine-grained: only the code that reads a changed component value, presence, `mut`, prop or `$var` re-runs.
  - Errors go to `EcsScript.error`.
- **Rules for reactive scripts.**
  - Native writers must call `modified<T>()` after `ensure`/`get_mut`, or scripts never see the change.
  - Each component must be owned by a single script scope.
  - Entities created in loops must be named (`"e_{i}"`), or they are deleted and recreated on every update.
- **One-shot scripts.** Use `ecs_script_run`. Parse-once is `ecs_script_parse` followed by repeated `ecs_script_eval` and finally `ecs_script_free` (`flecs.h:18202-18246`).
- **Language features.** `const`, `export const`/`export mut` (readable from native code; call `ecs_mut_var_modified` after a native write), `module`, `include`, `using`, `with`, `@tree`, `fn`, `for`/`while`, `match`, vectors and swizzles, `async`/`await`, and struct/enum definitions.
- **[4.2]** An experimental IR runtime (`.ir = true` on the script descriptor, `flecs.h:17524`), a script shell, and a REST endpoint to call script functions (`/call`).
- **Lenient loading.** The release notes mention loading that skips unknown components, members and functions. **No such flag was found in our 4.2.0 headers**, so check this before relying on it.

**What we do today**

- Almost all app data and rules are managed scripts: `fu_set_managed_script` at `flecs_unity.cpp:260-289`, which reloads at `:273-274` and inits at `:276-279`.
- Parse-once is used for short commands, with up to 64 cached scripts (`:231-258`). Script edits, undo and from_scene are at `:627-644`.
- Per-frame `Clock[Clock].time` expressions remain in `WorldGalleryTreeMotion.flecs:7-13` and `ReactiveBehavior.flecs:5-6`.

**Recommendation: mostly done.**

- **Keep moving Clock-driven rules with many instances into native systems.** The audit measured about 23× faster.
- **Audit native writers for missing `modified()`.** A missing call is a silent reactivity bug. Systems that write through iterators should use `set` or call `modified`.
- **Name loop-created entities in AI-generated Script.** Put the rule in the agent instructions.
- **Leave the IR runtime off** until it is measured behind a flag (audit item 7).

## 4. Relationships

**What Flecs offers** ([Relationships](https://www.flecs.dev/flecs/Relationships.html), [ComponentTraits](https://www.flecs.dev/flecs/ComponentTraits.html))

- **Pairs.** `(Rel, Target)`. In Script, write `(CapDependsOn, RoadBinding)`. In queries, use wildcards: `(Rel, *)` returns every match, `(*, X)` matches any relationship to X, and `_` returns at most one.
- **Traits.**
  - `Exclusive`: one target per entity. `ChildOf` is exclusive.
  - `Symmetric`: adding `(R, Y)` to X also adds `(R, X)` to Y.
  - `Transitive`, `Reflexive` (like `IsA`), and `Acyclic`.
  - `Traversable`: allows `up` traversal; needed for custom hierarchy-like relationships.
  - `OneOf`: the target must be a child of a given entity.
  - `With`: adding one component also adds another.
  - `PairIsTag`: the pair never holds data.
  - **[4.2]** The new `Relationship` and `Target` traits restrict how an entity may be used. Several constraint traits moved into the `FLECS_CONSTRAINT_TRAITS` addon, which our full build includes.
- **Union was removed in 4.1.** Its replacement is the `DontFragment` + `Exclusive` traits on the relationship, for example a state machine with an `(Movement, Walking)` pair.
- **Traversal in queries.**
  - `Position(up)` means `up ChildOf`. Also `up Rel`, `self|up`, `cascade` (breadth-first, parents first) and `cascade|desc`.
  - `cascade` requires a cached query.
- **Cleanup.**
  - `(OnDelete, Remove|Delete|Panic)` applies when the relationship or component entity is deleted.
  - `(OnDeleteTarget, ...)` applies when the target is deleted.
  - Deleting a `ChildOf` parent deletes its children.

**What we do today**

- **`CapDependsOn` is a plain tag** used as a relationship (`Packages/com.runtime.capabilities/Runtime/Capabilities/Resources/CapabilitySchema.flecs:5`). It is read by walking `ecs_get_type` (`Native/Core/src/RuntimeCapabilities.cpp:74-86`) and from JSON (`CapabilityGraph.cs:52`).
- **`GalleryOnLayer` is a plain tag** (`WorldGalleryTree.flecs:21`). Each world has exactly one layer (`CrystalWorldGallery.cs:578`).
- No traits are set anywhere, and no query uses `up`/`cascade`.

**Recommendation: adopt, small change.**

- **Add `Exclusive` to `GalleryOnLayer`.** Moving a world to another layer then replaces the pair instead of possibly adding a second one. Consider `DontFragment` too, so one table per layer is not created.
- **Give `CapDependsOn` `(OnDeleteTarget, Remove)`.** Then deleting a binding cleans its dependants' edges. Make it `Acyclic` once that is confirmed available.
- **Model creation-tool wires (BACKLOG 6) as relationships.** Use `(Triggers, target)`. Use `Symmetric` for two-way links.
- **Use `Exclusive` + `DontFragment` for state machines.** They replace Union.

## 5. Hierarchies: `ChildOf` vs `Parent`

**What Flecs offers** ([HierarchiesManual](https://www.flecs.dev/flecs/HierarchiesManual.html))

- **`(ChildOf, parent)` fragments storage.** Each parent gives its children their own table. Component access is O(1) and contiguous. Names and paths work, and `up`/`cascade` are fast.
- **`Parent{parent}` does not fragment.** It arrived in 4.1.5 and uses `ecs_new_w_parent` or `flecs::Parent{p}`.
  - Children of many parents share tables, and reparenting is cheap. A `(ParentDepth, n)` pair supports `cascade`/group_by.
  - Children are always ordered.
  - Costs:
    - Deleting a child is O(n).
    - `up` and `(ChildOf, p)` queries are slower.
    - `Or`/`Not` are unsupported.
    - Instance children do not get the prefab children's names [tested above].
- **Guidance.** Use `ChildOf` for large dynamic scenes and `Parent` for small structured trees such as prefabs with tens of children. **[4.2]** `Parent` is the default inside `prefab`/`template` scopes.
- **`OrderedChildren` trait** (4.1). It keeps insertion order for `children()` and lets you reorder with `ecs_set_child_order`.
- **Paths.** Use `.` in C/Script and `::` in C++. Names are unique per parent, and lookup is O(1).

**What we do today**

- Entities use plain `ChildOf` from Script nesting (`World.flecs:3-12`).
- Children inside templates get `Parent` by default; the audit observed `flecs.core.Parent` in JSON.
- The bridge reads `ecs_get_parent` per view (`flecs_unity.cpp:321`) and uses dotted paths everywhere (`:263-265`, `:358`).
- C# queries with `(ChildOf, ...)`: `ClayCreation.cs:523,535` and `LayoutCapability.cs:24`.

**Recommendation: check, then decide.**

- **Check that `(ChildOf, $parent)` queries also match `Parent` children.** The manual says they do, but more slowly. Do this before relying on it for template-made parts.
- **Use `@tree ChildOf` where parts are addressed by name**, such as creation parts and capability targets.
- **Use `OrderedChildren` where order matters**, such as UI and layout lists.

## 6. Queries, caching and change detection

**What Flecs offers** ([Queries](https://www.flecs.dev/flecs/Queries.html), [FlecsQueryLanguage](https://www.flecs.dev/flecs/FlecsQueryLanguage.html))

- **Cache kinds.**
  - `Default` (cached when owned by a system, otherwise uncached), `Auto` (cache what can be cached), `All` (must be fully cacheable) and `None`.
  - Cached queries iterate fast but are slower to create and add cost to every table create and delete.
  - Uncached queries suit ad-hoc lookups.
  - The FAQ says queries *"are expensive to create, so make sure to create them in advance"* ([FAQ](https://www.flecs.dev/flecs/FAQ.html)).
- **Language.**
  - `!`, `?`, `||`, `$var` (joins), `up`/`cascade`/`self`, scopes `!{ ... }`, `==`/`~=` name predicates, and the access modifiers `[in]`/`[out]`/`[none]`/`[filter]`.
  - `$this.child` is a lookup variable.
- **Change detection.**
  - Cached queries only. Since 4.1 you opt in with `EcsQueryDetectChanges`.
  - Tracking is per table and per component, not per entity.
  - Detected: `set`, `modified`, structural changes, and iterating out/inout terms. Use `in` terms, and call `ecs_iter_skip` on tables you did not write.
  - Not detected: writes through `ensure` without `modified`.
- **`group_by` and `order_by`.**
  - `group_by` is the cheapest ordering. You can iterate one group with `ecs_iter_set_group`, which is good for per-world or per-layer partitions.
  - `order_by` is expensive; use it only on rarely changing data.
  - 4.1.6 notes say change detection on sorted queries is an error. Verify on 4.2.
- **[4.2]** Faster cache iteration. Disabled, prefab and non-queryable tables are sorted to the end of the cache.

**What we do today**

- **Cached `UnityViews`** (`flecs_unity.cpp:163-169`).
- **Bridge query slots** use the `Auto` cache with `DetectChanges` (`:413-452`, `:428-429`) and stable handles (`:481-519`).
- **Per-tick uncached queries** in capabilities (`RuntimeCapabilities.cpp:83,94`, called from `:120`). The behaviour-tree condition node builds a query for each evaluation (`:49`).

**Recommendation: mostly done.**

- **Cache the per-tick capability queries once binding counts grow.** The audit left them uncached on purpose for now.
- **Use `group_by` by world or scope** once several gallery or open worlds live in one Flecs world. Each world then iterates only its own group.

## 7. Observers, hooks and events

**What Flecs offers** ([ObserversManual](https://www.flecs.dev/flecs/ObserversManual.html))

- **Events.** `OnAdd`, `OnRemove` and `OnSet`. `Monitor` fires when an entity starts or stops matching a multi-term query, which costs about twice as much. Custom events go through `ecs_emit`/`ecs_enqueue` with a payload in `it->param`. `yield_existing` replays matches that already exist.
- **Hooks.**
  - Hooks are per-component lifecycle callbacks: `on_add`, `on_set`, `on_remove`, and `on_replace` (4.1.1). Each component has one of each.
  - Hooks run before observers and may mutate the component. The manual: *"Hooks are much more efficient than observers."*
- **Guidance from the manual.**
  - Use observers for structural changes and infrequent events.
  - Do not add or remove tags only to trigger observers; that is what systems are for.
  - A plain event queue beats observers for simple cases. Wildcard observers are expensive.
- **Propagation.** Events travel down `IsA`/`ChildOf` when an observer has an `up` term. **[4.2]** Propagation happens only if such an observer exists, and tables are locked during `OnAdd`/`OnRemove`.
- **Since 4.1.1, `OnSet` is skipped when nothing listens.**

**What we do today**

- Two native `OnSet` observers: `GalleryRingIntentObserver` and `GalleryTreeActivityObserver` (`Native/App/src/Application.cpp:88-102`).
- Label hooks own copies of the label string (`Native/Core/include/FlecsComponents.hpp:19`).
- Capability coordination still polls every tick (`RuntimeCapabilities.cpp:83-111`).
- Script uses `await on.activate(...)` (`WorkshopChain.flecs:33-45`).

**Recommendation: adopt.**

- **Replace capability polling with an `OnSet(CapBinding)` observer** that pushes the entity onto a native dirty list, then process the list in a system. This is the audit's phase 3.
- **Bridge views need no observers**; the change-detected cached query is enough.
- **Use custom events for creation-tool wires** ("cause → reaction"): emit on cause, observe on reaction. This beats the tag-toggling the manual warns against.

## 8. Pipelines, phases, threads, timers

**What Flecs offers** ([Systems](https://www.flecs.dev/flecs/Systems.html))

- **Phases.** `OnStart` (first frame only), then `OnLoad`, `PostLoad`, `PreUpdate`, `OnUpdate`, `OnValidate`, `PostUpdate`, `PreStore`, `OnStore`.
  - A custom phase is an `EcsPhase` entity plus `(DependsOn, phase)`.
  - A custom pipeline uses `ecs_pipeline_init`.
  - Systems in one phase run in declaration order, which recycled IDs can disturb.
- **Sync points.** Structural changes inside `progress()` are deferred and merged at sync points. Declare writes with `.write<T>()` and reads with `.read<T>()`.
  - `immediate` systems run single-threaded with a writable world.
  - **[4.2]** Implicit defer mode was removed and `ecs_is_deferred` was renamed `ecs_is_stage`.
- **Threads.**
  - `ecs_set_threads(n)` plus `multi_threaded` systems split each table across workers. `ecs_set_task_threads` hands the work to an external job system.
  - Since 4.1, systems running on worker threads cannot create entities.
- **Timers** (`FLECS_TIMER`).
  - `ecs_set_interval`, `ecs_set_timeout`, `ecs_set_rate`, and `ecs_set_tick_source` for one shared timer.
  - Timers can be stopped and started. `it->delta_system_time` gives the time since the system last ran.

**What we do today**

- Three systems: `SpinSystem` (OnUpdate) and the two gallery systems (PreUpdate) at `Application.cpp:15,72,77`.
- `progress` is called from `fu_tick` (`flecs_unity.cpp:224`).
- REST requests, futures and `Clock` are applied before `progress`; capabilities tick after it (`:205-227`).
- No custom phases, threads or timers. `engine.wait` is a hand-rolled future list (`:152-157`, `:208-217`).

**Recommendation.**

- **Adopt timers** for low-rate work: capability coordination at 10 Hz and autosave. They replace hand-rolled throttles in C#.
- **Move `fu_caps_tick` into the pipeline** as a system in a named phase, so Explorer's pipeline stats can see it.
- **Leave threads off** until a profile shows a native system that is CPU-bound. We have no hot native loop yet.
- **Define our phase map once, in a doc comment:** PreUpdate for intents, OnUpdate for simulation, PostUpdate for derived state, PreStore for snapshots to Unity.

## 9. Serialization for saves

**What Flecs offers** ([FlecsRemoteApi](https://www.flecs.dev/flecs/FlecsRemoteApi.html), `json.h`, example `examples/c/reflection/world_ser_deser`)

- **`ecs_world_to_json` / `ecs_world_from_json`.** The only options are `serialize_builtin` and `serialize_modules` (`flecs.h:16134`). There is no scope filter. Entities are matched by name, so anonymous ones are fragile. Components without reflection are silently dropped unless `strict`.
- **`ecs_entity_to_json` options:**
  - `serialize_entity_id`, `_doc`, `_full_paths`, `_inherited`, `_values`, `_builtin`, `_type_info`, `_alerts`, `_refs`, `_matches`.
  - A `component_filter` callback (4.1.2).
- **`ecs_iter_to_json` options:** the same, plus `serialize_table`, `_fields`, `_parents_before_children` and query info, profile and plan.
  - For a single subtree, serialize a query such as `?ChildOf(self|up, Root)` instead of the whole world.
- **Prefab instances.** With `Inherit` components, entity and query JSON contain only overrides plus `IsA` [tested, section 1].
- **[4.2]** JSON fixes, map and value-type reflection, and support for more than 31 struct members.

**What we do today**

- Saves are hand-built JSON in C#, not a Flecs snapshot.
- `RuntimeExperience.cs:495-503` stores managed `sources` plus `{entity, components}` states from a `Persistent || PersistState` query (`:456`).
- The gallery saves `GallerySavedWorld` records (`GalleryExperienceStore.cs:48,177-200`). Clay has its own format (`ClayCreation.cs:313`).
- `ecs_world_to_json` is not used.

**Recommendation.**

- **Keep the "sources + persistent state" design.** It saves intent (Script) rather than a full world dump, so saves survive schema changes and stay AI-readable.
- **Use prefab inheritance for creation kinds** so that a creation's state is only its overrides (section 1). That is the actual "differences only" mechanism; `world_to_json` is not.
- **Do not adopt `world_to_json` for player saves.** It has no scope filter, is name-fragile and is tied to modules. It is fine for debug dumps.

## 10. Reflection and meta for the C ABI

**What Flecs offers** (`meta.h`, `examples/c/reflection/*`)

- **Type registration.** `ecs_struct`, `ecs_enum` and `ecs_bitmask`. C++ uses `.member()`. Script uses `struct X(a: f32)`.
- **`ecs_member_t` fields.** `offset`, `type`, `count` and `unit`, plus `range`, `warning_range` and `error_range` as min/max pairs for sliders.
- **Other kinds.** Opaque types (`ecs_opaque_init`) for custom containers, vector and map types, and `ecs_meta_cursor` for typed get/set by member name.
- **A binding can read raw memory.** It reads the member offsets once per type, then reads component memory directly through `ecs_field`/`ecs_get_id`. Strings, entities, vectors and maps need special handling. This is our inference; there is no official binding guide.

**What we do today**

- C++ components are reflected with `.member` (`Native/Core/src/Components.cpp:6-40`, `Application.cpp:54-70`).
- Script defines types in `Schemas/*.flecs`.
- The POD check walks `EcsStruct`/`EcsPrimitive` (`flecs_unity.cpp:564-577`).
- The binary paths are `fu_read_component`, `fu_write_components` and `fu_query_read_data`, backed by a native type-size cache.

**Recommendation: mostly done.**

- **Next: export a type layout** (`fu_type_layout(type) → [{name, offset, kind, count}]`). C# could then check its `StructLayout` mirrors at startup instead of drifting silently, and AI tools could describe component fields without JSON.
- **Add member `range`s on tunable fields.** They give free validation and Explorer sliders, and tell the AI the allowed values.

## 11. Modules

**What Flecs offers** ([Quickstart](https://www.flecs.dev/flecs/Quickstart.html), [DesignWithFlecs](https://www.flecs.dev/flecs/DesignWithFlecs.html))

- A module is `ECS_MODULE` or `world.import<T>()`. Its contents become children of the module entity. Importing the same module twice is free.
- In Script, `module a.b` scopes a file.
- Sander's design guide: *"The purpose of modules is really to enable reusability."* Scope modules to a feature, and split components from systems so implementations can be swapped. Import order sets system order within a phase.

**What we do today**

- No native modules. `register_core` (`Components.cpp:6`) and `register_application` (`Application.cpp:107`) are plain functions called from `fu_create_ex` (`flecs_unity.cpp:159-160`).
- One script uses `module` (`WorkshopWorldLooks.flecs:1`).
- Managed script names act as ad-hoc modules (`"GalleryGraph_" + id`).

**Recommendation: adopt, low effort.**

- **Wrap `register_core`, `register_application` and the capability code as `flecs::module`s.** Name them by feature (`core`, `gallery`, `capabilities`). The benefits are clean paths in the Explorer, `world_to_json` module filtering, and import order as explicit system order.
- **Give each content file a `module` line** so AI-authored entities do not collide in the root scope.

## 12. REST and Explorer

**What Flecs offers** ([FlecsRemoteApi](https://www.flecs.dev/flecs/FlecsRemoteApi.html), <https://github.com/flecs-hub/explorer>)

- **Endpoints.**
  - `/entity`, `/component`, `/query?expr=` (with serializer flags and `try`), `/world`, `/tables`, `/components`, `/queries`.
  - `/stats/world` and `/stats/pipeline` (these need `FlecsStats`).
  - `PUT /script/<path>`, `/call/<fn>` [4.2], `/commands/capture`, `/shell/session`.
- **Read-only mode** [4.2]. `readonly` returns 403 for everything except GET.
- **Security.** `EcsRest.ipaddr` defaults to `0.0.0.0`. The manual says the server *"should not be used in production environments."*
- **Explorer.** <https://www.flecs.dev/explorer/?host=localhost:27750>. **[4.2]** It discovers local apps automatically.

**What we do today.** The audit's REST items are done:

- REST is bound to 127.0.0.1 (`flecs_unity.cpp:176`).
- A busy port is non-fatal (`:106-135`), and read-only mode is available (`:178-181`).
- REST runs only in the Editor and development builds (`FlecsWorld.cs:15-19`).

**Recommendation: done.**

- **Optional extra hardening:** a release native build without `FLECS_REST`/`FLECS_HTTP`. Today every addon is compiled in.
- **Use `/call/<fn>` for AI and dev tools** to invoke Script functions without composing Script text.

## 13. Addons for an AI-editable engine

- **Doc** (`ecs_doc_set_brief/_detail/_link`).
  - **Today:** `fu_document` sets briefs (`flecs_unity.cpp:735-738`).
  - **Adopt:** put a brief on every component, capability, prefab kind and template prop. The agent then reads tool and field descriptions from the world with `/query?doc=true` instead of from hand-kept prompt text.
- **Alerts.** Query-defined warnings attached to entities, serialized with `serialize_alerts`.
  - **Today:** `fu_define_alert` exists (`:729-734`).
  - **Adopt for AI validation:** for example "creation without a kind", "binding without a target", "wire to a missing entity". Feed active alerts back to the agent after each Script edit.
- **Metrics.** Gauges and counters derived from component members.
  - **Today:** `fu_define_metric` exists (`:723-728`).
  - **Use for Explorer dashboards** of creation counts and capability faults.
- **Units.** Imported (`flecs_unity.cpp:144`) but unused.
  - **Annotate positions, angles and seconds.** This costs nothing and helps the AI and Explorer display values correctly.
- **Stats.** Imported. Feeds `/stats/*`.
- **Script math.** Enabled (`CMakeLists.txt:12`).

## 14. Performance: fragmentation, sparse, tags, table moves

**What Flecs offers** ([ComponentTraits](https://www.flecs.dev/flecs/ComponentTraits.html), [ecs-faq](https://github.com/SanderMertens/ecs-faq), releases)

- **Every add or remove of a component, tag or pair moves the entity to another table.** Every unique `(Rel, target)` pair creates a new table. Many small tables slow down cached queries and table creation.
- **`Sparse`.** Stored outside tables, with stable pointers. Adding and removing it is faster, at some cost to query speed. Non-movable C++ types are made sparse automatically.
- **`DontFragment`.** Sparse storage that creates no tables.
  - It does not appear in the entity type, monitors don't fire for it, and it supports no `Or`/optional/inheritance.
  - Use it for very sparse or rapidly toggled data and relationships. **[4.2]** Its operations are faster, and it is the only trait allowed with `With` targets.
- **`CanToggle`.** `enable<T>()`/`disable<T>()` turn a component on or off without a table move, at a small cost to every matching query.
- **Tags vs components.** A tag has no data and costs no storage, but adding or removing it is still a table move. Prefer a data field or `CanToggle` for state that flips every frame.
- **Prefabs.** Sander's design guide notes that prefabs are faster as well as convenient: shared components are stored once.
- **Quick wins in 4.1.** `set` became about 2× faster and `modified` about 5×.

**What we do today**

- We use no traits.
- Intent tags such as `GalleryReturnIntent` are set by Script at event rate.
- Capability state is component data.
- `fu_write_components` calls `ecs_set_id` per entity (`flecs_unity.cpp:597-604`).

**Recommendation.**

- **Use `CanToggle` for UI or selection state that flips often**, such as hover, selected or highlighted.
- **Make `GalleryOnLayer` and future wire relationships `DontFragment`** (section 4).
- **Measure before changing anything else.** We are nowhere near table-count limits.

## 15. Other things that help a runtime-scriptable, AI-editable engine

- **Script edits and undo** (`ecs_script_edits_*`, `ecs_script_undo`). We use them (`flecs_unity.cpp:627-644`). AI edits should go through edits and captures, not whole-source rewrites, so the user can undo them.
- **`@tree` and `with` blocks.** They shorten AI-generated Script and make it less error-prone. Add them to the agent's Script cheat-sheet.
- **`fn` in Script.** Shared math lives in one place, as in Mossvale's `geo.flecs`. Prefer it to copying expressions.
- **Name the "ownership" rule for AI Script.** One script owns each component; named entities live in loops; prefabs are for kinds and templates for procedural parts. Put this in the AI instructions (https://github.com/Ecaty/ai-agent-unity/blob/main/Documentation~/AI-INTEGRATION.md).
- **flecs-hub.**
  - The `city` and `tower_defense` demos split components and systems into hub modules (`flecs-components-*`, `flecs-systems-*`). Script holds the assets.
  - The audit confirms that in `tower_defense` every per-frame rule is a native C++ system and Script defines only prefabs and assets. That is the same split we chose.

## 16. Patterns from Sander's public demos (flecs-hub)

Repos: <https://github.com/orgs/flecs-hub/repositories>. Read through `gh api` on 2026-11-12. These demos ship older Flecs: `traffic` vendors 4.1.5 and `city` vendors 4.1.1. Nothing newer than 4.2.0 appears, and neither demo uses auto_override, slots, group_by, change detection, Sparse, DontFragment or CanToggle.

- **The vehicle model is [`city/etc/assets/city.flecs`](https://github.com/flecs-hub/city/blob/main/etc/assets/city.flecs)**, a `module flecs.city` prefab library.
  - `prefab Car` has part child prefabs (wheels, lights, body). The four wheels are anonymous instances of one `Wheel` prefab placed with `Position3`.
  - Variants are IsA chains: `SidewalkTree : Tree` and `ModernBuilding : Building`.
  - Parts inherit colour from shared material prefabs (`Base : BinMaterial {...}`).
  - Native code looks prefabs up by name and instantiates them with `ChildOf` + `IsA` + `set(Position3)` (`city/src/module.c:79-87`).
  - For us this is the pattern for every kind family, `Vehicle` included. A shared part library (wheel, seat, light, propeller) plus material prefabs, and kinds as IsA variants.
- **Configuration lives in Script; a native generator reacts to it.**
  - In `city`, `scene.flecs` holds one `city { City: {...} }` entity. An `on_set` hook on `City` generates the whole city natively (`module.c:709+`).
  - In `traffic`, `traffic.flecs` declares `export const AccelerationForce: 1.5` and similar constants. Native code reads them with `world.get_const_var(...)` (`traffic/src/cars.cpp:99-106`).
  - **Adopt:** this is how creation tools should drive native capabilities. The AI or user writes a small config component in Script, and a native hook or observer builds the result. It matches our `GalleryConfig.flecs` approach and the `CapBinding` adapters.
- **Script generates the scene; native code runs the simulation.**
  - `traffic/etc/assets/grid.flecs` builds a 50×50 road grid with nested `for`, interpolated names (`"road_lr_{x}_{y}"`), `with cars.Road(...) {...}`, and `#0.lookup(...)` to wire entity references.
  - All per-frame lane and car logic is native. It runs in `multi_threaded` systems (`cars.cpp:598-1201`) under `world.set_threads(8)`.
- **The `With` trait for companion components.** `Car` automatically adds `Transform` and `TransformManually`, and `Lane` adds its arrays (`cars.cpp:136,157`). **Adopt** this for capability components that always need a companion state component, so authors and the AI cannot forget it.
- **Static vs code-driven transforms.**
  - `TransformOnce` marks static entities, so per-frame transform systems skip them.
  - `TransformManually` marks entities whose matrix code writes directly (`cars.cpp:236-249`).
  - **Consider** the same split for our view sync: static scenery would skip `fu_read_views` work.
- **One-shot deferred work.** An `.immediate()` system disables itself after running, and observers re-enable it ("ping") when its inputs change (`cars.cpp:195-201`: `ConnectRoads`, `ConnectIntersections`). This gives a cheap batched post-edit step, for example rebuilding a capability graph once per frame after many Script edits.
- **Housekeeping.** `traffic/src/main.cpp:28-36` runs a `DeleteEmptyTables` system as `.immediate().rate(10)` in PostLoad. **Consider** it once creations come and go often (section 14).
- **A reactive template for a visual state.** `traffic_light.flecs` is `@tree Parent` + `template` with `prop state`, bound to the native `TrafficLight` component. It uses `match state {...}` for colour, so native code sets the component and the template re-renders. **Adopt** this for capability visuals: native code sets state, and Script decides how it looks.
- **Templates can be spawned like prefabs.** `flecs-game`'s grid spawner (`src/main.c:35-60`) accepts a Script template where it expects a prefab, by creating a private default instance. This is useful if our spawn tools accept both kinds.
- **[flecs-examples](https://github.com/flecs-hub/flecs-examples/tree/main/examples/scripts).** Small Script samples (`buildings`, `fence`, `traffic_light`, `perlin`, `for`, `math`). They are good few-shot material for the agent's Script instructions. They show relative assignment (`Rotation3: {x += ...}`), typed props with defaults, and composable room templates.
- **Not relevant to us.**
  - `flecs-cs` (C# bindings) was last updated in 2023; our own C ABI bridge stays.
  - `flecs-polyglot` provides Rust and TypeScript WASM bindings and is under construction.
  - The older `flecs-systems-*` renderers are superseded by Unity in our stack.

---

## Top changes for us (impact vs effort)

| # | Change | Impact | Effort | Section |
|---|---|---|---|---|
| 1 | **A general kind system: every family of composed objects is a prefab chain** (for example `Vehicle` → `Plane`/`Boat`, `Creature` → `Bird`, `Light` → `Lamp`), modelled on `city.flecs`: a shared part library plus material prefabs. Kind components `(OnInstantiate, Inherit)`, per-instance state `auto_override`, `@tree ChildOf` for named parts. Saves store only overrides plus `IsA`. | High: BACKLOG 6, smaller saves, editing a kind updates all its instances | Medium | 1, 2, 16 |
| 2 | **Prototype first:** reload a managed script that defines a kind and check that instance overrides and child paths survive. Then decide the save format (sources plus override states). | High: removes the main risk of item 1 | Low | 1, 9 |
| 3 | **Doc briefs and alerts as the AI's contract.** Briefs on components, kinds and props; alerts for invalid creations, fed back after each AI edit. | High for AI quality | Low–Medium | 13 |
| 4 | **Capability coordination becomes an `OnSet` observer, a dirty list and a timer-driven system** in a named phase, with cached queries (audit phase 3). | Medium: removes per-tick polling, shows in pipeline stats | Medium | 6, 7, 8 |
| 5 | **Relationship traits:** `GalleryOnLayer` Exclusive (+DontFragment); `CapDependsOn` with `OnDeleteTarget, Remove`. Creation wires as relationships plus custom events. | Medium: correctness and cleanup for free | Low | 4, 7 |
| 6 | **Native code as `flecs::module`s; content files with a `module` line.** | Medium: clarity in the Explorer and for AI, explicit system order | Low | 11 |
| 7 | **`fu_type_layout` export plus member ranges and units** for C# mirror checks and AI field descriptions. | Medium | Low–Medium | 10, 13 |
| 8 | **Copy Mossvale's content organisation** (include per concern, `fn` helpers, template scatter) for Workshop and creation content. Use Sander's demo patterns: config in Script built by a native hook, `With` companions, template visuals driven by native state. | Medium | Low | 2, 16 |
| 9 | **Audit native writers for `modified()`.** Name loop-created entities in AI Script. | Medium: prevents silent reactivity bugs | Low | 3 |
| 10 | Optional: release native build without REST/HTTP; `CanToggle` for frequent UI state; `group_by` per world; IR runtime A/B. | Low–Medium | Low–Medium | 8, 12, 14 |

**Status:** Done 2026-11-13 (T11): items 4 (partly), 5, 6, 7 and 9. Native systems and observers live in modules `engine.gallery`, `engine.spin` and `engine.capabilities` (components stay at the root: Script, C# and JSON use bare names); capability coordination runs in the pipeline as `CapabilityCoordinationSystem` (immediate, own `CapabilityPhase` after OnStore) instead of after `progress`; `GalleryOnLayer` is `Exclusive`, `CapDependsOn` has `(OnDeleteTarget, Remove)`; `fu_type_layout` exports member offsets/types/units/ranges and `FlecsSession` checks each managed mirror against it once; the `modified()` audit found no missing call (the only iterator write, SpinSystem, has it) and the agent instructions ask for named loop entities. Not done, on purpose: the OnSet observer + dirty list + 10 Hz timer for capabilities (Cap* components are Script-defined and may be redefined, which deletes observers and orphans cached queries; per-tick polling costs a few µs today), `DontFragment` on GalleryOnLayer (entity JSON and the gallery checks read the pair), member ranges on tunable fields, and `module` lines in content files (would change the entity paths C# looks up).

**Things to verify on 4.2.0 before relying on them:**

- `(ChildOf, $p)` queries matching `Parent` children.
- Change detection on sorted queries.
- Lenient script loading; there is no flag for it in our headers.
- Whether reloading a managed script keeps instance overrides (item 2).

**Version watch:**

- Mossvale's `event`/`emit` Script keywords are unreleased (not on any public Flecs branch as of 2026-11-12). Watch the release notes.
- `b718658` (pinned 2026-11-13) adds one C++ API change over `6879066`: `set`/`assign` convert the value to the component type. Nothing else in these notes changes.
- Revisit this file on the next Flecs update.
