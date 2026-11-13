# Flecs architecture audit (BACKLOG 3)

Date: 2026-11-11. Flecs 4.2.0 vendored. Supersedes docs/history/FLECS-4.2-AUDIT.md. Topic-by-topic API reference and ranked recommendations: [FLECS-NOTES](FLECS-NOTES.md). Tags: [V] external source, [C] project code, [M] measured, [I] inference.

## Verdict

Yes, a native/runtime split will improve performance, but the biggest measured cost today is not Script — it is the **JSON bridge between Unity and Flecs**. Fix that first, then move per-frame `Clock`-driven rules into compiled systems. Keep Script for content, templates and event-rate rules (that is also how Sander's own demos split it).

## Measured in the running Unity Editor (gallery, Play mode) [M]

| What | Cost |
|---|---|
| `EntityJson` of one gallery entity (3.6 KB) | 122 µs native + ~1.05 ms `JObject.Parse` = **~1.2 ms** |
| `Query("GalleryEntry")` JSON (3.8 KB) | 84 µs native, **~1.35 ms** with parse |
| Typed `ReadViews` (binary) | **4.7 µs** |
| P/Invoke baseline (`Lookup`) | 0.47 µs |
| Gallery JSON reads per frame | ~5.1 |
| `Game.Gallery.NativeRead` marker | **2.27 ms/frame** of an 11.9 ms PlayerLoop (~19%) |
| `Game.Gallery.Appearance` marker | 1.08 ms/frame |
| GC allocated per frame | **~290 KB** |
| Bridge query cache | 979 builds vs 976,952 hits (rebuilds come from whole-cache flushes) |

Editor/Mono numbers; IL2CPP players are faster in absolute terms but the JSON:binary ratio (~250×) and GC churn remain. Isolated ctypes measurements (Verification/bench_flecs.py) agree: Script Clock expressions ~1.5 µs each vs compiled system ~64 ns/entity (~23×); binary bulk query read 65–220× faster than JSON.

## Decided split

**Native C++ (Native/Core, Native/App):** stable reflected component contracts (gallery state, motion, intents, capability state); every per-frame continuous rule (ring turn integration, easing/progress, motion, capability coordination) as systems in explicit phases with queries cached at import; hooks/observers instead of per-tick polling; timers for low-rate work.

**Runtime (Flecs Script, prefabs, user/AI content):** templates and prefabs, parameters, event/intent-driven rules (async, `mut`), user and AI experiences, new reflected types, metrics/alerts/doc. Use `for`/templates instead of copy-paste; change template props by writing components, not reloading source. Rule of thumb: Clock-driven + many instances → native system parameterised by Script-authored components.

**Bridge:** typed blittable reads/writes and cached-query snapshots for anything per frame; stable query handles (no whole-cache flush on managed-script set); change-detected batches; names only when changed; native type-size cache. JSON stays for agent/tool/persistence/diagnostics only.

## Done 2026-11-11

- Gallery `Read()` fetches one component (`ComponentJson`) instead of the whole entity with type info; per-frame scalars (clock time, ring turns) use typed `TryRead<T>`. Measured (warm, Editor): `Game.Gallery.NativeRead` **2.27 → ~0.74 ms/frame**, GC **290 → ~158 KB/frame**, PlayerLoop ~11.9 → ~10.4 ms. All 118 gallery checks pass.
- Gallery tuning values ship as Flecs Script (`Assets/_Features/Gallery/GalleryConfig.flecs`, generated from Gallery.asset). Players no longer include Gallery.asset; the Editor polls the asset only under `UNITY_EDITOR`.

- Ring turns and the tree motion clock are compiled systems in `Native/App` (`GalleryRingMotionSystem`, `GalleryTreeMotionSystem`, PreUpdate) plus OnSet observers so same-frame reads still see a written intent immediately. The four contracts (`GalleryTreeActivity`, `GalleryTreeMotion`, `GalleryLayerIntent`, `GalleryLayerState`) are C++ components; return/glass/recenter rules stay reactive Script. Same outputs as the Script version (ctypes check); world tick 55 → 34 µs at 6 rings; Editor frame unchanged (the gain matters at scale, not at 6 rings).
- Bridge ABI 3 (plan items 2, 4, 5, 8; branch `perf/bridge-gc-theme`):
  - **Stable query handles** (`fu_query_create/destroy/json_h/changed/read_data`). Managed-script set/undo/delete no longer flush every query; a slot is rebuilt from its expression only when a component it references is no longer alive (type redefined). String-keyed calls map onto bounded (32) named slots. C# `FlecsSession.ReadQuery`/`QueryHandle` use handles.
  - **Native type-size cache** for `fu_read_component`/`fu_write_components` (cleared when sources change).
  - **`fu_read_views` via the cached `UnityViews` query** with optional Rotation/Scale/Color/Primitive/Label terms read per table (no per-entity `try_get`).
  - **Parse-once `fu_run_script`**: up to 64 parsed scripts (≤ 4 KB each) reused; dropped whenever managed sources change.
  - **REST**: busy port is non-fatal (probed before start; `fu_rest_status` = -1 and a warning, world still runs); optional read-only mode (`fu_create_ex(port, FU_REST_READ_ONLY)`, `FlecsWorld.restReadOnly`).
  - **No owned strings for per-frame text**: `fu_component_json_into`, `fu_label_into`, `fu_name_into` copy into caller buffers.
  - Isolated ctypes measurements, MSVC Release, same machine, before (main DLL) → after: `fu_read_views` N=1000 **226 → 31 µs**; managed-script set + `fu_read_query_data` N=1000 **1.10 ms → 21 µs**; repeated `fu_run_script` one-liner **12.2 → 1.2 µs**; `fu_read_component` 1.5 → 1.1 µs; warm query read and tick unchanged. Verification/native_smoke.py covers each item (14 checks).
- Gallery idle GC (plan item 2): component JSON is read into a reusable buffer and parsed only when the bytes changed or the caller modified the cached object; catalog state is one typed `ReadQuery<EntryData>`; visibility and camera-measure writes are typed; no LINQ/string concatenation/`MaterialPropertyBlock` allocation per frame; Editor live-tuning polls use an allocation-free field hash (`FieldSignature`) instead of `JsonUtility.ToJson`; `SyncViews` fetches names only for new views and rebuilds label strings only when the label bytes change. Idle gallery, warm, Editor (2026-11-11): `Game.Gallery.NativeRead` **0.74 → 0.085 ms/frame**; PlayerLoop 10.4 → ~9.7 ms; `Game.Gallery.Appearance` unchanged (~1.07 ms, rendering-property work, not bridge). Profiler PlayerLoop GC: gallery code **< 0.1 KB/frame** (CrystalWorldGallery Update + LateUpdate ≈ 86 B); remaining ~22 KB/frame is CapabilityRuntime.Update (capabilities owner). The "GC Allocated In Frame" counter (~88 KB) also counts Editor work outside PlayerLoop. Bridge after ~1 min: queryBuilds 29, queryRebuilds 0, queryHits ~24k.
- Change-detection semantics: query slots survive source changes but report "changed" once after any managed-script set/undo/delete (consumers such as model reconciliation relied on the full refresh the old flush produced); fixed entities in terms are re-resolved when recreated.
- Lesson (actor gait regression after the ABI 3 merge): the cause was not the bridge. The gallery's invitation and per-world pick spheres were solid colliders in the physics scene shared with an open world; the actor motor stopped against the invitation sphere (collisionFlags Sides|Below at x ≈ -1.19), so the legs stopped stepping. Whether the actor's path crossed a sphere depended on how far the gallery had animated, i.e. on frame time — so the faster bridge exposed it and bisect switches that restored old (slower) paths seemed to fix it as an interaction. Fix: pick volumes are triggers (they are only queried with `Collider.Raycast`); the gallery suite asserts it. Diagnose stalled actors with the verifier's `gait.motorTrace` and `gait.blockers` before suspecting the bridge. Verified 2026-11-11: Run-All passed (Validator 150, Resources 68, EditMode 15, PlayMode 3, Gallery 131) and Extended 12/12.
- Theme (plan item 3) ships as generated Flecs Script `Assets/__Common/Theme/ThemeConfig.flecs` (from the Editor-only `Theme.asset`), loaded at startup into the runtime theme, live in Play, with a validator stale check and build export.
- Capability coordination per-tick uncached queries were **left as is**: a few µs per tick today, and cached queries would add a component-deletion hazard while capability schemas are Script-defined. Revisit when binding counts grow.

## Phased plan

1. **Bridge hot path (partly done; remaining GC is elsewhere — profile next):** gallery `Read`/`Write` per-frame paths → typed reads/writes; stop whole query-cache flush on every managed-script set; SyncViews name allocation. Target < 0.1 ms and ~0 GC per frame for bridge traffic. Re-measure with the same benchmark.
2. **REST safety:** start REST only in Editor/development builds; port conflict non-fatal; optional read-only mode.
3. **Native systems:** ring motion (`WorldGalleryTreeMotion.flecs`, 6 copies) and capability coordination (`RuntimeCapabilities.cpp` per-tick ad-hoc queries) → compiled systems/observers. A/B measure at 6 and 1000 instances.
4. **Script hygiene:** templates/`for` for repeated rules; parse-once for repeated `fu_run_script`.
5. **Device baseline:** repeat measurements on the lowest target phone (IL2CPP).

---

# Research detail

Legend: **[V]** verified from an external source (URL given), **[C]** verified in project code (file:line), **[M]** measured by me on this machine, **[I]** my inference.

Project baseline [C]: vendored Flecs 4.2.0, commit `6879066` (submodule `Native/Core/vendor/flecs`). `FLECS_CUSTOM_BUILD` is not defined (`vendor/flecs/distr/flecs.h:215-254`), so **every default addon is compiled in, including REST/HTTP, in all builds**. Extra defines: `FLECS_SCRIPT_MATH`, `FLECS_SOFT_ASSERT`, `FLECS_KEEP_ASSERT`, `FLECS_CREATE_MEMBER_ENTITIES` (`Native/Core/CMakeLists.txt:12`). Imported at startup: Units, Metrics, Alerts, ScriptMath, Stats, ScriptInput, ScriptShell (`Native/Core/src/flecs_unity.cpp:65-71`).

## 1. What Flecs 4.x offers vs what Ecaty uses

| Feature | Ecaty status | Evidence |
|---|---|---|
| Managed (reactive) scripts | **Used heavily**: almost all app data/rules are managed scripts | `flecs_unity.cpp:143-172`; 25 `.flecs` files under Assets/Packages |
| Templates, `prop`, `mut`, async/await | Used in Workshop/Runtime behaviours (5 files use `template`, 4 use `async`) | e.g. `ActorBehavior.flecs:3`, `RuntimeLibrary.flecs:3`, `WorkshopChain.flecs:9-76` |
| `for` loops / `if` / `match` / `fn` | Barely used (1 file `for`, 3 `if`). Gallery motion copies the same rule six times for `GalleryTreeLayer1..6` | `WorldGalleryTreeMotion.flecs:2-8` (should be a template or a `for`) |
| Prefabs / IsA | 1 `.flecs` file uses `prefab`; native `SpinSystem` guards against prefab writes | `Native/App/src/Application.cpp:13-14` |
| Compiled systems / pipeline phases | **One** compiled system (`SpinSystem`, default OnUpdate). No phases, no `interval`/`rate`/tick sources, no multi-threading | `Application.cpp:13` |
| Observers / hooks | **None in native code.** Capability coordination polls every tick instead | `RuntimeCapabilities.cpp:81-111, 119-147` |
| Cached queries | `UnityViews` (Auto cache), plus a 32-entry string-keyed cache of query objects for `fu_query_json`/`fu_read_query_data`, cleared wholesale on overflow and on **every** managed-script set/undo/delete | `flecs_unity.cpp:83-84, 154, 237-258, 334, 356` |
| Change detection | Enabled for the cached bridge queries (`EcsQueryDetectChanges`), exposed as `fu_query_changes` | `flecs_unity.cpp:247, 260-266` |
| Uncached ad-hoc queries in hot paths | `coordinate()` and `footprints()` call `ecs_query_init`/`ecs_query_fini` **every tick**, plus `ecs_lookup` by name each tick. The BT condition node builds and frees a query on every evaluation | `RuntimeCapabilities.cpp:49-50, 82-83, 93-94` |
| Query DSL features (variables, `up`/`cascade`, `group_by`, `order_by`) | Variables are supported in `fu_read_query_data` (one variable target). There is no use of cascade, group_by or order_by in native code | `flecs_unity.cpp:418-429` |
| Meta/reflection | Used: C++ components declare `.member`, POD detection via EcsStruct/EcsPrimitive walk | `Components.cpp:7-40`, `flecs_unity.cpp:294-315` |
| JSON | Used as the **main C#↔native data path** for app features (entity/query/component JSON) | `flecs_unity.cpp:237-291, 403-408` |
| Metrics / Alerts / Doc | Imported; a few defined from C# (`EngineMetrics.*`, `EngineAlerts.AdapterFault`, doc briefs) | `RuntimeExperience.cs:57-60` |
| Stats | Imported (feeds Explorer stats) | `flecs_unity.cpp:69` |
| Units | Imported, no app-level unit annotations found | `flecs_unity.cpp:65` |
| Timers addon | Not used. Ecaty implements its own `engine.wait` future list in `fu_tick` | `flecs_unity.cpp:49-57, 113-122` |
| Script edits / from_scene / undo / Shell / Script Input | Used | `flecs_unity.cpp:336-390` |
| `ecs_script_parse` + `ecs_script_eval` (parse once) | Not used. `fu_run_script` re-parses every call | `flecs_unity.cpp:134-141` |
| Hierarchies | ChildOf for views (`target(flecs::ChildOf)`); template children default to the non-fragmenting `Parent` (I observed `flecs.core.Parent` in template child JSON) | `flecs_unity.cpp:193`; [M] |
| REST + Explorer | Compiled in. Started by default on 127.0.0.1:27750 by `FlecsWorld` in **every** build, with no `DEVELOPMENT_BUILD` guard, and **read-write** | `FlecsWorld.cs:10, 41`; `FlecsSession.cs:32`; `flecs_unity.cpp:87-98` |
| IR script runtime (4.2, experimental) | Disabled by choice | docs/RUNTIME-ENGINE.md |

Upstream features confirmed in the docs:
- **Script** [V] has templates, `prop`, `mut`, `export`, `const`, `for`, `if`, `match`, `fn`, `module`, `include`, struct/enum definitions, async/await and interfaces. *"Managed scripts are reactive, which means they will be reevaluated when the data that they depend on changes."* The 4.2 changes: *"Flecs Script now implements fine grained reactivity"*, re-running only the code that depends on the changed value, and templates *"reevaluated"* when a prop changes, with named children surviving rebuilds. Parse-once/eval-many is supported via `ecs_script_parse`/`ecs_script_eval`. Sources: https://www.flecs.dev/flecs/FlecsScript.html and https://medium.com/feed/@ajmmertens (4.2 post).
- **4.1 release** [V] claims script execution is *"40x faster than v4.0"*. It also added a bloom-filter query evaluation, a trivial-cache iterator, array-based caches and cached column pointers, and made change detection opt-in. https://github.com/SanderMertens/flecs/releases
- **4.2** [V] adds an experimental IR runtime *"for faster script evaluation (enable with .ir = true)"* and Explorer auto-discovery of local apps (Medium RSS).

## 2. Performance facts

### Upstream statements [V]
- Queries: *"Cached queries are really fast to iterate, but take more time to create because the cache must be initialized first"*. *"if you need to create a query ad-hoc, an uncached query makes more sense"*. Cached queries add overhead to archetype creation and deletion. https://www.flecs.dev/flecs/Queries.html
- FAQ: queries *"are expensive to create, so make sure to create them in advance."* https://www.flecs.dev/flecs/FAQ.html
- Change detection requires cached queries. It is tracked **per table and per component**, not per entity, and the docs recommend `in` terms. https://www.flecs.dev/flecs/Queries.html
- Observers: *"Hooks are much more efficient than observers"*. Systems are *"much more efficient to run, and have more predictable performance"* than observers. Wildcard observers *"add significant overhead"*. Monitors cost more than ordinary observers. A plain event queue beats observers for simple cases. https://www.flecs.dev/flecs/ObserversManual.html
- Systems: `run` and `each` show *"no significant performance difference"*. The docs also cover phases (OnLoad…OnStore), interval/rate/tick sources, and multi_threaded systems. https://www.flecs.dev/flecs/Systems.html
- REST: the docs say the built-in server *"should not be used in production environments."* https://www.flecs.dev/flecs/FlecsRemoteApi.html
- **I found no official numbers for JSON serialization or Script-vs-C cost.** That is why I took the measurements below.

### How Sander's own demo splits the work [V]
In the Tower Defense demo (https://github.com/SanderMertens/tower_defense), **all per-frame gameplay is compiled C++ systems** in `src/main.cpp` (30 KB): MoveEnemy, FindTarget (using a SpatialQuery), AimTarget, FireAtTarget, ProgressParticle and others, with `SpawnEnemy` on `.interval(0.15)`. **Flecs Script (about 8.6 KB in 15 files in `etc/assets`) defines only prefabs/assets/materials**, for example `turret.flecs` with `prefab Turret`, `slot Head` and `auto_override`. Rendering is a Flecs module (`flecs_systems_sokol`) that queries component data in-process. Gameplay instantiates with `is_a<prefabs::Bullet>()`. REST is turned on with `ecs.app().enable_rest().enable_stats()`. This matches the split recommended below.

### Measurements on this machine [M]
Setup: copy of `Assets/Plugins/Flecs/x86_64/flecs_unity.dll`, isolated world with REST off, called from Python ctypes. The script is in the scratchpad (`bench_flecs.py`). Bare call overhead was **0.35 µs**. Unity P/Invoke is cheaper, but Newtonsoft parsing plus GC allocation will be comparable or worse than Python's `json.loads` [I].

| Operation | Cost |
|---|---|
| `fu_entity_json` (1 entity, 6 comps, 1.6 KB incl. type_info) | **41 µs**, **69 µs** incl. parse |
| `fu_component_json` / `fu_set_component_json` (32 B struct) | 3.1 µs / 3.0 µs |
| `fu_read_component` (32 B binary) | 1.9 µs. Most of this is `fu_component_size` re-walking reflection on every call (`flecs_unity.cpp:317`) |
| `fu_query_json("Vec")`, N=1000 | **535 µs** (64 KB), **1.8 ms** incl. parse |
| `fu_read_query_data("Vec")`, N=1000 (memcpy) | **8.2 µs** (65× faster than JSON before parsing, about 220× including parsing) |
| `fu_write_components`, N=1000 | 109 µs (about 100 ns/entity of `ecs_set_id` + OnSet) |
| `fu_read_views`, N=1000 | 246 µs (about 250 ns/entity: 4 `try_get` + ChildOf target lookup per entity) |
| `fu_tick` baseline (near-empty world) | 34 µs (includes per-tick query init/fini in caps) |
| +1000 Clock-dependent managed Script expressions | **+1.5 ms/tick** (about 1.5 µs/expression) |
| +compiled `SpinSystem` over 1000 entities | **+64 µs/tick** (about 64 ns/entity, roughly **23× cheaper** than Script) |
| Instantiate 1000 template instances (2 entities each) | 17.7 ms (about 18 µs/instance) |
| Reload the same 1000-instance source with one value changed | 18 ms (a source reload is a full re-evaluation) |
| Change one instance's template prop as a component + tick | +18 µs (incremental fine-grained reactivity works) |
| `fu_run_script` one-line assignment | about 20 µs each (parse + eval every call) |

How to read these numbers [I]:
- Script reactivity is excellent for **event-rate** changes: a prop or intent changes and only the dependents re-run. It is about 20–25× slower than compiled code for **per-frame continuous** math such as anything depending on `Clock`.
- JSON is fine for agent and tool traffic. As a per-frame path it costs 50–200× more than binary transfer.
- To change a template parameter, write the template component. Do not re-send the source.

### Current per-frame bridge traffic in the Gallery [C]
`CrystalWorldGallery.Update` (`CrystalWorldGallery.cs:193-212`) calls `Read()`, which calls `fu_entity_json` on the **whole entity with `serialize_type_info=true`** (`flecs_unity.cpp:287-290`) and then `JObject.Parse` (`CrystalWorldGallery.cs:72-79`). Results are memoised per entity per frame:
- Every frame: `GalleryAnimation`, `GalleryTreeSettings` and one `GalleryTreeLayerN` per active ring (up to 6), so about 3–8 full-entity JSONs per frame.
- `SetPaused` can also `Write` up to 7 JSON components per frame when pause state changes (`:186-191`).
- Every 0.1 s: `ReadState` runs `Query("GalleryEntry")` as JSON (`:159`), and `Arrange` may call `SetManagedScript("GalleryTreeBindings")` (`:181`). That call also flushes the whole query cache (`flecs_unity.cpp:154`).

Expected cost [I]: about 0.3–0.6 ms/frame of CPU plus several KB of managed garbage per frame. That is not catastrophic on desktop, but it is pure overhead, and the GC pressure matters on mobile.

`FlecsWorld.SyncViews` also marshals `EntityName` to a new C# string **for every view, every frame** (`FlecsWorld.cs:72`).

## 3. Recommended split

**Native compiled C++ (modules in `Native/Core` / `Native/App`):**
- Stable reflected component *contracts*: Transform/Presented/Clock, motor/pose/intent structs, capability state, gallery state structs (GalleryLayerIntent/State, GalleryFocus, GalleryView…). Define them once in C++ with `.member` so they are blittable and visible to Script, JSON and REST alike.
- Every **per-frame continuous** rule: ring turn integration (currently `WorldGalleryTreeMotion.flecs` × 6 copies), motion integration, easing/progress states driven by `Clock`, steering, `cap_ready`/dependency coordination, footprint generation. Write these as systems in explicit phases, for example PreUpdate for intents, OnUpdate for simulation, PostUpdate for derived state, PreStore for snapshot export. Use cached queries built once at module import.
- Event reactions: hooks for invariants on a single component, and observers (OnSet / custom events / `yield_existing`) for "capability became dirty". These replace per-tick polling in `RuntimeCapabilities.cpp`.
- Timers: use `interval`/tick sources for low-rate work (persistence autosave, capability coordination at 10 Hz) instead of hand-rolled throttles like `nextRead` in C# (`CrystalWorldGallery.cs:196`).

**Runtime-defined (Flecs Script, prefabs, user content):**
- Templates and prefabs for content: models, behaviours, the gallery catalog and layout parameters. Use `for`/templates instead of copy-paste (`GalleryTreeLayer1..6`).
- Event-driven logic: async/await on input, `mut` state, reactive bodies that change on *intent* changes and not every frame.
- User/AI experiences, parameter overrides, new reflected struct types, metrics/alerts definitions, doc strings.
- Rule of thumb [I]: if an expression references `Clock[Clock].time` and has more than a handful of instances, it belongs in a compiled system. It can stay parameterised by Script-authored components, for example a native `Spin`/`Oscillate`/`Tween` component that Script instantiates.

**Cheap typed ABI to Unity [I, building on existing pieces]:**
1. **Snapshot by cached query into blittable arrays.** `fu_read_query_data` already proves the pattern at about 8 ns/entity.
   - Generalise it to multi-field queries: return per-field column pointers or memcpy several fields into a caller struct-of-arrays.
   - Expose a stable **query handle** (`fu_query_create(expr) → id`, `fu_query_fini(id)`) instead of string-keyed lookup with whole-cache flushes. Rebuild queries only when the types they reference change, not on every managed-script set.
2. **Change-detected batches.** Iterate the cached query with `ecs_iter_changed`/`ecs_iter_skip` and emit only changed tables. Or keep a native "dirty since frame N" list fed by an OnSet observer, which gives per-entity granularity.
   - Unity then pays for what changed, not for N entities × 4 `try_get`.
   - `fu_read_views` should become a cached query with optional terms `Position, ?Rotation, ?Scale, ?Color, ?Primitive, ?Label`, reading field arrays directly. Expected 5–10× faster [I].
   - Return names only for new or renamed entities.
3. **Cache type IDs and sizes once** (C# already caches IDs in `typeIds`). Add a native-side size cache keyed by type so `fu_read_component`/`fu_write_components` don't re-walk meta on every call.
4. **Gallery:** replace `Read()`/`Write()` JSON with `TryRead<T>`/`WriteBatch<T>` over `[StructLayout(Sequential)]` mirrors of the gallery structs. Read the `GalleryLayerState` of all rings in **one** `ReadQuery<T>("GalleryLayerState")` call. Keep JSON for the agent, tools, persistence and diagnostics.
5. **Optional zero-copy:** a native per-frame staging buffer (pinned, owned by native, valid until the next `fu_tick`) that Unity reads via `NativeArray` (`NativeArrayUnsafeUtility.ConvertExistingDataToNativeArray`). This avoids even the memcpy, and lets Burst jobs consume Transform data or push it to `TransformAccessArray`. This is the same pattern as the Unreal ISM batching in community integrations (https://jtferson.github.io/blog/quickstart_with_flecs_in_unreal_part_2). Do this only if profiling shows the copy matters.
6. Use `ecs_script_parse` once with `ecs_script_eval` for repeated `fu_run_script` commands with variables, or replace them with typed writes.

## 4. REST/Explorer in an embedded world, safely

Current state [C]:
- `FlecsWorld.restPort = 27750` (`FlecsWorld.cs:10`), and the `FlecsSession` default is 27750 (`FlecsSession.cs:32`). REST starts in the Editor **and release players**.
- It binds to 127.0.0.1 (`flecs_unity.cpp:91`), which is good.
- It uses `ecs_rest_server_init`, which is **read-write**: the read-only flag only exists on the `EcsRest` singleton path (`vendor/flecs/distr/flecs.h:13613-13622`) [C/I].
- So any local process can `PUT script/…`, `PUT component/…`, `DELETE entity/…` and use Shell sessions. Script and shell endpoints can even save script files [V: FlecsRemoteApi].
- If the port is busy (another Ecaty instance or another Flecs app), `fu_create` returns null and **world creation fails** (`flecs_unity.cpp:93-96`).

Recommendation:
- Make REST opt-in: `#if UNITY_EDITOR || DEVELOPMENT_BUILD` plus an explicit toggle, and port 0 in release.
- Optionally strip it from release natives with a `FLECS_CUSTOM_BUILD` variant, or `FLECS_NO_REST`/`FLECS_NO_HTTP` for the release config [I].
- Keep the bind on `127.0.0.1`. The `EcsRest` default is `0.0.0.0` [C: `flecs.h` EcsRest comment], so set ipaddr explicitly if switching to the singleton.
- Offer read-only mode for routine inspection. Either use `world.set<flecs::Rest>({port, "127.0.0.1", readonly=true})`, which REST then services during `progress()`, or wrap the request callback to reject non-GET requests.
- Make a port conflict non-fatal: log it and continue without REST.
- Your current pattern of servicing requests via `ecs_http_server_dequeue` in `fu_tick` outside `progress()` is correct for structural edits on the main thread.

Tool access: yes, over plain HTTP [V].
- Examples: `curl "http://127.0.0.1:27750/query/?expr=GalleryLayerState"`, `GET /entity/<path>`, `GET /world`, `GET /stats/world`, `GET /stats/pipeline`, `GET /components`, `GET /queries`, `GET /tables`.
- The JS client is `flecs.connect("localhost")`.
- Explorer: https://www.flecs.dev/explorer auto-connects to localhost:27750. Use `?host=127.0.0.1:27750` or `?remote=true` if needed, or self-host it. 4.2 also auto-discovers local apps. https://github.com/flecs-hub/explorer
- No auth exists. Treat it as dev-only [V].

## 5. Measurements to take before deciding

1. **Unity Profiler markers around every bridge call per frame.** Count calls, measure ms, and record GC alloc/frame from `Game.Gallery.NativeRead` and from `SyncViews`. Expect about 0.3–0.6 ms and several KB per frame from gallery JSON reads, and about 0.25 µs/view in `fu_read_views` plus string allocations. Target: under 0.1 ms and zero GC per frame.
2. **`fu_tick` breakdown.** Time `ecs_progress`, the managed-script reactive re-evaluation, and `fu_caps_tick` separately. Enable `FlecsStats` and read `GET /stats/pipeline` in the Explorer. Expect reactive Clock expressions at about 1.5 µs each, and per-tick query init/fini in caps at tens of µs.
3. **Script vs native A/B on the same rule.** Move the ring-turn rule into a native system and compare tick time at 6 rings and at a synthetic 1000 instances. Expect about 20× at scale and noise at 6.
4. **Template scale.** Instantiate cost (about 18 µs/instance measured), prop-change propagation (about 18 µs), and full source reload (scales linearly with instances). Decide whether AI edits should use `fu_script_edit`, prop writes or whole-module reloads.
5. **Query cache churn.** Track `fu_bridge_diagnostics` `queryBuilds` vs `queryHits` across a session. Any rebuild per 0.1 s (from `SetManagedScript` flushing) is a red flag.
6. **Device baseline.** Repeat 1–2 on the lowest target phone with IL2CPP. JSON+Newtonsoft and GC costs typically grow 3–10× relative to native memcpy on mobile [I].
7. **Optional: the IR runtime** (`.ir = true`) on the reactive-expression benchmark, behind a flag only.

## Key sources
- Flecs Script manual: https://www.flecs.dev/flecs/FlecsScript.html
- Queries manual: https://www.flecs.dev/flecs/Queries.html
- Observers manual: https://www.flecs.dev/flecs/ObserversManual.html
- Systems manual: https://www.flecs.dev/flecs/Systems.html
- Remote API: https://www.flecs.dev/flecs/FlecsRemoteApi.html
- FAQ: https://www.flecs.dev/flecs/FAQ.html
- Releases (4.0.5–4.2.0): https://github.com/SanderMertens/flecs/releases
- Sander's articles via RSS (Medium blocks direct fetch): https://medium.com/feed/@ajmmertens
- Tower Defense demo: https://github.com/SanderMertens/tower_defense (src/main.cpp, etc/assets/*.flecs)
- City demo: https://github.com/flecs-hub/city
- Explorer: https://github.com/flecs-hub/explorer
- Unreal community batching example: https://jtferson.github.io/blog/quickstart_with_flecs_in_unreal_part_2

Not accessible or not covered: Discord; direct Medium pages (403); any official JSON or Script throughput benchmark (none found).
