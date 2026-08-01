# CLIENT VIEW BRIDGE

## OVERVIEW

`src/view` is the client-only bridge between QML and domain/network code: `SimulationController`, scenario file operations and map tile rendering.

## WHERE TO LOOK

| Concern | Location | Notes |
|---------|----------|-------|
| Main facade | `SimulationController.*` | Exposes properties, signals and invokables to QML |
| Scenario files | `ScenarioEditor.*` | JSON/file helper, not simulation authority |
| Map drawing | `MapTileRenderer.*` | Coordinate conversion and tile painting |
| QML root | `Main.qml`, `qml/SimulationRoot.qml` | Initial property injection and page orchestration |

## CONVENTIONS

- `main.cpp` injects the controller/editor into `Main.qml`; QML must not instantiate a second controller.
- Local mode wraps `SimulationEngine`; online mode wraps `NetworkClient` and projected state. Keep these mode differences explicit in properties and signals.
- Generic QML actions use `controller.command(action, args)`. Scenario editing uses the dedicated replace/batch/transform APIs.
- QML notifications are not authorization or durability acknowledgements. Online UI must wait for server-confirmed seat, deployment and command results.
- Invalidate controller caches when replacing snapshots or scenario units; keep query helpers such as `unitAt` and target selection consistent with the current revision.
- `ScenarioEditor` handles files and `MapTileRenderer` handles painting; neither owns room permissions or simulation rules.

## ANTI-PATTERNS

- Do not expose `SimulationEngine`, `MessageBus` or network internals directly to QML.
- Do not implement authoritative validation only in controller/QML convenience flags.
- Do not rebuild or mutate a remote full state as if it were a local editable scenario.
- Do not leak credentials through properties, signals or diagnostic strings.
