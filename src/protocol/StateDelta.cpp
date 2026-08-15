#include "StateDelta.h"

#include "Protocol.h"
#include "IntelProtocol.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonValue>
#include <QSet>
#include <QStringList>

namespace gbr::StateDelta {

namespace {

QHash<QString, QJsonObject> unitsById(const QJsonArray& units) {
    QHash<QString, QJsonObject> result;
    for (const QJsonValue& value : units) {
        const QJsonObject unit = value.toObject();
        const QString id = unit.value(QStringLiteral("id")).toString();
        if (!id.isEmpty()) result.insert(id, unit);
    }
    return result;
}

QSet<QString> unitIds(const QJsonArray& units) {
    QSet<QString> result;
    for (const QJsonValue& value : units) {
        const QString id = value.toObject().value(QStringLiteral("id")).toString();
        if (!id.isEmpty()) result.insert(id);
    }
    return result;
}

QJsonArray sortedStringArray(const QSet<QString>& values) {
    QStringList ids = values.values();
    ids.sort();
    QJsonArray result;
    for (const QString& id : ids) result.append(id);
    return result;
}

QSet<QString> stringSet(const QJsonValue& value) {
    QSet<QString> result;
    if (!value.isArray()) return result;
    for (const QJsonValue& item : value.toArray()) result.insert(item.toString());
    return result;
}

QJsonObject trajectoryForUnit(const QJsonArray& trails, const QString& unitId) {
    for (const QJsonValue& value : trails) {
        const QJsonObject trail = value.toObject();
        if (trail.value(QStringLiteral("unitId")).toString() == unitId) return trail;
    }
    return {};
}

QJsonArray trajectoryPoints(const QJsonObject& trail) {
    return trail.value(QStringLiteral("points")).toArray();
}

bool trajectoryPointPrefix(const QJsonArray& prefix, const QJsonArray& full) {
    if (prefix.size() > full.size()) return false;
    for (qsizetype i = 0; i < prefix.size(); ++i) {
        if (prefix.at(i) != full.at(i)) return false;
    }
    return true;
}

QJsonObject makeTrajectoryDelta(const QJsonObject& base, const QJsonObject& current) {
    const QJsonObject baseTrails = base.value(QStringLiteral("observerTrajectories")).toObject();
    const QJsonObject currentTrails = current.value(QStringLiteral("observerTrajectories")).toObject();
    const QSet<QString> baseSelected = stringSet(baseTrails.value(QStringLiteral("selectedUnitIds")));
    const QSet<QString> currentSelected = stringSet(currentTrails.value(QStringLiteral("selectedUnitIds")));
    QJsonArray updates;
    const QJsonArray baseArray = baseTrails.value(QStringLiteral("trails")).toArray();
    const QJsonArray currentArray = currentTrails.value(QStringLiteral("trails")).toArray();
    QStringList ids = currentSelected.values();
    ids.sort();
    const bool resetSelection = baseTrails.isEmpty() || baseSelected != currentSelected;
    for (const QString& id : ids) {
        const QJsonObject before = trajectoryForUnit(baseArray, id);
        const QJsonObject after = trajectoryForUnit(currentArray, id);
        const QJsonArray beforePoints = trajectoryPoints(before);
        const QJsonArray afterPoints = trajectoryPoints(after);
        if (resetSelection) {
            updates.append(QJsonObject{{QStringLiteral("unitId"), id},
                                       {QStringLiteral("reset"), true},
                                       {QStringLiteral("points"), afterPoints}});
            continue;
        }
        if (after.isEmpty()) {
            updates.append(QJsonObject{{QStringLiteral("unitId"), id},
                                       {QStringLiteral("reset"), true},
                                       {QStringLiteral("points"), QJsonArray{}}});
            continue;
        }
        if (before == after) continue;
        QJsonObject update{{QStringLiteral("unitId"), id},
                           {QStringLiteral("reset"), false},
                           {QStringLiteral("points"), afterPoints}};
        if (trajectoryPointPrefix(beforePoints, afterPoints)) {
            QJsonArray appended;
            for (qsizetype i = beforePoints.size(); i < afterPoints.size(); ++i) {
                appended.append(afterPoints.at(i));
            }
            update[QStringLiteral("points")] = appended;
        } else {
            update[QStringLiteral("reset")] = true;
        }
        if (!afterPoints.isEmpty()) {
            update[QStringLiteral("trimBefore")] =
                afterPoints.first().toObject().value(
                    afterPoints.first().toObject().contains(QStringLiteral("time"))
                        ? QStringLiteral("time") : QStringLiteral("simTime"));
        }
        updates.append(update);
    }
    return QJsonObject{{QStringLiteral("selectedUnitIds"), sortedStringArray(currentSelected)},
                       {QStringLiteral("updates"), updates}};
}

bool mergeTrajectoryDelta(QJsonObject& candidate, const QJsonObject& delta, QString* error) {
    const QJsonObject incoming = delta.value(QStringLiteral("observerTrajectoryDelta")).toObject();
    if (incoming.isEmpty()) return true;
    QSet<QString> selected = stringSet(incoming.value(QStringLiteral("selectedUnitIds")));
    QJsonObject existing = candidate.value(QStringLiteral("observerTrajectories")).toObject();
    QHash<QString, QJsonArray> trails;
    for (const QJsonValue& value : existing.value(QStringLiteral("trails")).toArray()) {
        const QJsonObject trail = value.toObject();
        trails.insert(trail.value(QStringLiteral("unitId")).toString(),
                      trail.value(QStringLiteral("points")).toArray());
    }
    for (auto it = trails.begin(); it != trails.end();) {
        if (!selected.contains(it.key())) it = trails.erase(it);
        else ++it;
    }
    for (const QJsonValue& value : incoming.value(QStringLiteral("updates")).toArray()) {
        const QJsonObject update = value.toObject();
        const QString id = update.value(QStringLiteral("unitId")).toString();
        if (!selected.contains(id)) {
            if (error) *error = QStringLiteral("轨迹增量包含未选择单位");
            return false;
        }
        QJsonArray points = update.value(QStringLiteral("points")).toArray();
        if (update.value(QStringLiteral("reset")).toBool(false)) {
            trails[id] = points;
        } else {
            QJsonArray merged = trails.value(id);
            for (const QJsonValue& point : points) merged.append(point);
            trails[id] = merged;
        }
        if (update.contains(QStringLiteral("trimBefore"))) {
            const double trimBefore = update.value(QStringLiteral("trimBefore")).toDouble();
            QJsonArray trimmed;
            for (const QJsonValue& point : trails.value(id)) {
                const QJsonObject object = point.toObject();
                const QString key = object.contains(QStringLiteral("time"))
                    ? QStringLiteral("time") : QStringLiteral("simTime");
                if (object.value(key).toDouble() >= trimBefore) trimmed.append(point);
            }
            trails[id] = trimmed;
        }
        while (trails.value(id).size() > Protocol::MaxObserverTrajectoryPoints) {
            QJsonArray trimmed = trails.value(id);
            trimmed.removeFirst();
            trails[id] = trimmed;
        }
    }
    QJsonArray output;
    QStringList ids = trails.keys();
    ids.sort();
    for (const QString& id : ids) {
        output.append(QJsonObject{{QStringLiteral("unitId"), id},
                                  {QStringLiteral("points"), trails.value(id)}});
    }
    candidate[QStringLiteral("observerTrajectories")] =
        QJsonObject{{QStringLiteral("selectedUnitIds"), sortedStringArray(selected)},
                    {QStringLiteral("trails"), output}};
    return true;
}

} // namespace

bool canCreate(const QJsonObject& base, const QJsonObject& current) {
    if (base.isEmpty() || current.isEmpty()) return false;
    const int schemaVersion = base.value(QStringLiteral("schemaVersion")).toInt();
    if (!Protocol::isSupportedWireVersion(schemaVersion, schemaVersion)
        || current.value(QStringLiteral("schemaVersion")).toInt() != schemaVersion) {
        return false;
    }
    if (base.value(QStringLiteral("scenario")) != current.value(QStringLiteral("scenario"))) {
        return false;
    }
    if (!Protocol::validateSnapshotState(base, schemaVersion).valid
        || !Protocol::validateSnapshotState(current, schemaVersion).valid) {
        return false;
    }
    if (current.value(QStringLiteral("stateRevision")).toInteger()
        <= base.value(QStringLiteral("stateRevision")).toInteger()) {
        return false;
    }
    return unitIds(base.value(QStringLiteral("units")).toArray())
        == unitIds(current.value(QStringLiteral("units")).toArray());
}

QJsonObject create(const QJsonObject& base, const QJsonObject& current) {
    if (!canCreate(base, current)) return {};
    const int schemaVersion = base.value(QStringLiteral("schemaVersion")).toInt();
    const auto previousUnits = unitsById(base.value(QStringLiteral("units")).toArray());
    const auto currentUnits = unitsById(current.value(QStringLiteral("units")).toArray());
    QJsonArray changedUnits;
    QSet<QString> changedIds;
    for (auto it = currentUnits.cbegin(); it != currentUnits.cend(); ++it) {
        if (!previousUnits.contains(it.key()) || previousUnits.value(it.key()) != it.value()) {
            changedUnits.append(it.value());
            changedIds.insert(it.key());
        }
    }

    QJsonObject delta{{QStringLiteral("schemaVersion"), schemaVersion},
                      {QStringLiteral("baseStateRevision"),
                       base.value(QStringLiteral("stateRevision"))},
                      {QStringLiteral("stateRevision"),
                       current.value(QStringLiteral("stateRevision"))},
                      {QStringLiteral("scenarioRevision"),
                       current.value(QStringLiteral("roomState")).toObject()
                           .value(QStringLiteral("scenarioRevision"))},
                      {QStringLiteral("units"), changedUnits},
                      {QStringLiteral("changedUnitIds"), sortedStringArray(changedIds)},
                      {QStringLiteral("roomState"), current.value(QStringLiteral("roomState"))}};
    if (base.value(QStringLiteral("messages")) != current.value(QStringLiteral("messages"))) {
        delta[QStringLiteral("messages")] = current.value(QStringLiteral("messages"));
    }
    if (base.value(QStringLiteral("mapMarks")) != current.value(QStringLiteral("mapMarks"))) {
        delta[QStringLiteral("mapMarks")] = current.value(QStringLiteral("mapMarks"));
    }
    if (base.value(QStringLiteral("projectiles"))
        != current.value(QStringLiteral("projectiles"))) {
        delta[QStringLiteral("projectiles")] = current.value(QStringLiteral("projectiles"));
    }
    if (base.value(QStringLiteral("observerTrajectories"))
        != current.value(QStringLiteral("observerTrajectories"))) {
        const QJsonObject trajectoryDelta = makeTrajectoryDelta(base, current);
        delta[QStringLiteral("observerTrajectoryDelta")] = trajectoryDelta;
    }
    if (schemaVersion >= Protocol::IntelSchemaVersion
        && base.value(QStringLiteral("intelState"))
               != current.value(QStringLiteral("intelState"))) {
        Protocol::IntelState before;
        Protocol::IntelState after;
        if (!Protocol::fromJson(base.value(QStringLiteral("intelState")).toObject(), &before).valid
            || !Protocol::fromJson(current.value(QStringLiteral("intelState")).toObject(), &after).valid) {
            return {};
        }
        const QJsonObject intelDelta = Protocol::makeIntelDelta(before, after);
        if (intelDelta.isEmpty()) return {};
        delta[QStringLiteral("intelDelta")] = intelDelta;
    }
    return delta;
}

bool apply(QJsonObject& state, const QJsonObject& delta, QString* error) {
    if (error) error->clear();
    auto fail = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };
    if (state.isEmpty()) return fail(QStringLiteral("尚未收到完整快照"));
    const int schemaVersion = state.value(QStringLiteral("schemaVersion")).toInt();
    if (!Protocol::isSupportedWireVersion(schemaVersion, schemaVersion)
        || delta.value(QStringLiteral("schemaVersion")).toInt() != schemaVersion) {
        return fail(QStringLiteral("增量结构版本不兼容"));
    }
    const Protocol::ValidationResult deltaValidation =
        Protocol::validateServerPayloadForVersion(QStringLiteral("delta"), delta, schemaVersion);
    if (!deltaValidation.valid) {
        return fail(deltaValidation.message.isEmpty()
                        ? QStringLiteral("增量结构无效") : deltaValidation.message);
    }
    if (schemaVersion == Protocol::LegacySchemaVersion
        && delta.contains(QStringLiteral("intelDelta"))) {
        return fail(QStringLiteral("旧版增量不能包含情报字段"));
    }
    const qint64 expected = state.value(QStringLiteral("stateRevision")).toInteger();
    const qint64 base = delta.value(QStringLiteral("baseStateRevision")).toInteger();
    const qint64 next = delta.value(QStringLiteral("stateRevision")).toInteger();
    if (base != expected || next <= base) {
        return fail(QStringLiteral("增量状态版本不连续"));
    }
    const qint64 currentScenarioRevision = state.value(QStringLiteral("roomState")).toObject()
        .value(QStringLiteral("scenarioRevision")).toInteger();
    if (delta.value(QStringLiteral("scenarioRevision")).toInteger()
        != currentScenarioRevision) {
        return fail(QStringLiteral("场景版本已变化，需要完整同步"));
    }

    if (!delta.value(QStringLiteral("units")).isArray()
        || !delta.value(QStringLiteral("roomState")).isObject()) {
        return fail(QStringLiteral("增量结构无效"));
    }

    QJsonObject candidate = state;
    QHash<QString, QJsonObject> units = unitsById(
        candidate.value(QStringLiteral("units")).toArray());
    QSet<QString> actualChangedIds;
    for (const QJsonValue& value : delta.value(QStringLiteral("units")).toArray()) {
        const QJsonObject unit = value.toObject();
        const QString id = unit.value(QStringLiteral("id")).toString();
        if (id.isEmpty() || !units.contains(id)) {
            return fail(QStringLiteral("增量包含未知单元"));
        }
        actualChangedIds.insert(id);
        units[id] = unit;
    }
    if (delta.contains(QStringLiteral("changedUnitIds"))) {
        const QSet<QString> declared = stringSet(delta.value(QStringLiteral("changedUnitIds")));
        if (declared != actualChangedIds) {
            return fail(QStringLiteral("增量变化单元列表与内容不一致"));
        }
    }
    QStringList ids = units.keys();
    ids.sort();
    QJsonArray merged;
    for (const QString& id : ids) merged.append(units.value(id));
    candidate[QStringLiteral("units")] = merged;
    candidate[QStringLiteral("roomState")] = delta.value(QStringLiteral("roomState"));
    if (delta.contains(QStringLiteral("messages"))) {
        candidate[QStringLiteral("messages")] = delta.value(QStringLiteral("messages"));
    }
    if (delta.contains(QStringLiteral("mapMarks"))) {
        candidate[QStringLiteral("mapMarks")] = delta.value(QStringLiteral("mapMarks"));
    }
    if (delta.contains(QStringLiteral("projectiles"))) {
        candidate[QStringLiteral("projectiles")] = delta.value(QStringLiteral("projectiles"));
    }
    if (delta.contains(QStringLiteral("observerTrajectories"))) {
        candidate[QStringLiteral("observerTrajectories")]
            = delta.value(QStringLiteral("observerTrajectories"));
    }
    if (delta.contains(QStringLiteral("observerTrajectoryDelta"))
        && !mergeTrajectoryDelta(candidate, delta, error)) {
        return false;
    }
    if (schemaVersion >= Protocol::IntelSchemaVersion && delta.contains(QStringLiteral("intelDelta"))) {
        Protocol::IntelState intel;
        if (!Protocol::fromJson(candidate.value(QStringLiteral("intelState")).toObject(), &intel).valid) {
            return fail(QStringLiteral("增量缺少有效情报基线"));
        }
        const Protocol::ValidationResult intelResult = Protocol::applyIntelDelta(
            &intel, delta.value(QStringLiteral("intelDelta")).toObject());
        if (!intelResult.valid) return fail(intelResult.message);
        candidate[QStringLiteral("intelState")] = Protocol::toJson(intel);
    }
    candidate[QStringLiteral("stateRevision")] = next;

    const Protocol::ValidationResult validation = Protocol::validateSnapshotState(
        candidate, schemaVersion);
    if (!validation.valid) {
        return fail(validation.message.isEmpty()
                        ? QStringLiteral("增量合并后状态无效")
                        : validation.message);
    }
    state = candidate;
    return true;
}

} // namespace gbr::StateDelta
