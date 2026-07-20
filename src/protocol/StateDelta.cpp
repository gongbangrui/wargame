#include "StateDelta.h"

#include "Protocol.h"

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

} // namespace

bool canCreate(const QJsonObject& base, const QJsonObject& current) {
    if (base.isEmpty() || current.isEmpty()) return false;
    if (base.value(QStringLiteral("schemaVersion")).toInt() != Protocol::SchemaVersion
        || current.value(QStringLiteral("schemaVersion")).toInt() != Protocol::SchemaVersion) {
        return false;
    }
    if (base.value(QStringLiteral("scenario")) != current.value(QStringLiteral("scenario"))) {
        return false;
    }
    return unitIds(base.value(QStringLiteral("units")).toArray())
        == unitIds(current.value(QStringLiteral("units")).toArray());
}

QJsonObject create(const QJsonObject& base, const QJsonObject& current) {
    if (!canCreate(base, current)) return {};
    const auto previousUnits = unitsById(base.value(QStringLiteral("units")).toArray());
    const auto currentUnits = unitsById(current.value(QStringLiteral("units")).toArray());
    QJsonArray changedUnits;
    for (auto it = currentUnits.cbegin(); it != currentUnits.cend(); ++it) {
        if (!previousUnits.contains(it.key()) || previousUnits.value(it.key()) != it.value()) {
            changedUnits.append(it.value());
        }
    }

    QJsonObject delta{{QStringLiteral("schemaVersion"), Protocol::SchemaVersion},
                      {QStringLiteral("baseStateRevision"),
                       base.value(QStringLiteral("stateRevision"))},
                      {QStringLiteral("stateRevision"),
                       current.value(QStringLiteral("stateRevision"))},
                      {QStringLiteral("scenarioRevision"),
                       current.value(QStringLiteral("roomState")).toObject()
                           .value(QStringLiteral("scenarioRevision"))},
                      {QStringLiteral("units"), changedUnits},
                      {QStringLiteral("roomState"), current.value(QStringLiteral("roomState"))}};
    if (base.value(QStringLiteral("messages")) != current.value(QStringLiteral("messages"))) {
        delta[QStringLiteral("messages")] = current.value(QStringLiteral("messages"));
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
    if (delta.value(QStringLiteral("schemaVersion")).toInt() != Protocol::SchemaVersion) {
        return fail(QStringLiteral("增量结构版本不兼容"));
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

    QHash<QString, QJsonObject> units = unitsById(state.value(QStringLiteral("units")).toArray());
    for (const QJsonValue& value : delta.value(QStringLiteral("units")).toArray()) {
        const QJsonObject unit = value.toObject();
        const QString id = unit.value(QStringLiteral("id")).toString();
        if (id.isEmpty() || !units.contains(id)) {
            return fail(QStringLiteral("增量包含未知单元"));
        }
        units[id] = unit;
    }
    QStringList ids = units.keys();
    ids.sort();
    QJsonArray merged;
    for (const QString& id : ids) merged.append(units.value(id));
    state[QStringLiteral("units")] = merged;
    state[QStringLiteral("roomState")] = delta.value(QStringLiteral("roomState"));
    if (delta.contains(QStringLiteral("messages"))) {
        state[QStringLiteral("messages")] = delta.value(QStringLiteral("messages"));
    }
    state[QStringLiteral("stateRevision")] = next;
    return true;
}

} // namespace gbr::StateDelta
