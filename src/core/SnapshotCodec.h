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

/// @brief 编解码完整场景和运行时 JSON 快照。
/// @details 快照分为两类：
///   - @c encodeScenario — the canonical scenario (ScenarioIo JSON), no runtime data.
///     由服务器用于描述按角色裁剪后的可见世界。
///   - @c encodeRuntime / @c decodeRuntime — per-unit runtime state (HP, status,
///     recent path tail, jamming factor, schedule sample). Used for incremental
///     用于让客户端镜像跟上权威服务器状态。
class SnapshotCodec {
public:
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

    /// @brief Apply a runtime snapshot back to the engine: full replace per unit.
    /// @details For units present in the snapshot: set HP, position, status,
    /// 干扰系数和计划；快照中不存在的单位保持不变。
    static void decodeRuntimeUnits(SimulationEngine& engine, const QJsonArray& units);

    /// @brief Pretty diff: list unit ids whose runtime fields differ.
    /// @details Used by tests to confirm that two peers converge after sync.
    /// Cheap structural comparison (HP, position, status).
    static QStringList diffUnitIds(const QJsonArray& a, const QJsonArray& b);

    static QJsonArray encodeCheckpointUnits(const SimulationEngine& engine);
    static bool decodeCheckpointUnits(SimulationEngine& engine, const QJsonArray& units,
                                      QString* error = nullptr);
};

} // namespace gbr
