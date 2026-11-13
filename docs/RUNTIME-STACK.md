# Runtime stack

Updated 2026-11-06. Current implementation and limitations: [RUNTIME-CAPABILITIES](https://github.com/Ecaty/capabilities-unity/blob/main/Documentation~/RUNTIME-CAPABILITIES.md). This is the selected stack, not a list of every plugin feature.

| Area | Runs in | Selected solution / state |
|---|---|---|
| World, reflection, queries, relationships, reactive/event rules | Native | Flecs 4.2 C core + C++ host, runtime Script; integrated |
| Optional complex tasks | Native | BehaviorTree.CPP runtime XML; FlecsSet/FlecsQuery/SimWait leaves; integrated and native-tested |
| Instrument/procedural sound | Native + Unity playback | TinySoundFont, bundled GeneralUser GS, simple sine/noise voices; shared bank memory; integrated |
| Footprints | Native | Clipper2 outputs ECS polygon rings; integrated. It does not replace a 3D mesher or PhysX |
| Organic geometry/pivots | Unity | Existing MudBun adapter |
| Curve and filled-area geometry | Unity | Unity Splines + Spline Mesher Pro adapter |
| Modular buildings | Unity | Cozy Builder registered kits + runtime rebuild adapter |
| Vegetation | Unity | Broccoli registered pipeline clones + seeded generation adapter |
| Ground | Unity, coordinated in Flecs | Bounded heightfield mesh/collider + spline modifier; dependency rebuild. Texture/scatter exclusion and multi-modifier terrain remain future work |
| Physical objects / actors | Unity | Sole PhysX Rigidbody adapter or existing CharacterController motor; explicit ownership, commands/readback |
| Navigation | Unity | AI Navigation runtime surface + path-direction adapter; existing motor executes movement. No separate Detour |
| Animation | Unity | Existing FImpossible/Boing bindings; runtime Animation Rigging TwoBoneIK adapter |
| Camera / sequencing | Unity | Cinemachine follow + Timeline/Playables audio track adapter |
| Appearance/effects | Unity | Material/light/ParticleSystem adapters, registered precompiled VFX Graph assets |
| Feedback/music | Unity | Feel feedback palette + quantized sound sequencer adapters |
| Spatial layout / handles | Unity | Flexalon circular layout + direct manipulation capability; finished spatial creation UX is separate |
| Water physics | Unity | Simple level/density plane + buoyancy on the sole PhysX body |
| Weather/ocean rendering | Unity | Initial UniStorm/Crest bindings; configured rendered-scene verification remains |
| Save/resume | Flecs + selected adapter state | Rebuild from sources/configuration and selected physical state; not arbitrary VM/plugin checkpoints |

MicroSplat and TVE are excluded. Meshoptimizer is reserved for a future profiled baking workflow. No Jolt, new collider solver, standalone Detour, FastNoise or STK was added. No additional purchases/imports are required to run the verified workshop with the plugins already installed.

Native code owns portable data/algorithms; Unity/plugin adapters own execution against their systems. AI and human tools author the same ECS configuration and recipes. Normal frame simulation makes no inference calls. Optional vendor bindings compile independently according to detected imports; no universal Inspector-reflection adapter is used.

Windows x64 is the verified development target. Mobile/XR/WebGL/IL2CPP and full shader/platform compatibility need separate builds and tests. Initial bindings do not imply every feature of each package is exposed.
