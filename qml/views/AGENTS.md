# QML ROLE VIEWS

## OVERVIEW

`qml/views` contains the scenario editor, local command-post/director views and the online operations view selected by `SimulationRoot.qml`.

## CONVENTIONS

- `SimulationRoot.qml` owns page selection, view mode and global session/error state; each view owns only its role-specific presentation and interaction.
- Existing local modes are editor, `commandpost-red`, `commandpost-blue` and director. Online operations has separate session/seat/permission state.
- Pass the injected controller and scenario editor into a view. Use controller signals for snapshots/events and controller methods for actions.
- Scenario editor changes must use explicit edit APIs and be blocked while the simulation is running or when the online server denies editing.
- Command-post views must respect side/unit control restrictions and current projected visibility. Director actions are still checked by C++/server.
- Add a view by updating its QML file, `SimulationRoot.qml` model/loader and `CMakeLists.txt` QML module list.

## ANTI-PATTERNS

- Do not copy controller/network state into a second authoritative model.
- Do not treat a stale snapshot, QML timer or optimistic binding as server confirmation.
- Do not add a view-specific direct engine call or hardcode command-post ids when `controller.commandPostIdFor(side)` exists.
- Do not reopen end-of-match dialogs from repeated notifications after the engine has stopped.

## MODES

- Editor owns stopped local scenario editing and validation.
- Command-post views own side-specific control and projected unit information.
- Director owns observation and director-only lifecycle controls.
- Online operations owns account, room, seat, deployment and asynchronous server feedback.

## VALIDATION

Run `cmake --build build/debug --target all_qmllint` after view changes and manually verify the affected local or online role flow when bindings or permissions change.
