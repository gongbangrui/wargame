#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QString>

namespace gbr {

class SimulationEngine;

class StateProjector final {
public:
    static QString sideForRole(const QString& role);
    static bool canEditSide(const QString& role, const QString& side);
    static bool canControlSide(const QString& role, const QString& side);
    // 网络通信是有方向的：发送方的通信半径决定能否把信息送到接收方。
    // 该方法不依赖本地 MessageBus；服务端直接按定向通信图投影。
    static bool canTransmit(const SimulationEngine& engine, const QString& senderUnitId,
                            const QString& recipientUnitId);
    static void resetReachabilityCacheStats();
    static quint64 reachabilityGraphBuildCount();
    static quint64 reachabilityBfsTraversalCount();
    static QSet<QString> visibleUnitIds(const SimulationEngine& engine, const QString& role);
    static QSet<QString> visibleUnitIds(const SimulationEngine& engine, const QString& role,
                                        const QString& ownedUnitId);
    static QSet<QString> visibleUnitIds(const SimulationEngine& engine, const QString& role,
                                        const QSet<QString>& explicitlyShared,
                                        const QString& ownedUnitId = {});
    // Returns the ordinary detection/communication view used to update the
    // intelligence ledger. Active scan contacts are intentionally excluded;
    // they are transient ability output rather than persistent observations.
    static QSet<QString> sensorVisibleUnitIds(const SimulationEngine& engine,
                                               const QString& role,
                                               const QString& ownedUnitId = {});
    static QJsonArray filteredMessages(const SimulationEngine& engine, const QString& role,
                                       const QString& ownedUnitId = {});
    /// Project the durable workflow snapshot to the bounded wire shape. The
    /// complete snapshot remains server/checkpoint-only because it contains
    /// event history and sequencing metadata.
    static QJsonObject projectWorkflow(const QJsonObject& workflow);
    static QJsonObject projectEvent(const SimulationEngine& engine, const QString& role,
                                    const QJsonObject& event,
                                    const QString& ownedUnitId = {});
    static QJsonObject snapshotFor(const SimulationEngine& engine, const QString& role,
                                   quint64 stateRevision, const QJsonObject& roomState,
                                   const QSet<QString>& explicitlyShared = {},
                                   const QString& ownedUnitId = {},
                                   const QJsonObject& observerTrajectories = {});
};

} // namespace gbr
