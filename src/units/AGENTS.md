# UNIT IMPLEMENTATIONS

## OVERVIEW

`src/units` implements the five runtime unit kinds: command post, recon UAV, attack UAV, ground scout and jammer UAV.

## WHERE TO LOOK

| Task | Location |
|------|----------|
| Base behavior | `MobileUnitBase.*`, `src/core/UnitBase.*` |
| Unit factory/type registration | `src/core/UnitBase.cpp`, `UnitKind` enum |
| Command post | `CommandPost.*` |
| Mobile units | `ReconUAV.*`, `AttackUAV.*`, `GroundScout.*`, `JammerUAV.*` |
| Behavior tests | `tests/test_unitfsm.cpp`, `test_engine.cpp`, `test_attack_power.cpp` |

## CONVENTIONS

- New unit types require the `UnitKind` name conversion, `UnitBase::create()` factory branch, paired source files and the root domain source list.
- Mobile unit constructors call `setupFsm()` and `onTick(double)` delegates movement/state behavior to `m_fsm`.
- Preserve the base invariants for health, ranges, communications, ownership, death and subsystem damage.
- Unit behavior reports facts through the existing message/command interfaces; orchestration and authorization stay in `SimulationEngine` or `server/game`.
- Keep unit parameters and serialized kind names compatible with scenario and checkpoint formats.

## ANTI-PATTERNS

- Do not call QML, `NetworkClient`, account APIs or server room classes from a unit.
- Do not add a new unit kind without factory, serialization and focused behavior coverage.
- Do not make a unit decide whether a remote client is authorized to control it.

## TESTS

Run the unit FSM and engine tests after changing tick behavior, unit parameters, factory registration or serialized unit kinds.
