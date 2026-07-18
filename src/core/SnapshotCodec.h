#pragma once

#include <QObject>
#include <QJsonObject>
#include <QJsonArray>
#include <QString>
#include <QStringList>

namespace gbr {

class SimulationEngine;
class Scenario;
class ScenarioUnit;

/// @brief Encode/decode full-scenario and runtime-state JSON snapshots.
/// @details Stage 0 implementation. Two distinct payloads are produced:
///   - @c encodeScenario — the canonical scenario (ScenarioIo JSON), no runtime data.
///     Used at handshake to give a joining peer the same starting world.
///   - @c encodeRuntime / @c decodeRuntime — per-unit runtime state (HP, status,
///     recent path tail, jamming factor, schedule sample). Used for incremental
///     sync so a freshly-joined peer catches up to current state.
/// @details Diffing (computing the delta between two snapshots) is intentionally
/// out of scope for Stage 0; the network loop in Stage 2 will own that logic.
class SnapshotCodec {
public:
    static constexpr int RuntimeSchemaVersion = 1;
    /// @brief Serialize a Scenario (no runtime state) to compact JSON.
    /// @details Equivalent to @c ScenarioIo::toJson() but exposed here so the
    /// network layer has one place to look for snapshot encoders.
    static QJsonObject encodeScenario(const Scenario& s);

    /// @brief Inverse of @c encodeScenario.
    /// @returns empty Scenario on parse failure (caller can validate).
    static Scenario decodeScenario(const QJsonObject& obj);

    /// @brief Collect every alive unit's runtime state into a JSON array.
    /// @details Caller-supplied engine; the engine is read-only here.
    static QJsonArray encodeRuntimeUnits(const SimulationEngine& engine);

    /// @brief Versioned runtime snapshot used by persistence and networking.
    static QJsonObject encodeRuntime(const SimulationEngine& engine,
                                     qint64 serverTick = -1,
                                     qint64 scenarioRevision = 1);

    /// @brief Apply a runtime snapshot back to the engine: full replace per unit.
    /// @details For units present in the snapshot: set HP, position, status,
    /// jamming factor, schedule, recent path. For units NOT in the snapshot:
    /// no change (the joiner is expected to keep its existing world).
    static void decodeRuntimeUnits(SimulationEngine& engine, const QJsonArray& units);

    /// @brief Pretty diff: list unit ids whose runtime fields differ.
    /// @details Used by tests to confirm that two peers converge after sync.
    /// Cheap structural comparison (HP, position, status).
    static QStringList diffUnitIds(const QJsonArray& a, const QJsonArray& b);
};

} // namespace gbr
