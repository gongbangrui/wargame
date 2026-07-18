#include "VisibleStateProjector.h"

#include "core/SimulationEngine.h"
#include "core/Scenario.h"

#include <QJsonValue>
#include <cmath>

namespace gbr {

QSet<QString> VisibleStateProjector::visibleEnemyIds(const SimulationEngine& engine,
                                                     const QString& friendlySide) {
    return visibleEnemyIds(engine.collectAllUnitsSnapshot(), friendlySide);
}

QSet<QString> VisibleStateProjector::visibleEnemyIds(const QJsonArray& units,
                                                     const QString& friendlySide) {
    QSet<QString> visible;
    for (const auto& friendlyValue : units) {
        const QJsonObject friendly = friendlyValue.toObject();
        if (friendly.value(QStringLiteral("side")).toString() != friendlySide
            || !friendly.value(QStringLiteral("alive")).toBool()) continue;
        const QJsonArray friendlyPosition = friendly.value(QStringLiteral("position")).toArray();
        if (friendlyPosition.size() < 2) continue;
        const double detectRange = friendly.value(QStringLiteral("detectRange")).toDouble();
        for (const auto& enemyValue : units) {
            const QJsonObject enemy = enemyValue.toObject();
            if (enemy.value(QStringLiteral("side")).toString() == friendlySide
                || !enemy.value(QStringLiteral("alive")).toBool()) continue;
            const QJsonArray enemyPosition = enemy.value(QStringLiteral("position")).toArray();
            if (enemyPosition.size() < 2) continue;
            const double dx = friendlyPosition.at(0).toDouble() - enemyPosition.at(0).toDouble();
            const double dy = friendlyPosition.at(1).toDouble() - enemyPosition.at(1).toDouble();
            if (std::sqrt(dx * dx + dy * dy) <= detectRange) {
                visible.insert(enemy.value(QStringLiteral("id")).toString());
            }
        }
        const QJsonObject knowledge = friendly.value(QStringLiteral("sharedKnowledge")).toObject();
        for (auto it = knowledge.constBegin(); it != knowledge.constEnd(); ++it) {
            if (!it.key().startsWith(QLatin1String("shared:detect:"))) continue;
            const QString id = it.value().toObject().value(QStringLiteral("targetId")).toString();
            if (!id.isEmpty()) visible.insert(id);
        }
    }
    return visible;
}

QJsonObject VisibleStateProjector::sanitizeDetectedEnemy(const QJsonObject& unit) {
    QJsonObject result;
    static const QStringList allowed{
        QStringLiteral("id"), QStringLiteral("callsign"), QStringLiteral("kind"),
        QStringLiteral("side"), QStringLiteral("position"), QStringLiteral("alive"),
        QStringLiteral("status"),
    };
    for (const QString& key : allowed) {
        if (unit.contains(key)) result.insert(key, unit.value(key));
    }
    result.insert(QStringLiteral("detected"), true);
    return result;
}

QJsonObject VisibleStateProjector::sanitizeFriendlyUnit(const QJsonObject& unit) {
    QJsonObject result;
    static const QStringList allowed{
        QStringLiteral("id"), QStringLiteral("callsign"), QStringLiteral("kind"),
        QStringLiteral("side"), QStringLiteral("movable"), QStringLiteral("position"),
        QStringLiteral("detectRange"), QStringLiteral("attackRange"),
        QStringLiteral("commRange"), QStringLiteral("speed"), QStringLiteral("maxHp"),
        QStringLiteral("attackPower"), QStringLiteral("hp"), QStringLiteral("alive"),
        QStringLiteral("status"), QStringLiteral("schedule"),
        QStringLiteral("jammer"), QStringLiteral("jamFactor"),
    };
    for (const QString& key : allowed) {
        if (unit.contains(key)) result.insert(key, unit.value(key));
    }
    return result;
}

QJsonArray VisibleStateProjector::projectMessages(const SimulationEngine& engine,
                                                  const SessionIdentity& identity,
                                                  const QSet<QString>& visibleIds) {
    QJsonArray result;
    if (identity.role == QLatin1String(SessionRole::Observer)) return result;
    const bool full = identity.role == QLatin1String(SessionRole::Director)
        || identity.role == QLatin1String(SessionRole::Editor);
    for (const QVariant& value : engine.recentMessages()) {
        QJsonObject message = QJsonObject::fromVariantMap(value.toMap());
        if (!full) {
            const QString senderSide = message.value(QStringLiteral("senderSide")).toString();
            const QString receiverSide = message.value(QStringLiteral("receiverSide")).toString();
            if (senderSide != identity.side && receiverSide != identity.side) continue;
            const QJsonObject payload = message.value(QStringLiteral("payload")).toObject();
            const QString targetId = payload.value(QStringLiteral("targetId")).toString();
            if (!targetId.isEmpty() && !visibleIds.contains(targetId)) continue;
        }
        result.append(message);
    }
    return result;
}

QJsonObject VisibleStateProjector::project(const SimulationEngine& engine,
                                           const SessionIdentity& identity,
                                           qint64 scenarioRevision,
                                           qint64 serverTick,
                                           qint64 lastSequence) {
    const bool full = identity.role == QLatin1String(SessionRole::Director)
        || identity.role == QLatin1String(SessionRole::Editor);
    const bool sideView = identity.role == QLatin1String(SessionRole::Red)
        || identity.role == QLatin1String(SessionRole::Blue);
    const QJsonArray units = engine.collectAllUnitsSnapshot();
    const QSet<QString> enemyIds = sideView
        ? visibleEnemyIds(units, identity.side) : QSet<QString>{};
    QSet<QString> visibleIds = enemyIds;
    QJsonArray projectedUnits;
    for (const auto& value : units) {
        const QJsonObject unit = value.toObject();
        const QString id = unit.value(QStringLiteral("id")).toString();
        const QString side = unit.value(QStringLiteral("side")).toString();
        if (full) {
            projectedUnits.append(unit);
            visibleIds.insert(id);
        } else if (sideView && side == identity.side) {
            projectedUnits.append(sanitizeFriendlyUnit(unit));
            visibleIds.insert(id);
        } else if (sideView && enemyIds.contains(id)) {
            projectedUnits.append(sanitizeDetectedEnemy(unit));
        }
    }
    QJsonObject result{
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("scenarioRevision"), scenarioRevision},
        {QStringLiteral("serverTick"), serverTick},
        {QStringLiteral("lastSequence"), lastSequence},
        {QStringLiteral("simTime"), engine.simTime()},
        {QStringLiteral("running"), engine.running()},
        {QStringLiteral("readyForSim"), engine.readyForSim()},
        {QStringLiteral("cpIssues"), full ? engine.cpIssues() : QString()},
        {QStringLiteral("map"), engine.mapInfo()},
        {QStringLiteral("units"), projectedUnits},
        {QStringLiteral("messages"), projectMessages(engine, identity, visibleIds)},
    };
    if (full) result.insert(QStringLiteral("scenario"), ScenarioIo::toJson(engine.scenario()));
    return result;
}

} // namespace gbr
