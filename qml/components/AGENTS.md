# QML COMPONENTS

## OVERVIEW

`qml/components` provides reusable map, panel, dialog, button and settings controls shared by role views.

## WHERE TO LOOK

| Component | Purpose |
|-----------|---------|
| `MapCanvas.qml`, `Minimap.qml` | Map interaction, coordinate display and unit selection |
| `CommandPanel.qml`, `UnitPanel.qml` | Unit actions and status |
| `ChatPanel.qml`, `MessageLog.qml`, `EventDialog.qml` | Communication and event presentation |
| `SessionDialog.qml`, `SettingsPanel.qml` | Session and client settings |
| `*Dialog.qml`, `TonalButton.qml`, `GhostButton.qml`, `Icon.qml` | Reusable editing/visual controls |

## CONVENTIONS

- Components receive `controller` and other data through properties; keep them usable from multiple views without reaching into global engine objects.
- Send generic orders through `controller.command(...)`; use explicit controller APIs for scenario editing, online seat and session actions.
- `MapCanvas` should use controller query APIs and stable snapshot data. Avoid timers or bindings that rebuild complete unit lists unnecessarily.
- Keep dialog commit/cancel behavior explicit and do not display server-side success before the corresponding signal/state update.
- Follow the existing Basic-controls theme, compact panel hierarchy and keyboard/focus behavior.

## ANTI-PATTERNS

- Do not duplicate authorization or combat rules in a component.
- Do not store credentials in QML properties, `localStorage` or visible error text.
- Do not use nested decorative cards, color-only status, or layout-changing transforms for operational state.

## VALIDATION

Run the QML lint target after changing component properties, signal handlers, bindings or imports, and manually exercise the affected role view when the component carries live session state.
