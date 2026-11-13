# Native runtime core backlog

Engine tasks for this repo only; product tasks live in the product's `docs/BACKLOG.md`. Keep it short; mark work done only with current evidence.

## Open

1. **Engine neutrality check.** Look for product assumptions in Core (names, world doc name, gallery/workshop specifics, product comments) and move anything product-specific to the application module.
2. **Member ranges and units on tunable native fields.** `fu_type_layout` already exports them; adding `.range()` gives Explorer sliders, validation and AI field descriptions (FLECS-NOTES item 7).
3. **Event-driven capability coordination** (observer + dirty list + low-rate timer, cached queries). Blocked while capability components are Script-defined and may be redefined (redefinition deletes observers and orphans cached queries); polling costs a few µs per tick today. Revisit when binding counts grow.
4. **Verify on Flecs 4.2 before relying on them:** `(ChildOf, $p)` queries matching `Parent` children; change detection on sorted queries; lenient script loading; whether reloading a managed script keeps instance overrides (see FLECS-NOTES).
5. **Optional, measure first:** IR script runtime (`.ir = true`) behind a flag; `group_by` per world once several worlds share one Flecs world.

## Decisions

- 2026-11-13: Third-party code only through Ecaty forks as shallow submodules; weekly upstream check in this repo; updates manual (docs/THIRD-PARTY.md).
- 2026-11-13: Native systems/observers in `engine.*` modules, components at the root. Capability coordination runs as an immediate pipeline system after OnStore.
- 2026-11-13: No separate release build without REST/HTTP for now: REST never starts in release players, and a second plugin variant per build type is not worth it yet.
