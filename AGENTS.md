# AGENTS.md

OpenCode instructions for the `wargame-master` (兵器推演) repository.

## Build & Run

```bash
# One-time configure (adjust CMAKE_PREFIX_PATH to your Qt6 install)
cmake -S . -B build -G Ninja \
  -DCMAKE_PREFIX_PATH=/path/to/Qt/6.x/gcc_64 \
  -DCMAKE_BUILD_TYPE=Debug

ninja -C build           # incremental build
./build/appindex         # run (binary is "appindex", not "index" or "wargame")
./build/wargame_tests    # run unit tests
```

CMakeLists.txt requires Qt 6.10+ (`REQUIRES 6.10` in qt_standard_project_setup). The `.pro` file is a qmake leftover — **ignore it**, build with CMake only.

Google Test is fetched via `FetchContent` during CMake configure. No manual setup needed.

## Architecture (must-know)

- **Single process**: C++ backend (`SimulationEngine`) drives all units at 50ms ticks. QML UI reads state via `controller.*` context property. No network, no multi-process.
- **`gbr` namespace**: all C++ classes.
- **`controller`** (`SimulationController`) is the sole C++↔QML bridge. QML binds `controller.viewMode`, `controller.focusedSide`, `controller.units`, etc. Never call `engine->bus()` or unit methods from QML directly.
- **`controller.command(action, args)`** is the universal action entry point — QML calls this, `SimulationEngine::command` dispatches to `MessageBus`.
- **View modes**: exactly 4: `editor`, `commandpost-red`, `commandpost-blue`, `director`. Confirmed by `SimulationController::viewModeOptions()` and `SimulationRoot.qml` ListModel+Loader. Old views (`ReconUavView.qml`, `AttackUavView.qml`, `GroundScoutView.qml`) have been **removed** from disk.
- **Unit types**: 5 kinds — `CommandPost`, `ReconUAV`, `AttackUAV`, `GroundScout`, `JammerUAV`. Confirmed in `UnitBase.h` enum.
- **UnitFsm**: Each mobile unit (AttackUAV, ReconUAV, JammerUAV, GroundScout) now uses `UnitFsm` for state management (idle/moving/patrolling/withdrawing). `onTick` delegates to `m_fsm.tick(dt)`. New states should follow this pattern.

## Critical invariants

- **Exactly one CP per side**: `SimulationEngine::recomputeReadyForSim()` requires each side to have exactly one alive CommandPost. Missing/duplicate/destroyed → `readyForSim = false` → run button disabled, red text, auto-pause.
- **CP ids are hardcoded**: `SimulationEngine::command` uses `"red_cp"` / `"blue_cp"` literally for sender resolution when no live CP is found. Any scenario should name its command posts exactly these ids to avoid fallback issues.
- **CP comms bypass distance**: `MessageBus::canCommunicate` checks `isCp` flag — CPs can always reach their own side's units.
- **ECM jamming**: Each tick resets all units' jamming to 1.0, then `JammerUAV` applies 0.4 multiplier to hostile units within `detectRange` (repurposed as jam range), scaling their `detectRange` and `commRange`.
- **CP detect dedup**: `CommandPost` won't re-report a `targetId` already in `m_pending` or `m_targets`.
- **Command alive guard**: All `SimulationEngine::command()` actions now reject dead units.

## Adding new unit types

1. `src/core/UnitBase.h` — add to `enum class UnitKind` + `kindName`/`kindFromName`.
2. `src/core/UnitBase.cpp` — add branch in `create()` factory.
3. `src/units/Foo.h` + `Foo.cpp` — subclass `UnitBase`, implement `onTick(double)`, call `setupFsm()` in constructor and delegate to `m_fsm`.
4. `CMakeLists.txt` — add to `corelib` source list.
5. If new actions needed: `SimulationEngine::command` + `Message::Type`.

## Adding new view modes

1. `qml/views/FooView.qml` (top-level `Item`).
2. `SimulationRoot.qml` — add `ListElement` to `viewCombo` model + case in `pageLoader.sourceComponent` + `Component { id: fooPage; FooView {} }`.
3. `SimulationController::viewModeOptions()`.
4. `CMakeLists.txt` — add the new `.qml` file to `QML_FILES`.

## File locations

| Concern | Path |
|---------|------|
| C++ core (engine, bus, geo, scenario, map) | `src/core/` |
| Unit implementations | `src/units/` |
| QML↔C++ bridge | `src/view/SimulationController.{h,cpp}` |
| Scenario file IO helper | `src/view/ScenarioEditor.{h,cpp}` |
| QML views | `qml/views/` |
| QML reusable components | `qml/components/` |
| Unit tests | `tests/` |
| Saved scenarios | `./scenarios/editing.json` |
| Map asset | `assets/sample_map.json` |

## Caveats

- **README.md is stale**: it lists `recon`/`attack`/`ground` views and omits `JammerUAV`. Trust `SimulationController::viewModeOptions()`, `UnitBase.h` enum, and `CMakeLists.txt` QML_FILES over README.
- `UnitFsm` is now actively used by all mobile units. New units must follow the same pattern: `setupFsm()` in constructor, `m_fsm.tick(dt)` in `onTick`.
- All UI text, comments, and status messages are **Chinese**. Keep new strings in Chinese.
- QML uses `QtQuick.Controls.Basic` (not Material or Fusion). Theme object `id: theme` in `SimulationRoot.qml`; each view copies to local `id: t`.
- Heavy QML JS computations (`attackableTargets`, `detectedEnemyOptions`, etc.) are moved to C++ `SimulationController` for performance. Add new QML-facing queries there, not as JS functions.
- Use `controller.commandPostIdFor(side)` instead of hardcoding `"red_cp"`/`"blue_cp"` in QML.

