# Third-party code

The only outside GitHub code in the engine is in `vendor/`, each a submodule of our fork in the Ecaty org. The submodule commit is the pin. Never edit these sources in place.

| Path | Fork | Original tracked | Fork branch holding the pin |
|---|---|---|---|
| `vendor/flecs` | Ecaty/flecs | SanderMertens/flecs | master |
| `vendor/behaviortree` | Ecaty/BehaviorTree.CPP | BehaviorTree/BehaviorTree.CPP | master |
| `vendor/clipper2` | Ecaty/Clipper2 | PolyClipping/Clipper2 (the fork comes from AngusJohnson/Clipper2, which lacks these commits) | ecaty |
| `vendor/tinysoundfont` | Ecaty/TinySoundFont | schellingb/TinySoundFont | main |

## Checkout

- Windows: Flecs has very long test paths. Run `git config --global core.longpaths true` once before cloning.
- Use `git submodule update --init --recursive --depth 1` (or `git clone --recurse-submodules --shallow-submodules`). Without `--depth 1`, a pin behind the fork's tip pulls full history (Flecs alone is about 230 MB instead of about 30 MB).
- Clipper2's CMake rewrites `clipper.version.h` in its own source tree on every configure; `.gitmodules` sets `ignore = dirty` for that submodule.
- BehaviorTree.CPP forces `-Werror`/`/WX` on its target; `CMakeLists.txt` makes warnings non-fatal for that target only.

## Upstream check and updates

- `.github/workflows/upstream-check.yml` (in this repo) runs weekly or by hand. It compares each pin with the original's default branch and latest release, and opens or updates one `upstream` issue per library here. It changes nothing.
- To update a library:
  1. `gh repo sync Ecaty/<fork>`. For Clipper2, push the wanted PolyClipping commit to the fork's `ecaty` branch instead.
  2. `git -C vendor/<lib> fetch --depth 1 origin <commit>`, then `checkout <commit>`.
  3. Rebuild through the host product (for example its `Native/Build.ps1`); the binary reports `FLECS_VERSION+<flecs short commit>`.
  4. Run the host's native smokes, plus its full Unity suite for a Flecs update.
  5. Commit the pin here, then the pointer in the host product. Close the issue.
- If a library ever needs a patch, commit it on an `ecaty` branch in its fork and pin that.
