#include "StateProjector.h"

#include "core/Scenario.h"
#include "core/SimulationEngine.h"
#include "core/UnitBase.h"
#include "protocol/Protocol.h"

#include <QCryptographicHash>
#include <QHash>
#include <QJsonArray>
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
    return role == QLatin1String("director") || role == QLatin1String("editor")
        || role == QLatin1String("room_admin");
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
    // Each graph stores reachability for every source on this side. A role or
    // owned-unit-specific cache key would rebuild the same graph once per
    // seat during every broadcast. Keep the parameter for call-site clarity;
    // it is not part of the graph's identity.
    Q_UNUSED(projectionKey);
    const QString cacheKey = sender->sideStr();
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

QString ownedUnitForRole(const SimulationEngine& engine, const QString& role,
                         const QString& authoritativeOwnedUnitId) {
    const QString side = StateProjector::sideForRole(role);
    UnitBase* authoritativeOwned = engine.unit(authoritativeOwnedUnitId);
    if (!authoritativeOwnedUnitId.isEmpty()) {
        return authoritativeOwned && authoritativeOwned->sideStr() == side
            ? authoritativeOwnedUnitId : QString{};
    }
    if (!isSeat(role)) return {};

    const bool commander = role.contains(QLatin1String("_commander"));
    const QString kind = seatKind(role);
    const int separator = role.lastIndexOf(QLatin1Char('_'));
    const int requestedIndex = separator >= 0 ? role.mid(separator + 1).toInt() - 1 : -1;
    int matchingIndex = 0;
    for (const QString& id : sortedUnitIds(engine)) {
        UnitBase* unit = engine.unit(id);
        if (!unit || unit->sideStr() != side) continue;
        if (commander && unit->kind() == UnitKind::CommandPost) return id;
        if (!commander && unit->kindStr() == kind && matchingIndex++ == requestedIndex) {
            return id;
        }
    }
    return {};
}

QString publicProjectileId(const QString& authoritativeId) {
    const QByteArray digest = QCryptographicHash::hash(
        QByteArrayLiteral("wargame-projectile-track:") + authoritativeId.toUtf8(),
        QCryptographicHash::Sha256).toHex();
    return QStringLiteral("track-%1").arg(QString::fromLatin1(digest.left(24)));
}

QJsonObject projectileForWire(const QJsonObject& source, bool exposeAuthoritativeId,
                              bool exposeAttacker, bool exposeTarget,
                              double mapWidth, double mapHeight) {
    static const QStringList fields{
        QStringLiteral("side"), QStringLiteral("headingRad"), QStringLiteral("speed"),
        QStringLiteral("age"), QStringLiteral("lifetime"), QStringLiteral("active"),
        QStringLiteral("terminalReason"), QStringLiteral("terminalAge"),
        QStringLiteral("resultSettled"), QStringLiteral("threatRadius")};
    QJsonObject projected;
    const QString authoritativeId = source.value(QStringLiteral("id")).toString();
    projected[QStringLiteral("id")] = exposeAuthoritativeId
        ? authoritativeId : publicProjectileId(authoritativeId);
    for (const QString& field : fields) {
        if (source.contains(field)) projected.insert(field, source.value(field));
    }

    QJsonArray position = source.value(QStringLiteral("position")).toArray();
    if (position.size() >= 2) {
        position[0] = std::clamp(position.at(0).toDouble(), 0.0, mapWidth);
        position[1] = std::clamp(position.at(1).toDouble(), 0.0, mapHeight);
    }
    projected[QStringLiteral("position")] = position;
    if (exposeAttacker && source.contains(QStringLiteral("attackerId"))) {
        projected[QStringLiteral("attackerId")] = source.value(QStringLiteral("attackerId"));
    }
    if (exposeTarget && source.contains(QStringLiteral("targetId"))) {
        projected[QStringLiteral("targetId")] = source.value(QStringLiteral("targetId"));
    }
    return projected;
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

QJsonObject actionCapability(bool enabled) {
    return QJsonObject{{QStringLiteral("visible"), true},
                       {QStringLiteral("enabled"), enabled}};
}

bool canProjectActions(const SimulationEngine& engine, const QString& role,
                       const QString& ownedUnitId, const UnitBase* unit,
                       quint64 stateRevision) {
    if (!unit || !unit->alive()) return false;
    if (role == QLatin1String("director")) return true;
    const QString side = StateProjector::sideForRole(role);
    if (side.isEmpty() || unit->sideStr() != side) return false;
    if (!isSeat(role)) return true;

    const QString resolvedOwned = ownedUnitForRole(engine, role, ownedUnitId);
    if (resolvedOwned.isEmpty()) return false;
    if (!role.contains(QLatin1String("_commander"))) {
        return unit->id() == resolvedOwned;
    }
    // The server grants commander control along the same directed link used
    // by command validation. A unit may be visible through received data while
    // still being non-controllable, so this check intentionally is stricter
    // than visibleUnitIds().
    return unit->id() == resolvedOwned
        || directedReachable(engine, resolvedOwned, unit->id(), stateRevision,
                             projectionCacheKey(role, resolvedOwned));
}

QJsonObject actionCapabilities(const SimulationEngine& engine, const QString& role,
                               const QString& ownedUnitId, const UnitBase* unit,
                               const QJsonObject& runtime, quint64 stateRevision) {
    QJsonObject actions;
    if (!canProjectActions(engine, role, ownedUnitId, unit, stateRevision)) return actions;

    const bool movable = runtime.value(QStringLiteral("movable")).toBool();
    const bool serviceRequested = runtime.value(QStringLiteral("serviceRequested")).toBool();
    const bool serviceEligible = runtime.value(QStringLiteral("serviceEligible")).toBool();
    const double fuel = runtime.value(QStringLiteral("fuelRemaining")).toDouble();
    const bool hasFuel = !movable || fuel > 1e-9;
    const auto ability = [&runtime](const QString& name) {
        return runtime.value(QStringLiteral("abilities")).toObject()
            .value(name).toObject();
    };
    const auto available = [&ability](const QString& name) {
        return ability(name).value(QStringLiteral("available")).toBool(false);
    };
    const QString kind = runtime.value(QStringLiteral("kind")).toString();
    const bool attack = kind == QLatin1String("attackuav");
    const bool hasAmmo = runtime.value(QStringLiteral("ammoRemaining")).toInt() > 0;
    const bool cooldownReady = runtime.value(QStringLiteral("cooldownRemaining")).toDouble()
        <= 1e-9;
    const bool weaponReady = hasAmmo && cooldownReady && !serviceRequested;

    if (movable) {
        actions.insert(QStringLiteral("moveTo"), actionCapability(hasFuel && !serviceRequested));
        actions.insert(QStringLiteral("withdraw"), actionCapability(hasFuel && !serviceRequested));
        actions.insert(QStringLiteral("setSpeed"), actionCapability(hasFuel && !serviceRequested));
        actions.insert(QStringLiteral("service"), actionCapability(serviceEligible));
        actions.insert(QStringLiteral("cancelService"), actionCapability(serviceRequested));
    }
    if (unit->countermeasureState().supported()) {
        actions.insert(QStringLiteral("activateCountermeasure"),
                       actionCapability(available(QStringLiteral("countermeasure"))));
    }
    if (unit->scanState().supported()) {
        actions.insert(QStringLiteral("activateScan"),
                       actionCapability(available(QStringLiteral("scan"))));
    }

    const QJsonObject subsystems = runtime.value(QStringLiteral("subsystems")).toObject();
    const bool damaged = subsystems.value(QStringLiteral("sensor")).toDouble(1.0) < 1.0 - 1e-9
        || subsystems.value(QStringLiteral("comms")).toDouble(1.0) < 1.0 - 1e-9
        || subsystems.value(QStringLiteral("mobility")).toDouble(1.0) < 1.0 - 1e-9
        || subsystems.value(QStringLiteral("weapon")).toDouble(1.0) < 1.0 - 1e-9;
    const QJsonObject repair = ability(QStringLiteral("fieldRepair"));
    const bool repairReady = damaged
        && repair.value(QStringLiteral("available")).toBool(false);
    actions.insert(QStringLiteral("attemptFieldRepair"), actionCapability(repairReady));

    if (attack) {
        actions.insert(QStringLiteral("engageTarget"), actionCapability(weaponReady));
        actions.insert(QStringLiteral("assignTarget"), actionCapability(weaponReady));
        actions.insert(QStringLiteral("attackAt"), actionCapability(weaponReady && hasFuel));
        actions.insert(QStringLiteral("setFlightPlan"), actionCapability(hasFuel));
        actions.insert(QStringLiteral("cancelEngagement"), actionCapability(true));
        actions.insert(QStringLiteral("setRoe"), actionCapability(true));
    }
    return actions;
}

QJsonObject observerRuntime(const QJsonObject& source) {
    static const QStringList fields{
        QStringLiteral("id"), QStringLiteral("callsign"), QStringLiteral("kind"),
        QStringLiteral("side"), QStringLiteral("movable"), QStringLiteral("position"),
        QStringLiteral("detectRange"), QStringLiteral("attackRange"),
        QStringLiteral("commRange"), QStringLiteral("speed"), QStringLiteral("baseSpeed"),
        QStringLiteral("maxCommandedSpeed"),
        QStringLiteral("maxHp"), QStringLiteral("attackPower"), QStringLiteral("armor"),
        QStringLiteral("hp"), QStringLiteral("alive"), QStringLiteral("subsystems"),
        QStringLiteral("serviceRequested"), QStringLiteral("serviceProgress"),
        QStringLiteral("ammoRemaining"), QStringLiteral("ammoCapacity"),
        QStringLiteral("cooldownRemaining"), QStringLiteral("cooldownSec"),
        QStringLiteral("activeProjectileCount"),
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

QJsonObject workflowProjection(const QJsonObject& source) {
    static const QStringList fields{
        QStringLiteral("taskId"), QStringLiteral("stage"), QStringLiteral("side"),
        QStringLiteral("reconId"), QStringLiteral("targetId"), QStringLiteral("attackerId"),
        QStringLiteral("guideId"), QStringLiteral("correlationId"),
        QStringLiteral("createdAt"), QStringLiteral("updatedAt")};
    QJsonObject projected;
    for (const QString& field : fields) {
        if (source.contains(field)) projected.insert(field, source.value(field));
    }
    return projected;
}

QJsonObject workflowMapProjection(const QJsonObject& source) {
    QJsonObject projected;
    for (const QString& side : {QStringLiteral("red"), QStringLiteral("blue")}) {
        const QJsonObject workflow = workflowProjection(source.value(side).toObject());
        if (!workflow.isEmpty()) projected.insert(side, workflow);
    }
    return projected;
}

QJsonObject observerRoomState(const QJsonObject& source, quint64 stateRevision) {
    static const QStringList fields{
        QStringLiteral("phase"), QStringLiteral("roomId"), QStringLiteral("roomName"),
        QStringLiteral("roomStatus"), QStringLiteral("roomMode"),
        QStringLiteral("aiDifficulty"), QStringLiteral("aiEngine"),
        QStringLiteral("configVersion"),
        QStringLiteral("running"), QStringLiteral("simTime"), QStringLiteral("speed"),
        QStringLiteral("scenarioRevision"), QStringLiteral("vmfWorkflows")};
    QJsonObject projected;
    for (const QString& field : fields) {
        if (source.contains(field)) projected.insert(field, source.value(field));
    }
    projected[QStringLiteral("observer")] = true;
    projected[QStringLiteral("stateRevision")] = static_cast<qint64>(stateRevision);
    if (source.contains(QStringLiteral("vmfWorkflows"))) {
        projected[QStringLiteral("vmfWorkflows")] = workflowMapProjection(
            source.value(QStringLiteral("vmfWorkflows")).toObject());
    }
    return projected;
}

QJsonObject copyEventFields(const QJsonObject& event, const QStringList& fields) {
    QJsonObject projected{{QStringLiteral("kind"), event.value(QStringLiteral("kind"))}};
    for (const QString& field : fields) {
        if (event.contains(field)) projected.insert(field, event.value(field));
    }
    return projected;
}

bool isVmfMessage(const QJsonObject& message) {
    return message.value(QStringLiteral("wireFormat")).toString()
               == QLatin1String("vmf-design-v1")
        || message.value(QStringLiteral("vmfMessage")).isString();
}

QJsonObject projectVmfMessage(const QJsonObject& message) {
    static const QStringList fields{
        QStringLiteral("id"), QStringLiteral("type"), QStringLiteral("sender"),
        QStringLiteral("receiver"), QStringLiteral("time"),
        QStringLiteral("requiresAck"), QStringLiteral("acked"),
        QStringLiteral("automaticAck"), QStringLiteral("retryCount"),
        QStringLiteral("traceId"), QStringLiteral("correlationId"),
        QStringLiteral("wireFormat"), QStringLiteral("vmfMessage"),
        QStringLiteral("wireBitLength"), QStringLiteral("catalogId"),
        QStringLiteral("trigger"), QStringLiteral("informationValue"),
        QStringLiteral("senderSide"),
        QStringLiteral("receiverSide"), QStringLiteral("simulationTime")};
    QJsonObject projected;
    for (const QString& field : fields) {
        if (message.contains(field)) projected.insert(field, message.value(field));
    }
    const QJsonObject payload = message.value(QStringLiteral("payload")).toObject();
    if (payload.contains(QStringLiteral("vmfValidated"))) {
        projected.insert(QStringLiteral("vmfValidated"),
                         payload.value(QStringLiteral("vmfValidated")));
    }
    if (payload.contains(QStringLiteral("vmfFieldCount"))) {
        projected.insert(QStringLiteral("fieldCount"),
                         payload.value(QStringLiteral("vmfFieldCount")));
    }
    for (const QString& field : {QStringLiteral("vmfCatalogId"),
                                 QStringLiteral("vmfTrigger"),
                                 QStringLiteral("vmfInformationValue")}) {
        if (!payload.contains(field)) continue;
        const QString projectedName = field == QLatin1String("vmfCatalogId")
            ? QStringLiteral("catalogId")
            : field == QLatin1String("vmfTrigger")
                ? QStringLiteral("trigger") : QStringLiteral("informationValue");
        projected.insert(projectedName, payload.value(field));
    }
    return projected;
}

QJsonObject observerEvent(const QJsonObject& event) {
    const QString kind = event.value(QStringLiteral("kind")).toString();
    if (kind == QLatin1String("vmfMessage")) {
        QJsonObject projected = copyEventFields(event, {QStringLiteral("messageType"),
                                       QStringLiteral("senderSide"),
                                       QStringLiteral("receiverSide"),
                                       QStringLiteral("catalogId"),
                                       QStringLiteral("trigger"),
                                       QStringLiteral("informationValue"),
                                       QStringLiteral("direction"),
                                       QStringLiteral("summary"),
                                       QStringLiteral("validated")});
        if (event.contains(QStringLiteral("workflow"))) {
            projected.insert(QStringLiteral("workflow"), workflowProjection(
                event.value(QStringLiteral("workflow")).toObject()));
        }
        return projected;
    }
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
    if (role == QLatin1String("editor") || role == QLatin1String("room_admin")
        || role == side) return true;
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
    const QString ownedId = ownedUnitForRole(engine, role, ownedUnitId);
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

QSet<QString> scanVisibleTargetIds(const SimulationEngine& engine, const QString& role,
                                   const QString& ownedUnitId, quint64 stateRevision) {
    QSet<QString> ids;
    if (hasFullVisibility(role)) return ids;
    const QString side = StateProjector::sideForRole(role);
    if (side.isEmpty()) return ids;
    const QString recipientId = ownedUnitForRole(engine, role, ownedUnitId);
    const QString cacheKey = projectionCacheKey(role, recipientId);
    for (const QJsonValue& value : engine.activeScanContacts()) {
        if (!value.isObject()) continue;
        const QJsonObject contact = value.toObject();
        if (contact.value(QStringLiteral("side")).toString() != side) continue;
        const QString scannerId = contact.value(QStringLiteral("scannerId")).toString();
        const QString targetId = contact.value(QStringLiteral("targetId")).toString();
        if (!engine.unit(scannerId) || !engine.unit(targetId)) continue;
        const bool eligible = !isSeat(role) || scannerId == recipientId
            || (!recipientId.isEmpty()
                && directedReachable(engine, scannerId, recipientId,
                                     stateRevision, cacheKey));
        if (eligible) ids.insert(targetId);
    }
    return ids;
}

QSet<QString> visibleUnitIdsImpl(const SimulationEngine& engine, const QString& role,
                                 const QString& ownedUnitId, quint64 stateRevision,
                                 bool includeActiveScan) {
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
    if (includeActiveScan) {
        ids.unite(scanVisibleTargetIds(engine, role, ownedUnitId, stateRevision));
    }
    return ids;
}

QJsonArray projectedProjectiles(const SimulationEngine& engine, const QString& role,
                                const QSet<QString>& visibleIds,
                                const QSet<QString>& friendlyVisibleIds) {
    const QString side = StateProjector::sideForRole(role);
    const bool fullVisibility = hasFullVisibility(role);
    const bool observer = isObserver(role);
    const double mapWidth = std::max(0.0, engine.scenario().map.widthMeters);
    const double mapHeight = std::max(0.0, engine.scenario().map.heightMeters);
    QList<QJsonObject> output;

    for (const QJsonValue& value : engine.projectilesSnapshot()) {
        if (!value.isObject()) continue;
        const QJsonObject projectile = value.toObject();
        const QString projectileSide = projectile.value(QStringLiteral("side")).toString();
        const QJsonArray position = projectile.value(QStringLiteral("position")).toArray();
        if (position.size() < 2) continue;
        const double x = position.at(0).toDouble();
        const double y = position.at(1).toDouble();

        bool visible = observer || fullVisibility || projectileSide == side;
        if (!visible && !side.isEmpty()) {
            for (const QString& sensorId : friendlyVisibleIds) {
                const UnitBase* sensor = engine.unit(sensorId);
                if (!sensor || !sensor->alive() || sensor->sideStr() != side) continue;
                if (std::hypot(sensor->pos().x - x, sensor->pos().y - y)
                    <= std::max(0.0, sensor->detectRange())) {
                    visible = true;
                    break;
                }
            }
            const QString targetId = projectile.value(QStringLiteral("targetId")).toString();
            const UnitBase* target = engine.unit(targetId);
            const double threatRadius = projectile.value(QStringLiteral("threatRadius"))
                                            .toDouble(SimulationEngine::kProjectileThreatRadiusMeters);
            if (!visible && target && target->sideStr() == side
                && std::hypot(target->pos().x - x, target->pos().y - y)
                    <= std::max(0.0, threatRadius)) {
                visible = true;
            }
        }
        if (!visible) continue;

        const QString attackerId = projectile.value(QStringLiteral("attackerId")).toString();
        const QString targetId = projectile.value(QStringLiteral("targetId")).toString();
        output.append(projectileForWire(
            projectile, fullVisibility,
            fullVisibility || (!observer && visibleIds.contains(attackerId)),
            fullVisibility || (!observer && visibleIds.contains(targetId)),
            mapWidth, mapHeight));
    }
    std::sort(output.begin(), output.end(), [](const QJsonObject& left,
                                               const QJsonObject& right) {
        return left.value(QStringLiteral("id")).toString()
            < right.value(QStringLiteral("id")).toString();
    });
    QJsonArray projected;
    for (const QJsonObject& projectile : output) projected.append(projectile);
    return projected;
}

} // namespace

QSet<QString> StateProjector::visibleUnitIds(const SimulationEngine& engine,
                                             const QString& role) {
    return visibleUnitIds(engine, role, QString{});
}

QSet<QString> StateProjector::visibleUnitIds(const SimulationEngine& engine,
                                             const QString& role,
                                             const QString& ownedUnitId) {
    return visibleUnitIdsImpl(engine, role, ownedUnitId, 0, true);
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

QSet<QString> StateProjector::sensorVisibleUnitIds(const SimulationEngine& engine,
                                                   const QString& role,
                                                   const QString& ownedUnitId) {
    return visibleUnitIdsImpl(engine, role, ownedUnitId, 0, false);
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
        const bool vmf = isVmfMessage(message);
        if (seat) {
            const QString sender = message.value(QStringLiteral("sender")).toString();
            const QString receiver = message.value(QStringLiteral("receiver")).toString();
            if (communicationVisible.contains(sender)
                || communicationVisible.contains(receiver)) {
                output.append(vmf ? projectVmfMessage(message) : message);
            }
        } else if (side.isEmpty() || message.value(QStringLiteral("senderSide")).toString() == side
                   || message.value(QStringLiteral("receiverSide")).toString() == side) {
            output.append(vmf ? projectVmfMessage(message) : message);
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

QJsonObject StateProjector::projectWorkflow(const QJsonObject& workflow) {
    return workflowProjection(workflow);
}

QJsonObject StateProjector::projectEvent(const SimulationEngine& engine, const QString& role,
                                         const QJsonObject& event,
                                         const QString& ownedUnitId) {
    if (isObserver(role)) return observerEvent(event);
    const QString kind = event.value(QStringLiteral("kind")).toString();
    if (kind == QLatin1String("vmfMessage")) {
        const QString side = sideForRole(role);
        if (side.isEmpty() && !hasFullVisibility(role)) return {};
        const QString senderId = event.value(QStringLiteral("senderUnitId")).toString();
        const QString receiverId = event.value(QStringLiteral("receiverUnitId")).toString();
        const UnitBase* sender = engine.unit(senderId);
        const UnitBase* receiver = receiverId == QLatin1String("*")
            ? nullptr : engine.unit(receiverId);
        const QString senderSide = event.value(QStringLiteral("senderSide")).toString(
            sender ? sender->sideStr() : QString());
        const QString receiverSide = event.value(QStringLiteral("receiverSide")).toString(
            receiver ? receiver->sideStr() : senderSide);
        const QSet<QString> visible = visibleUnitIds(engine, role, ownedUnitId);
        if (!hasFullVisibility(role)) {
            if (senderSide != side && !visible.contains(senderId)) return {};
            if (receiverId != QLatin1String("*") && receiverSide != side
                && !visible.contains(receiverId)) return {};
        }
        QJsonObject projected = copyEventFields(event, {
            QStringLiteral("messageId"), QStringLiteral("traceId"),
            QStringLiteral("correlationId"), QStringLiteral("vmfMessage"),
            QStringLiteral("wireFormat"), QStringLiteral("wireBitLength"),
            QStringLiteral("senderUnitId"), QStringLiteral("receiverUnitId"),
            QStringLiteral("messageType"), QStringLiteral("validated"),
            QStringLiteral("fieldCount"), QStringLiteral("retryCount"),
            QStringLiteral("acked"), QStringLiteral("catalogId"),
            QStringLiteral("trigger"), QStringLiteral("informationValue"),
            QStringLiteral("summary")});
        projected.insert(QStringLiteral("kind"), kind);
        projected.insert(QStringLiteral("senderSide"), senderSide);
        projected.insert(QStringLiteral("receiverSide"), receiverSide);
        if (event.contains(QStringLiteral("workflow"))
            && (hasFullVisibility(role) || senderSide == side)) {
            projected.insert(QStringLiteral("workflow"), workflowProjection(
                event.value(QStringLiteral("workflow")).toObject()));
        }
        return projected;
    }
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
                                        const QString& ownedUnitId,
                                        const QJsonObject& observerTrajectories) {
    if (isObserver(role)) {
        QStringList ids = sortedUnitIds(engine);
        QJsonArray runtime;
        for (const QString& id : ids) {
            const QJsonObject projected = observerRuntime(engine.unitSnapshot(id));
            if (!projected.isEmpty()) runtime.append(projected);
        }
        QJsonObject snapshot{{QStringLiteral("schemaVersion"), Protocol::SchemaVersion},
                           {QStringLiteral("stateRevision"),
                            static_cast<qint64>(stateRevision)},
                           {QStringLiteral("scenario"), observerScenario(engine)},
                           {QStringLiteral("units"), runtime},
                           {QStringLiteral("projectiles"),
                            projectedProjectiles(engine, role, {}, {})},
                           {QStringLiteral("roomState"),
                            observerRoomState(roomState, stateRevision)}};
        if (!observerTrajectories.isEmpty()) {
            snapshot[QStringLiteral("observerTrajectories")] = observerTrajectories;
        }
        return snapshot;
    }
    QSet<QString> visibleIds = visibleUnitIdsImpl(engine, role, ownedUnitId, stateRevision, true);
    for (const QString& id : explicitlyShared) {
        if (engine.unit(id)) visibleIds.insert(id);
    }
    const QString side = sideForRole(role);
    const bool fullVisibility = hasFullVisibility(role);

    QHash<QString, QJsonObject> runtimeById;
    for (const QJsonValue& value : engine.collectAllUnitsSnapshot()) {
        QJsonObject runtime = value.toObject();
        runtime.remove(QStringLiteral("recentPath"));
        const QString id = runtime.value(QStringLiteral("id")).toString();
        if (!visibleIds.contains(id)) continue;
        const bool friendly = runtime.value(QStringLiteral("side")).toString() == side;
        if (fullVisibility || friendly) {
            const UnitBase* unit = engine.unit(id);
            const QJsonObject capabilities = actionCapabilities(
                engine, role, ownedUnitId, unit, runtime, stateRevision);
            if (!capabilities.isEmpty()) {
                runtime.insert(QStringLiteral("actions"), capabilities);
            }
            runtimeById.insert(id, runtime);
        } else {
            runtimeById.insert(id, observedEnemyRuntime(runtime));
        }
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
    const QSet<QString> friendlyVisibleIds = friendlyVisibleUnitIdsImpl(
        engine, role, ownedUnitId, stateRevision);
    QJsonObject projectedRoomState = roomState;
    if (!fullVisibility) {
        projectedRoomState.remove(QStringLiteral("roomDescription"));
        projectedRoomState.remove(QStringLiteral("scenarioId"));
        projectedRoomState.remove(QStringLiteral("seatLimits"));
        projectedRoomState.remove(QStringLiteral("seatParameters"));
    }
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
                       {QStringLiteral("projectiles"),
                        projectedProjectiles(engine, role, visibleIds,
                                             friendlyVisibleIds)},
                       {QStringLiteral("messages"),
                        filteredMessagesImpl(engine, role, ownedUnitId, stateRevision)},
                       {QStringLiteral("roomState"), projectedRoomState}};
}

} // namespace gbr
