#include "ClientStateStore.h"

#include <QSet>
#include <cmath>

namespace gbr {

namespace {

bool finiteNumber(const QJsonValue& value) {
    return value.isDouble() && std::isfinite(value.toDouble());
}

bool validUnit(const QJsonValue& value, QString* id = nullptr) {
    if (!value.isObject()) return false;
    const QJsonObject unit = value.toObject();
    const QString unitId = unit.value(QStringLiteral("id")).toString();
    const QJsonArray position = unit.value(QStringLiteral("position")).toArray();
    if (!unit.value(QStringLiteral("id")).isString()
        || unitId.isEmpty() || unitId.size() > 128
        || !unit.value(QStringLiteral("position")).isArray()
        || (position.size() != 2 && position.size() != 3)) {
        return false;
    }
    for (const auto& coordinate : position) {
        if (!finiteNumber(coordinate)) return false;
    }
    if (id) *id = unitId;
    return true;
}

} // namespace

ClientStateStore::ClientStateStore(QObject* parent) : QObject(parent) {}

void ClientStateStore::setIdentity(const QString& role, const QString& side) {
    if (m_role == role && m_side == side) return;
    m_role = role;
    m_side = side;
    emit identityChanged();
}

void ClientStateStore::setError(const QString& error) {
    if (m_lastError == error) return;
    m_lastError = error;
    emit errorChanged(error);
}

void ClientStateStore::rebuildIndex() {
    m_unitsById.clear();
    for (const auto& value : m_units) {
        const QJsonObject unit = value.toObject();
        const QString id = unit.value(QStringLiteral("id")).toString();
        if (!id.isEmpty()) m_unitsById.insert(id, unit);
    }
}

bool ClientStateStore::applySnapshot(const QJsonObject& snapshot, QString* error) {
    if (error) error->clear();
    const int schemaVersion = snapshot.value(QStringLiteral("schemaVersion")).toInt(-1);
    const qint64 revision = snapshot.value(QStringLiteral("scenarioRevision")).toInteger(-1);
    const qint64 tick = snapshot.value(QStringLiteral("serverTick")).toInteger(-1);
    const qint64 sequence = snapshot.value(QStringLiteral("lastSequence")).toInteger(-1);
    const double simTime = snapshot.value(QStringLiteral("simTime")).toDouble(-1.0);
    if (schemaVersion != 1 || revision < 1 || tick < 0 || sequence < 1
        || !std::isfinite(simTime) || simTime < 0.0
        || !snapshot.value(QStringLiteral("running")).isBool()
        || !snapshot.value(QStringLiteral("readyForSim")).isBool()
        || !snapshot.value(QStringLiteral("cpIssues")).isString()
        || !snapshot.value(QStringLiteral("map")).isObject()
        || !snapshot.value(QStringLiteral("units")).isArray()
        || !snapshot.value(QStringLiteral("messages")).isArray()
        || (snapshot.contains(QStringLiteral("lobby"))
            && !snapshot.value(QStringLiteral("lobby")).isObject())
        || (snapshot.contains(QStringLiteral("chatMessages"))
            && !snapshot.value(QStringLiteral("chatMessages")).isArray())
        || (snapshot.contains(QStringLiteral("scenario"))
            && !snapshot.value(QStringLiteral("scenario")).isObject())) {
        if (error) *error = QStringLiteral("服务器快照结构无效");
        return false;
    }
    QSet<QString> ids;
    for (const auto& value : snapshot.value(QStringLiteral("units")).toArray()) {
        QString id;
        if (!validUnit(value, &id) || ids.contains(id)) {
            if (error) *error = QStringLiteral("服务器快照包含无效或重复单元");
            return false;
        }
        ids.insert(id);
    }
    const bool timeChanged = m_simTime != simTime;
    const bool didRunningChange = m_running != snapshot.value(QStringLiteral("running")).toBool();
    const bool readyChanged = m_readyForSim != snapshot.value(QStringLiteral("readyForSim")).toBool()
        || m_cpIssues != snapshot.value(QStringLiteral("cpIssues")).toString();
    const bool didMapChange = m_map != snapshot.value(QStringLiteral("map")).toObject();
    const bool didLobbyChange = m_lobby != snapshot.value(QStringLiteral("lobby")).toObject();
    const bool didChatChange = m_chatMessages != snapshot.value(QStringLiteral("chatMessages")).toArray();
    m_simTime = simTime;
    m_running = snapshot.value(QStringLiteral("running")).toBool();
    m_readyForSim = snapshot.value(QStringLiteral("readyForSim")).toBool();
    m_cpIssues = snapshot.value(QStringLiteral("cpIssues")).toString();
    m_map = snapshot.value(QStringLiteral("map")).toObject();
    m_units = snapshot.value(QStringLiteral("units")).toArray();
    m_messages = snapshot.value(QStringLiteral("messages")).toArray();
    m_lobby = snapshot.value(QStringLiteral("lobby")).toObject();
    m_chatMessages = snapshot.value(QStringLiteral("chatMessages")).toArray();
    m_scenario = snapshot.value(QStringLiteral("scenario")).toObject();
    m_scenarioRevision = revision;
    m_serverTick = tick;
    m_lastSequence = sequence;
    rebuildIndex();
    if (timeChanged) emit simTimeChanged();
    if (didRunningChange) emit runningChanged();
    if (readyChanged) emit readyForSimChanged();
    if (didMapChange) emit mapChanged();
    if (didLobbyChange) emit lobbyChanged();
    if (didChatChange) emit chatMessagesChanged();
    emit unitsChanged();
    emit messagesChanged();
    return true;
}

bool ClientStateStore::applyDelta(const QJsonObject& delta, QString* error) {
    if (error) error->clear();
    const qint64 sequence = delta.value(QStringLiteral("sequence")).toInteger(-1);
    const qint64 serverTick = delta.value(QStringLiteral("serverTick")).toInteger(-1);
    const double simTime = delta.value(QStringLiteral("simTime")).toDouble(-1.0);
    if (sequence != m_lastSequence + 1
        || delta.value(QStringLiteral("scenarioRevision")).toInteger(-1) != m_scenarioRevision) {
        if (error) *error = QStringLiteral("增量序号或场景版本不连续");
        return false;
    }
    if (delta.value(QStringLiteral("schemaVersion")).toInt(-1) != 1
        || serverTick < m_serverTick
        || !std::isfinite(simTime) || simTime < 0.0
        || !delta.value(QStringLiteral("running")).isBool()
        || !delta.value(QStringLiteral("readyForSim")).isBool()
        || !delta.value(QStringLiteral("cpIssues")).isString()
        || (delta.contains(QStringLiteral("messages"))
            && !delta.value(QStringLiteral("messages")).isArray())
        || !delta.value(QStringLiteral("upsertUnits")).isArray()
        || (delta.contains(QStringLiteral("removeFields"))
            && !delta.value(QStringLiteral("removeFields")).isArray())
        || !delta.value(QStringLiteral("removeUnitIds")).isArray()
        || (delta.contains(QStringLiteral("map"))
            && !delta.value(QStringLiteral("map")).isObject())
        || (delta.contains(QStringLiteral("lobby"))
            && !delta.value(QStringLiteral("lobby")).isObject())
        || (delta.contains(QStringLiteral("chatMessages"))
            && !delta.value(QStringLiteral("chatMessages")).isArray())) {
        if (error) *error = QStringLiteral("服务器增量结构无效");
        return false;
    }
    QHash<QString, QJsonObject> next = m_unitsById;
    QSet<QString> modifiedUnitIds;
    QSet<QString> removedUnitIds;
    for (const auto& value : delta.value(QStringLiteral("upsertUnits")).toArray()) {
        const QJsonObject patch = value.toObject();
        const QString id = patch.value(QStringLiteral("id")).toString();
        if (!value.isObject() || id.isEmpty() || id.size() > 128 || modifiedUnitIds.contains(id)) {
            if (error) *error = QStringLiteral("增量包含无效单元");
            return false;
        }
        modifiedUnitIds.insert(id);
        QJsonObject merged = next.value(id);
        for (auto field = patch.constBegin(); field != patch.constEnd(); ++field) {
            merged.insert(field.key(), field.value());
        }
        next.insert(id, std::move(merged));
    }
    for (const auto& value : delta.value(QStringLiteral("removeFields")).toArray()) {
        const QJsonObject removal = value.toObject();
        const QString id = removal.value(QStringLiteral("id")).toString();
        const QJsonArray fields = removal.value(QStringLiteral("fields")).toArray();
        if (!value.isObject() || removal.size() != 2 || id.isEmpty() || id.size() > 128
            || !removal.value(QStringLiteral("fields")).isArray() || fields.isEmpty()
            || !next.contains(id) || removedUnitIds.contains(id)) {
            if (error) *error = QStringLiteral("增量字段删除项无效");
            return false;
        }
        QSet<QString> seenFields;
        QJsonObject merged = next.value(id);
        for (const auto& fieldValue : fields) {
            const QString field = fieldValue.toString();
            if (!fieldValue.isString() || field.isEmpty() || field == QLatin1String("id")
                || seenFields.contains(field)) {
                if (error) *error = QStringLiteral("增量字段删除项无效");
                return false;
            }
            seenFields.insert(field);
            merged.remove(field);
        }
        modifiedUnitIds.insert(id);
        next.insert(id, std::move(merged));
    }
    for (const auto& value : delta.value(QStringLiteral("removeUnitIds")).toArray()) {
        const QString id = value.toString();
        if (!value.isString() || id.isEmpty() || id.size() > 128
            || modifiedUnitIds.contains(id) || removedUnitIds.contains(id)) {
            if (error) *error = QStringLiteral("增量包含无效移除项");
            return false;
        }
        removedUnitIds.insert(id);
        next.remove(id);
    }
    for (const QString& id : modifiedUnitIds) {
        QString mergedId;
        if (!validUnit(next.value(id), &mergedId) || mergedId != id) {
            if (error) *error = QStringLiteral("增量单元补丁不完整或无效");
            return false;
        }
    }
    QStringList ids = next.keys();
    ids.sort();
    QJsonArray units;
    for (const QString& id : ids) units.append(next.value(id));
    const bool timeChanged = m_simTime != simTime;
    const bool didRunningChange = m_running != delta.value(QStringLiteral("running")).toBool();
    const bool readyChanged = m_readyForSim != delta.value(QStringLiteral("readyForSim")).toBool()
        || m_cpIssues != delta.value(QStringLiteral("cpIssues")).toString();
    const bool didMapChange = delta.contains(QStringLiteral("map"))
        && m_map != delta.value(QStringLiteral("map")).toObject();
    const bool didUnitsChange = !delta.value(QStringLiteral("upsertUnits")).toArray().isEmpty()
        || !delta.value(QStringLiteral("removeFields")).toArray().isEmpty()
        || !delta.value(QStringLiteral("removeUnitIds")).toArray().isEmpty();
    const bool didMessagesChange = delta.contains(QStringLiteral("messages"))
        && m_messages != delta.value(QStringLiteral("messages")).toArray();
    const bool didLobbyChange = delta.contains(QStringLiteral("lobby"))
        && m_lobby != delta.value(QStringLiteral("lobby")).toObject();
    const bool didChatChange = delta.contains(QStringLiteral("chatMessages"))
        && m_chatMessages != delta.value(QStringLiteral("chatMessages")).toArray();
    m_units = std::move(units);
    m_unitsById = std::move(next);
    m_lastSequence = sequence;
    m_serverTick = serverTick;
    m_simTime = simTime;
    m_running = delta.value(QStringLiteral("running")).toBool();
    m_readyForSim = delta.value(QStringLiteral("readyForSim")).toBool();
    m_cpIssues = delta.value(QStringLiteral("cpIssues")).toString();
    if (delta.contains(QStringLiteral("messages"))) {
        m_messages = delta.value(QStringLiteral("messages")).toArray();
    }
    if (delta.contains(QStringLiteral("map"))) {
        m_map = delta.value(QStringLiteral("map")).toObject();
    }
    if (delta.contains(QStringLiteral("lobby"))) {
        m_lobby = delta.value(QStringLiteral("lobby")).toObject();
    }
    if (delta.contains(QStringLiteral("chatMessages"))) {
        m_chatMessages = delta.value(QStringLiteral("chatMessages")).toArray();
    }
    if (timeChanged) emit simTimeChanged();
    if (didRunningChange) emit runningChanged();
    if (readyChanged) emit readyForSimChanged();
    if (didMapChange) emit mapChanged();
    if (didLobbyChange) emit lobbyChanged();
    if (didChatChange) emit chatMessagesChanged();
    if (didUnitsChange) emit unitsChanged();
    if (didMessagesChange) emit messagesChanged();
    return true;
}

bool ClientStateStore::advanceSequence(qint64 sequence, QString* error) {
    if (error) error->clear();
    if (sequence != m_lastSequence + 1) {
        if (error) *error = QStringLiteral("协议消息序号不连续");
        return false;
    }
    m_lastSequence = sequence;
    return true;
}

void ClientStateStore::resetSequence() {
    m_lastSequence = 0;
}

void ClientStateStore::clear() {
    m_simTime = 0.0;
    m_running = false;
    m_readyForSim = false;
    m_cpIssues.clear();
    m_lastError.clear();
    m_map = {};
    m_units = {};
    m_messages = {};
    m_lobby = {};
    m_chatMessages = {};
    m_scenario = {};
    m_unitsById.clear();
    m_scenarioRevision = 0;
    m_serverTick = 0;
    m_lastSequence = 0;
    emit simTimeChanged();
    emit runningChanged();
    emit readyForSimChanged();
    emit mapChanged();
    emit unitsChanged();
    emit messagesChanged();
    emit lobbyChanged();
    emit chatMessagesChanged();
}

} // namespace gbr
