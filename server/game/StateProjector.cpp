#include "StateProjector.h"

#include "core/Scenario.h"
#include "core/SimulationEngine.h"
#include "core/UnitBase.h"
#include "protocol/Protocol.h"

#include <QHash>

#include <algorithm>

namespace gbr {

namespace {

bool hasFullVisibility(const QString& role) {
    return role == QLatin1String("director") || role == QLatin1String("editor");
}

QJsonObject observedEnemyRuntime(const QJsonObject& source) {
    QJsonObject projected;
    for (const QString& field : {QStringLiteral("id"), QStringLiteral("callsign"),
                                 QStringLiteral("kind"), QStringLiteral("side"),
                                 QStringLiteral("movable"), QStringLiteral("position"),
                                 QStringLiteral("maxHp"), QStringLiteral("hp"),
                                 QStringLiteral("alive")}) {
        if (source.contains(field)) projected.insert(field, source.value(field));
    }
    projected[QStringLiteral("status")] = QStringLiteral("已探测");
    return projected;
}

} // namespace

QString StateProjector::sideForRole(const QString& role) {
    if (role == QLatin1String("red") || role == QLatin1String("blue")) return role;
    return {};
}

bool StateProjector::canEditSide(const QString& role, const QString& side) {
    if (side != QLatin1String("red") && side != QLatin1String("blue")) return false;
    return role == QLatin1String("editor") || role == side;
}

bool StateProjector::canControlSide(const QString& role, const QString& side) {
    if (side != QLatin1String("red") && side != QLatin1String("blue")) return false;
    return role == QLatin1String("director") || role == side;
}

QSet<QString> StateProjector::visibleUnitIds(const SimulationEngine& engine,
                                             const QString& role) {
    QSet<QString> ids;
    if (hasFullVisibility(role)) {
        for (const QString& id : engine.unitIds()) ids.insert(id);
        return ids;
    }
    const QString side = sideForRole(role);
    if (side.isEmpty()) return ids;
    for (const QString& id : engine.unitIds()) {
        UnitBase* unit = engine.unit(id);
        if (!unit || unit->sideStr() != side) continue;
        ids.insert(id);
        const QJsonObject snapshot = engine.unitSnapshot(id);
        for (const QJsonValue& value : snapshot.value(QStringLiteral("detections")).toArray()) {
            const QString targetId = value.toObject().value(QStringLiteral("id")).toString();
            if (!targetId.isEmpty() && engine.unit(targetId)) ids.insert(targetId);
        }
        const QJsonObject knowledge = snapshot.value(QStringLiteral("sharedKnowledge")).toObject();
        for (auto it = knowledge.constBegin(); it != knowledge.constEnd(); ++it) {
            const QString targetId = it.value().toObject()
                                         .value(QStringLiteral("targetId")).toString();
            if (!targetId.isEmpty() && engine.unit(targetId)) ids.insert(targetId);
        }
    }
    return ids;
}

QJsonArray StateProjector::filteredMessages(const SimulationEngine& engine,
                                            const QString& role) {
    QJsonArray output;
    const QString side = sideForRole(role);
    if (side.isEmpty() && !hasFullVisibility(role)) return output;
    for (const QVariant& value : engine.recentMessages()) {
        const QJsonObject message = QJsonObject::fromVariantMap(value.toMap());
        if (side.isEmpty() || message.value(QStringLiteral("senderSide")).toString() == side
            || message.value(QStringLiteral("receiverSide")).toString() == side) {
            output.append(message);
        }
    }
    return output;
}

QJsonObject StateProjector::projectEvent(const SimulationEngine& engine, const QString& role,
                                         const QJsonObject& event) {
    if (hasFullVisibility(role)) return event;
    const QString side = sideForRole(role);
    if (side.isEmpty()) return {};
    const QSet<QString> visibleIds = visibleUnitIds(engine, role);
    for (const QString& field : {QStringLiteral("unitId"),
                                 QStringLiteral("sourceUnitId")}) {
        const QString id = event.value(field).toString();
        if (!id.isEmpty() && engine.unit(id) && !visibleIds.contains(id)) return {};
    }

    QJsonObject projected = event;
    for (const QString& id : engine.unitIds()) {
        UnitBase* unit = engine.unit(id);
        if (!unit || unit->sideStr() == side || visibleIds.contains(id)) continue;
        const QString callsign = unit->callsign();
        for (auto it = projected.begin(); it != projected.end(); ++it) {
            if (!it.value().isString()) continue;
            QString value = it.value().toString();
            value.replace(id, QStringLiteral("未知单元"));
            if (!callsign.isEmpty()) value.replace(callsign, QStringLiteral("未知单元"));
            it.value() = value;
        }
    }
    return projected;
}

QJsonObject StateProjector::snapshotFor(const SimulationEngine& engine, const QString& role,
                                        quint64 stateRevision,
                                        const QJsonObject& roomState) {
    const QSet<QString> visibleIds = visibleUnitIds(engine, role);
    const QString side = sideForRole(role);
    const bool fullVisibility = hasFullVisibility(role);

    QHash<QString, QJsonObject> runtimeById;
    for (const QJsonValue& value : engine.collectAllUnitsSnapshot()) {
        const QJsonObject runtime = value.toObject();
        const QString id = runtime.value(QStringLiteral("id")).toString();
        if (!visibleIds.contains(id)) continue;
        const bool friendly = runtime.value(QStringLiteral("side")).toString() == side;
        runtimeById.insert(id, fullVisibility || friendly
                                  ? runtime : observedEnemyRuntime(runtime));
    }

    Scenario filtered = engine.scenario();
    std::erase_if(filtered.units, [&visibleIds](const ScenarioUnit& unit) {
        return !visibleIds.contains(unit.id);
    });
    if (!fullVisibility) {
        filtered.notes.clear();
        for (ScenarioUnit& unit : filtered.units) {
            if (unit.side == side) continue;
            const QJsonObject runtime = runtimeById.value(unit.id);
            const QJsonArray position = runtime.value(QStringLiteral("position")).toArray();
            if (position.size() >= 2) {
                unit.pos.x = position.at(0).toDouble();
                unit.pos.y = position.at(1).toDouble();
                if (position.size() >= 3) unit.pos.alt = position.at(2).toDouble();
            }
            unit.detectRange = 0.0;
            unit.attackRange = 0.0;
            unit.commRange = 0.0;
            unit.speed = 0.0;
            unit.maxHp = runtime.value(QStringLiteral("maxHp")).toDouble(100.0);
            unit.attackPower = 0.0;
            unit.schedule.clear();
        }
    }

    QStringList runtimeIds = runtimeById.keys();
    runtimeIds.sort();
    QJsonArray runtime;
    for (const QString& id : runtimeIds) runtime.append(runtimeById.value(id));
    return QJsonObject{{QStringLiteral("schemaVersion"), Protocol::SchemaVersion},
                       {QStringLiteral("stateRevision"), static_cast<qint64>(stateRevision)},
                       {QStringLiteral("scenario"), ScenarioIo::toJson(filtered)},
                       {QStringLiteral("units"), runtime},
                       {QStringLiteral("messages"), filteredMessages(engine, role)},
                       {QStringLiteral("roomState"), roomState}};
}

} // namespace gbr
