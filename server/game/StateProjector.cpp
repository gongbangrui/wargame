#include "StateProjector.h"

#include "core/Scenario.h"
#include "core/SimulationEngine.h"
#include "core/UnitBase.h"
#include "protocol/Protocol.h"

#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QQueue>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>

namespace gbr {

namespace {

bool hasFullVisibility(const QString& role) {
    // Kept for replay/import compatibility. The network server never assigns
    // these legacy values to a client account.
    return role == QLatin1String("director") || role == QLatin1String("editor");
}

bool isObserver(const QString& role) {
    return role == QLatin1String("observer");
}

bool isSeat(const QString& role) {
    return role.contains(QLatin1String("_commander"))
        || role.contains(QLatin1String("_attack"))
        || role.contains(QLatin1String("_recon"))
        || role.contains(QLatin1String("_ground"))
        || role.contains(QLatin1String("_jammer"));
}

QStringList sortedUnitIds(const SimulationEngine& engine) {
    QStringList ids = engine.unitIds();
    std::sort(ids.begin(), ids.end());
    return ids;
}

bool withinDirectedRange(const UnitBase* sender, const UnitBase* recipient) {
    if (!sender || !recipient || !sender->alive() || !recipient->alive()) return false;
    if (sender->side() != recipient->side()) return false;
    const double dx = sender->pos().x - recipient->pos().x;
    const double dy = sender->pos().y - recipient->pos().y;
    return std::hypot(dx, dy) <= std::max(0.0, sender->commRange());
}

struct ReachabilityCache {
    quint64 stateRevision = 0;
    quint64 topologyFingerprint = 0;
    QHash<QString, QSet<QString>> reachableBySource;
};

QHash<quintptr, QHash<QString, ReachabilityCache>> reachabilityCaches;
QList<quintptr> reachabilityEngineOrder;
QMutex reachabilityCacheMutex;
quint64 reachabilityGraphBuilds = 0;
quint64 reachabilityBfsTraversals = 0;
constexpr qsizetype kMaxCachedEngines = 16;

quint64 mixFingerprint(quint64 value, quint64 component) {
    value ^= component + 0x9e3779b97f4a7c15ULL + (value << 6) + (value >> 2);
    return value;
}

quint64 doubleBits(double value) {
    return std::bit_cast<quint64>(value);
}

quint64 topologyFingerprint(const SimulationEngine& engine) {
    quint64 fingerprint = 0xcbf29ce484222325ULL;
    QStringList ids = engine.unitIds();
    std::sort(ids.begin(), ids.end());
    for (const QString& id : ids) {
        const UnitBase* unit = engine.unit(id);
        fingerprint = mixFingerprint(fingerprint, qHash(id));
        if (!unit) continue;
        fingerprint = mixFingerprint(fingerprint, qHash(unit->sideStr()));
        fingerprint = mixFingerprint(fingerprint, unit->alive() ? 1 : 0);
        fingerprint = mixFingerprint(fingerprint, doubleBits(unit->pos().x));
        fingerprint = mixFingerprint(fingerprint, doubleBits(unit->pos().y));
        fingerprint = mixFingerprint(fingerprint, doubleBits(unit->pos().alt));
        fingerprint = mixFingerprint(fingerprint, doubleBits(unit->commRange()));
    }
    return fingerprint;
}

ReachabilityCache buildReachabilityCache(const SimulationEngine& engine,
                                         const QString& side, quint64 stateRevision,
                                         quint64 fingerprint) {
    ReachabilityCache cache;
    cache.stateRevision = stateRevision;
    cache.topologyFingerprint = fingerprint;
    const QStringList ids = sortedUnitIds(engine);
    ++reachabilityGraphBuilds;
    for (const QString& sourceId : ids) {
        const UnitBase* source = engine.unit(sourceId);
        if (!source || source->sideStr() != side) continue;
        QSet<QString> reachable{sourceId};
        QQueue<QString> pending;
        pending.enqueue(sourceId);
        while (!pending.isEmpty()) {
            const QString currentId = pending.dequeue();
            const UnitBase* current = engine.unit(currentId);
            if (!current) continue;
            for (const QString& candidateId : ids) {
                if (reachable.contains(candidateId)) continue;
                const UnitBase* candidate = engine.unit(candidateId);
                if (!candidate || candidate->sideStr() != side
                    || !withinDirectedRange(current, candidate)) continue;
                reachable.insert(candidateId);
                pending.enqueue(candidateId);
            }
        }
        ++reachabilityBfsTraversals;
        cache.reachableBySource.insert(sourceId, std::move(reachable));
    }
    return cache;
}

bool directedReachable(const SimulationEngine& engine, const QString& senderId,
                       const QString& recipientId, quint64 stateRevision = 0,
                       const QString& projectionKey = {}) {
    if (senderId.isEmpty() || recipientId.isEmpty()) return false;
    if (senderId == recipientId) return true;
    const UnitBase* sender = engine.unit(senderId);
    const UnitBase* recipient = engine.unit(recipientId);
    if (!sender || !recipient || sender->side() != recipient->side()) return false;
    const quint64 fingerprint = topologyFingerprint(engine);
    const quintptr engineKey = reinterpret_cast<quintptr>(&engine);
    const QString cacheKey = sender->sideStr() + QLatin1Char('\x1f') + projectionKey;
    QMutexLocker locker(&reachabilityCacheMutex);
    reachabilityEngineOrder.removeAll(engineKey);
    reachabilityEngineOrder.append(engineKey);
    while (reachabilityEngineOrder.size() > kMaxCachedEngines) {
        reachabilityCaches.remove(reachabilityEngineOrder.takeFirst());
    }
    auto& perEngine = reachabilityCaches[engineKey];
    auto it = perEngine.find(cacheKey);
    if (it == perEngine.end() || it->stateRevision != stateRevision
        || it->topologyFingerprint != fingerprint) {
        it = perEngine.insert(cacheKey,
                              buildReachabilityCache(engine, sender->sideStr(),
                                                     stateRevision, fingerprint));
    }
    return it->reachableBySource.value(senderId).contains(recipientId);
}

QString projectionCacheKey(const QString& role, const QString& ownedUnitId) {
    return role + QLatin1Char('\x1e') + ownedUnitId;
}

QString seatSide(const QString& seat) {
    if (seat.startsWith(QLatin1String("red_"))) return QStringLiteral("red");
    if (seat.startsWith(QLatin1String("blue_"))) return QStringLiteral("blue");
    return {};
}

QString seatKind(const QString& seat) {
    if (seat.contains(QLatin1String("commander"))) return QStringLiteral("commandpost");
    if (seat.contains(QLatin1String("attack"))) return QStringLiteral("attackuav");
    if (seat.contains(QLatin1String("recon"))) return QStringLiteral("reconuav");
    if (seat.contains(QLatin1String("ground"))) return QStringLiteral("groundscout");
    if (seat.contains(QLatin1String("jammer"))) return QStringLiteral("jammeruav");
    return {};
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

QJsonObject observerRuntime(const QJsonObject& source) {
    static const QStringList fields{
        QStringLiteral("id"), QStringLiteral("callsign"), QStringLiteral("kind"),
        QStringLiteral("side"), QStringLiteral("movable"), QStringLiteral("position"),
        QStringLiteral("detectRange"), QStringLiteral("attackRange"),
        QStringLiteral("commRange"), QStringLiteral("speed"), QStringLiteral("baseSpeed"),
        QStringLiteral("maxHp"), QStringLiteral("attackPower"), QStringLiteral("armor"),
        QStringLiteral("hp"), QStringLiteral("alive"), QStringLiteral("subsystems"),
        QStringLiteral("serviceRequested"), QStringLiteral("serviceProgress"),
        QStringLiteral("ammoRemaining"), QStringLiteral("ammoCapacity"),
        QStringLiteral("cooldownRemaining"), QStringLiteral("cooldownSec"),
        QStringLiteral("fuelRemaining"), QStringLiteral("fuelCapacity"),
        QStringLiteral("turnaroundProgress")};
    QJsonObject projected;
    for (const QString& field : fields) {
        if (source.contains(field)) projected.insert(field, source.value(field));
    }
    return projected;
}

QJsonObject observerScenario(const SimulationEngine& engine) {
    const Scenario& source = engine.scenario();
    const QJsonObject map{{QStringLiteral("name"), source.map.name},
                          {QStringLiteral("widthMeters"), source.map.widthMeters},
                          {QStringLiteral("heightMeters"), source.map.heightMeters},
                          {QStringLiteral("backgroundResource"),
                           source.map.backgroundResource}};
    QList<const ScenarioUnit*> sortedUnits;
    sortedUnits.reserve(static_cast<qsizetype>(source.units.size()));
    for (const ScenarioUnit& unit : source.units) sortedUnits.append(&unit);
    std::sort(sortedUnits.begin(), sortedUnits.end(), [](const ScenarioUnit* left,
                                                         const ScenarioUnit* right) {
        return left->id < right->id;
    });
    QJsonArray units;
    for (const ScenarioUnit* unit : sortedUnits) {
        units.append(QJsonObject{
            {QStringLiteral("id"), unit->id},
            {QStringLiteral("callsign"), unit->callsign},
            {QStringLiteral("kind"), unit->kind},
            {QStringLiteral("side"), unit->side},
            {QStringLiteral("x"), unit->pos.x},
            {QStringLiteral("y"), unit->pos.y},
            {QStringLiteral("alt"), unit->pos.alt},
            {QStringLiteral("detectRange"), unit->detectRange},
            {QStringLiteral("attackRange"), unit->attackRange},
            {QStringLiteral("commRange"), unit->commRange},
            {QStringLiteral("speed"), unit->speed},
            {QStringLiteral("maxHp"), unit->maxHp},
            {QStringLiteral("attackPower"), unit->attackPower},
            {QStringLiteral("armor"), unit->armor},
            {QStringLiteral("ammoCapacity"), unit->ammoCapacity},
            {QStringLiteral("initialAmmo"), unit->initialAmmo},
            {QStringLiteral("cooldownSec"), unit->cooldownSec},
            {QStringLiteral("fuelCapacitySec"), unit->fuelCapacitySec},
            {QStringLiteral("initialFuelSec"), unit->initialFuelSec}});
    }
    return QJsonObject{{QStringLiteral("schemaVersion"), ScenarioIo::SchemaVersion},
                       {QStringLiteral("map"), map},
                       {QStringLiteral("units"), units}};
}

QJsonObject observerRoomState(const QJsonObject& source, quint64 stateRevision) {
    static const QStringList fields{
        QStringLiteral("phase"), QStringLiteral("roomId"), QStringLiteral("roomName"),
        QStringLiteral("roomStatus"), QStringLiteral("roomMode"),
        QStringLiteral("aiDifficulty"), QStringLiteral("aiEngine"),
        QStringLiteral("configVersion"),
        QStringLiteral("running"), QStringLiteral("simTime"), QStringLiteral("speed"),
        QStringLiteral("scenarioRevision")};
    QJsonObject projected;
    for (const QString& field : fields) {
        if (source.contains(field)) projected.insert(field, source.value(field));
    }
    projected[QStringLiteral("observer")] = true;
    projected[QStringLiteral("stateRevision")] = static_cast<qint64>(stateRevision);
    return projected;
}

QJsonObject copyEventFields(const QJsonObject& event, const QStringList& fields) {
    QJsonObject projected{{QStringLiteral("kind"), event.value(QStringLiteral("kind"))}};
    for (const QString& field : fields) {
        if (event.contains(field)) projected.insert(field, event.value(field));
    }
    return projected;
}

QJsonObject observerEvent(const QJsonObject& event) {
    const QString kind = event.value(QStringLiteral("kind")).toString();
    if (kind == QLatin1String("roomClosed") || kind == QLatin1String("matchReset")
        || kind == QLatin1String("matchStarted") || kind == QLatin1String("matchEndedByAdmin")) {
        return copyEventFields(event, {QStringLiteral("message")});
    }
    if (kind == QLatin1String("simulationEnded") || kind == QLatin1String("forfeit")) {
        return copyEventFields(event, {QStringLiteral("winner"), QStringLiteral("loser"),
                                       QStringLiteral("message")});
    }
    if (kind == QLatin1String("targetDestroyed")) {
        return copyEventFields(event, {QStringLiteral("unitId"), QStringLiteral("x"),
                                       QStringLiteral("y")});
    }
    return {};
}

} // namespace

QString StateProjector::sideForRole(const QString& role) {
    if (role == QLatin1String("red") || role == QLatin1String("blue")) return role;
    return seatSide(role);
}

bool StateProjector::canTransmit(const SimulationEngine& engine, const QString& senderUnitId,
                                 const QString& recipientUnitId) {
    return directedReachable(engine, senderUnitId, recipientUnitId);
}

void StateProjector::resetReachabilityCacheStats() {
    QMutexLocker locker(&reachabilityCacheMutex);
    reachabilityCaches.clear();
    reachabilityEngineOrder.clear();
    reachabilityGraphBuilds = 0;
    reachabilityBfsTraversals = 0;
}

quint64 StateProjector::reachabilityGraphBuildCount() {
    QMutexLocker locker(&reachabilityCacheMutex);
    return reachabilityGraphBuilds;
}

quint64 StateProjector::reachabilityBfsTraversalCount() {
    QMutexLocker locker(&reachabilityCacheMutex);
    return reachabilityBfsTraversals;
}

bool StateProjector::canEditSide(const QString& role, const QString& side) {
    if (side != QLatin1String("red") && side != QLatin1String("blue")) return false;
    if (role == QLatin1String("editor") || role == side) return true;
    // Initial scenario and room configuration are controlled by the web
    // administrator. A commander can deploy units, but cannot mutate the
    // authoritative roster from a client seat.
    return false;
}

bool StateProjector::canControlSide(const QString& role, const QString& side) {
    if (side != QLatin1String("red") && side != QLatin1String("blue")) return false;
    return role == QLatin1String("director") || role == side
        || (isSeat(role) && seatSide(role) == side);
}

namespace {

QSet<QString> friendlyVisibleUnitIdsImpl(const SimulationEngine& engine, const QString& role,
                                         const QString& ownedUnitId, quint64 stateRevision) {
    QSet<QString> ids;
    if (hasFullVisibility(role)) {
        for (const QString& id : engine.unitIds()) ids.insert(id);
        return ids;
    }
    const QString side = StateProjector::sideForRole(role);
    if (side.isEmpty()) return ids;
    const bool commander = role.contains(QLatin1String("_commander"));
    const QString ownedKind = seatKind(role);
    int ownedIndex = -1;
    const int underscore = role.lastIndexOf(QLatin1Char('_'));
    if (underscore >= 0) ownedIndex = role.mid(underscore + 1).toInt() - 1;
    const QStringList allIds = sortedUnitIds(engine);
    if (!isSeat(role)) {
        // Legacy faction snapshots are retained only for replay/import tests.
        // New network sessions always use an explicit seat id.
        for (const QString& id : allIds) {
            UnitBase* unit = engine.unit(id);
            if (!unit || unit->sideStr() != side) continue;
            ids.insert(id);
        }
        return ids;
    }
    QString ownedId = ownedUnitId;
    UnitBase* authoritativeOwned = engine.unit(ownedId);
    if (!ownedId.isEmpty() && (!authoritativeOwned || authoritativeOwned->sideStr() != side)) {
        return ids;
    }
    if (ownedId.isEmpty()) {
        int matchingIndex = 0;
        for (const QString& id : allIds) {
            UnitBase* unit = engine.unit(id);
            if (!unit || unit->sideStr() != side) continue;
            if (commander) {
                if (unit->kind() == UnitKind::CommandPost) { ownedId = id; break; }
            } else if (unit->kindStr() == ownedKind && matchingIndex++ == ownedIndex) {
                ownedId = id;
                break;
            }
        }
    }
    if (ownedId.isEmpty()) return ids;

    ids.insert(ownedId);
    const QString cacheKey = projectionCacheKey(role, ownedId);
    if (commander) {
        // 指挥官可沿着有向通信链查看友方单位，而不是直接读取全图。
        for (const QString& id : allIds) {
            UnitBase* unit = engine.unit(id);
            if (unit && unit->sideStr() == side
                && directedReachable(engine, ownedId, id, stateRevision, cacheKey)) {
                ids.insert(id);
            }
        }
    } else {
        for (const QString& id : allIds) {
            UnitBase* unit = engine.unit(id);
            if (!unit || unit->sideStr() != side) continue;
            if (directedReachable(engine, ownedId, id, stateRevision, cacheKey)
                && directedReachable(engine, id, ownedId, stateRevision, cacheKey)) {
                ids.insert(id);
            }
        }
    }
    return ids;
}

QSet<QString> visibleUnitIdsImpl(const SimulationEngine& engine, const QString& role,
                                 const QString& ownedUnitId, quint64 stateRevision) {
    QSet<QString> ids = friendlyVisibleUnitIdsImpl(engine, role, ownedUnitId, stateRevision);
    if (hasFullVisibility(role)) return ids;
    const QString side = StateProjector::sideForRole(role);
    if (side.isEmpty()) return ids;
    const QSet<QString> friendlyVisible = ids;
    // Only friendly units in the current communication view may contribute
    // detections and received intelligence to the visual state.
    for (const QString& id : friendlyVisible) {
        const QJsonObject snapshot = engine.unitSnapshot(id);
        for (const QJsonValue& value : snapshot.value(QStringLiteral("detections")).toArray()) {
            const QString targetId = value.toObject().value(QStringLiteral("id")).toString();
            if (!targetId.isEmpty() && engine.unit(targetId)) ids.insert(targetId);
        }
        const QJsonObject knowledge = snapshot.value(QStringLiteral("sharedKnowledge")).toObject();
        for (auto it = knowledge.constBegin(); it != knowledge.constEnd(); ++it) {
            const QString targetId = it.value().toObject().value(QStringLiteral("targetId")).toString();
            if (!targetId.isEmpty() && engine.unit(targetId)) ids.insert(targetId);
        }
    }
    return ids;
}

} // namespace

QSet<QString> StateProjector::visibleUnitIds(const SimulationEngine& engine,
                                             const QString& role) {
    return visibleUnitIds(engine, role, QString{});
}

QSet<QString> StateProjector::visibleUnitIds(const SimulationEngine& engine,
                                             const QString& role,
                                             const QString& ownedUnitId) {
    return visibleUnitIdsImpl(engine, role, ownedUnitId, 0);
}

QSet<QString> StateProjector::visibleUnitIds(const SimulationEngine& engine,
                                             const QString& role,
                                             const QSet<QString>& explicitlyShared,
                                             const QString& ownedUnitId) {
    QSet<QString> ids = visibleUnitIds(engine, role, ownedUnitId);
    for (const QString& id : explicitlyShared) {
        if (engine.unit(id)) ids.insert(id);
    }
    return ids;
}

namespace {

QJsonArray filteredMessagesImpl(const SimulationEngine& engine, const QString& role,
                                const QString& ownedUnitId, quint64 stateRevision) {
    QJsonArray output;
    const QString side = StateProjector::sideForRole(role);
    if (side.isEmpty() && !hasFullVisibility(role)) return output;
    const bool seat = isSeat(role);
    const QSet<QString> communicationVisible = seat
        ? friendlyVisibleUnitIdsImpl(engine, role, ownedUnitId, stateRevision)
        : QSet<QString>{};
    for (const QVariant& value : engine.recentMessages()) {
        const QJsonObject message = QJsonObject::fromVariantMap(value.toMap());
        if (seat) {
            const QString sender = message.value(QStringLiteral("sender")).toString();
            const QString receiver = message.value(QStringLiteral("receiver")).toString();
            if (communicationVisible.contains(sender)
                || communicationVisible.contains(receiver)) output.append(message);
        } else if (side.isEmpty() || message.value(QStringLiteral("senderSide")).toString() == side
                   || message.value(QStringLiteral("receiverSide")).toString() == side) {
            output.append(message);
        }
    }
    return output;
}

QJsonValue redactHiddenIdentities(
    const QJsonValue& value,
    const QList<QPair<QString, QString>>& hiddenIdentities) {
    if (value.isString()) {
        QString text = value.toString();
        for (const auto& [id, callsign] : hiddenIdentities) {
            text.replace(id, QStringLiteral("未知单元"));
            if (!callsign.isEmpty()) text.replace(callsign, QStringLiteral("未知单元"));
        }
        return text;
    }
    if (value.isArray()) {
        QJsonArray projected;
        for (const QJsonValue& item : value.toArray()) {
            projected.append(redactHiddenIdentities(item, hiddenIdentities));
        }
        return projected;
    }
    if (value.isObject()) {
        QJsonObject projected = value.toObject();
        for (auto it = projected.begin(); it != projected.end(); ++it) {
            it.value() = redactHiddenIdentities(it.value(), hiddenIdentities);
        }
        return projected;
    }
    return value;
}

} // namespace

QJsonArray StateProjector::filteredMessages(const SimulationEngine& engine,
                                            const QString& role,
                                            const QString& ownedUnitId) {
    if (isObserver(role)) return {};
    return filteredMessagesImpl(engine, role, ownedUnitId, 0);
}

QJsonObject StateProjector::projectEvent(const SimulationEngine& engine, const QString& role,
                                         const QJsonObject& event,
                                         const QString& ownedUnitId) {
    if (isObserver(role)) return observerEvent(event);
    const QString kind = event.value(QStringLiteral("kind")).toString();
    if (kind == QLatin1String("roomClosed") || kind == QLatin1String("matchReset")) {
        return event;
    }
    if (hasFullVisibility(role)) return event;
    const QString side = sideForRole(role);
    if (side.isEmpty()) return {};
    const QSet<QString> visibleIds = visibleUnitIds(engine, role, ownedUnitId);
    const QJsonObject identityCatalog = engine.unitIdentityCatalog();
    for (const QString& field : {QStringLiteral("unitId"),
                                 QStringLiteral("sourceUnitId")}) {
        const QString id = event.value(field).toString();
        const QJsonObject identity = identityCatalog.value(id).toObject();
        if (!id.isEmpty() && identity.value(QStringLiteral("side")).toString() != side
            && !identity.isEmpty() && !visibleIds.contains(id)) return {};
    }

    QList<QPair<QString, QString>> hiddenIdentities;
    for (auto it = identityCatalog.constBegin(); it != identityCatalog.constEnd(); ++it) {
        const QJsonObject identity = it.value().toObject();
        if (identity.value(QStringLiteral("side")).toString() == side
            || visibleIds.contains(it.key())) continue;
        hiddenIdentities.append(qMakePair(
            it.key(), identity.value(QStringLiteral("callsign")).toString()));
    }
    std::sort(hiddenIdentities.begin(), hiddenIdentities.end(), [](const auto& left,
                                                                  const auto& right) {
        if (left.first.size() != right.first.size()) {
            return left.first.size() > right.first.size();
        }
        return left.first < right.first;
    });
    return redactHiddenIdentities(event, hiddenIdentities).toObject();
}

QJsonObject StateProjector::snapshotFor(const SimulationEngine& engine, const QString& role,
                                        quint64 stateRevision,
                                        const QJsonObject& roomState,
                                        const QSet<QString>& explicitlyShared,
                                        const QString& ownedUnitId) {
    if (isObserver(role)) {
        QStringList ids = sortedUnitIds(engine);
        QJsonArray runtime;
        for (const QString& id : ids) {
            const QJsonObject projected = observerRuntime(engine.unitSnapshot(id));
            if (!projected.isEmpty()) runtime.append(projected);
        }
        return QJsonObject{{QStringLiteral("schemaVersion"), Protocol::SchemaVersion},
                           {QStringLiteral("stateRevision"),
                            static_cast<qint64>(stateRevision)},
                           {QStringLiteral("scenario"), observerScenario(engine)},
                           {QStringLiteral("units"), runtime},
                           {QStringLiteral("roomState"),
                            observerRoomState(roomState, stateRevision)}};
    }
    QSet<QString> visibleIds = visibleUnitIdsImpl(engine, role, ownedUnitId, stateRevision);
    for (const QString& id : explicitlyShared) {
        if (engine.unit(id)) visibleIds.insert(id);
    }
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
            unit.armor = 0.0;
            unit.repairRate = 0.0;
            unit.subsystemRepairRate = 0.0;
            unit.ammoCapacity = 0;
            unit.initialAmmo = 0;
            unit.hitProbability = 0.0;
            unit.optimalRange = 0.0;
            unit.minAttackRange = 0.0;
            unit.cooldownSec = 0.0;
            unit.damageMin = 0.0;
            unit.damageMax = 0.0;
            unit.rangeFalloff = 0.0;
            unit.fuelCapacitySec = 1.0;
            unit.initialFuelSec = 0.0;
            unit.rearmDurationSec = 0.0;
            unit.schedule.clear();
        }
    }

    QStringList runtimeIds = runtimeById.keys();
    runtimeIds.sort();
    QJsonArray runtime;
    for (const QString& id : runtimeIds) runtime.append(runtimeById.value(id));
    QJsonObject projectedRoomState = roomState;
    if (isSeat(role) && !role.contains(QLatin1String("_commander"))) {
        QString subordinateUnitId = ownedUnitId;
        if (subordinateUnitId.isEmpty()) {
            const QString kind = seatKind(role);
            int matchingIndex = 0;
            const int separator = role.lastIndexOf(QLatin1Char('_'));
            const int requestedIndex = separator >= 0 ? role.mid(separator + 1).toInt() - 1 : -1;
            for (const QString& id : sortedUnitIds(engine)) {
                const UnitBase* unit = engine.unit(id);
                if (!unit || unit->sideStr() != side || unit->kindStr() != kind) continue;
                if (matchingIndex++ == requestedIndex) {
                    subordinateUnitId = id;
                    break;
                }
            }
        }
        QString commanderUnitId;
        for (const QString& id : sortedUnitIds(engine)) {
            const UnitBase* unit = engine.unit(id);
            if (unit && unit->sideStr() == side && unit->kind() == UnitKind::CommandPost) {
                commanderUnitId = id;
                break;
            }
        }
        const QString cacheKey = projectionCacheKey(role, subordinateUnitId);
        const bool receives = directedReachable(engine, commanderUnitId, subordinateUnitId,
                                                stateRevision, cacheKey);
        const bool transmits = directedReachable(engine, subordinateUnitId, commanderUnitId,
                                                 stateRevision, cacheKey);
        projectedRoomState[QStringLiteral("communicationState")] = receives && transmits
            ? QStringLiteral("bilateral")
            : receives ? QStringLiteral("receiveOnly") : QStringLiteral("disconnected");
    }
    return QJsonObject{{QStringLiteral("schemaVersion"), Protocol::SchemaVersion},
                       {QStringLiteral("stateRevision"), static_cast<qint64>(stateRevision)},
                       {QStringLiteral("scenario"), ScenarioIo::toJson(filtered)},
                       {QStringLiteral("units"), runtime},
                       {QStringLiteral("messages"),
                        filteredMessagesImpl(engine, role, ownedUnitId, stateRevision)},
                       {QStringLiteral("roomState"), projectedRoomState}};
}

} // namespace gbr
