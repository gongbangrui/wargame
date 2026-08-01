# QML CLIENT

## OVERVIEW

`qml/` contains the Qt Quick shell, role views and reusable controls. It is a presentation/client interaction layer over the injected `controller` facade.

## WHERE TO LOOK

| Area | Location |
|------|----------|
| Root window and initial properties | `Main.qml`, `qml/SimulationRoot.qml` |
| Role pages | `qml/views/` |
| Reusable controls and map | `qml/components/` |
| C++ contract | `src/view/SimulationController.h` |

## CONVENTIONS

- `Main.qml` receives required `controller` and `scenarioEditor` properties from C++; do not create them in QML.
- Pass the controller down through page/component properties. Invoke business actions via `controller.command(action, args)` or the documented controller methods.
- Use `Connections` for controller notifications and keep derived UI state tied to the current snapshot/revision.
- Permission flags such as `simulationControlAllowed` and `unitControlAllowed` are UX gates only; server/C++ validation remains mandatory.
- Keep view modes and command strings aligned with `SimulationController` options and protocol values. Add a new view to the QML module list and view model/loader.
- Use `QtQuick.Controls.Basic` and the existing theme conventions. Keep map/UI state readable without relying on color alone.

## ANTI-PATTERNS

- Do not access engine, message bus, network sockets or account APIs directly from QML.
- Do not optimistically display online seat ownership, deployment or command success.
- Do not put heavy domain calculations in repeated QML JavaScript loops when a controller query/cache is the established path.
- Do not use QML-only permissions as a security boundary or transmit secrets in settings/local storage.
