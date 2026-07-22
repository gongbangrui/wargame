#include "SimulationEngine.h"
#include "UnitBase.h"
#include "SnapshotCodec.h"
#include "LocalTransport.h"
#include "../units/AttackUAV.h"

#include <QDateTime>
#include <QJsonArray>
#include <QFileInfo>
#include <QDir>
#include <QSet>
#include <QRandomGenerator>
#include <algorithm>
#include <cmath>

namespace gbr {

namespace {

constexpr size_t kMaxScenarioUnits = 512;
constexpr size_t kMaxSchedulePoints = 512;
constexpr qsizetype kMaxUnitIdLength = 64;
constexpr qsizetype kMaxCallsignLength = 128;
constexpr double kMaxMapExtentMeters = 1'000'000.0;

bool isKnownKind(const QString& kind) {
    return kind == QLatin1String("commandpost")
        || kind == QLatin1String("reconuav")
        || kind == QLatin1String("attackuav")
        || kind == QLatin1String("groundscout")
        || kind == QLatin1String("jammeruav");
}

QString validateScenarioUnit(const ScenarioUnit& u, const ScenarioMap& map) {
    if (u.id.trimmed().isEmpty()) return QStringLiteral("单元ID不能为空");
    if (u.id.size() > kMaxUnitIdLength || u.callsign.size() > kMaxCallsignLength) {
        return QStringLiteral("单元 ID 或名称过长: %1").arg(u.id.left(kMaxUnitIdLength));
    }
    if (!isKnownKind(u.kind)) {
        return QStringLiteral("未知单元类型: %1 (%2)").arg(u.kind, u.id);
    }
    if (u.side != QLatin1String("red") && u.side != QLatin1String("blue")) {
        return QStringLiteral("单元阵营无效: %1 (%2)").arg(u.side, u.id);
    }
    const bool validPosition = std::isfinite(u.pos.x) && std::isfinite(u.pos.y)
        && std::isfinite(u.pos.alt) && u.pos.x >= 0.0 && u.pos.y >= 0.0
        && u.pos.x <= map.widthMeters && u.pos.y <= map.heightMeters;
    const bool validParams = std::isfinite(u.detectRange) && u.detectRange >= 0.0
        && std::isfinite(u.attackRange) && u.attackRange >= 0.0
        && std::isfinite(u.commRange) && u.commRange >= 0.0
        && std::isfinite(u.speed) && u.speed >= 0.0
        && std::isfinite(u.maxHp) && u.maxHp > 0.0
        && std::isfinite(u.attackPower) && u.attackPower >= 0.0
        && std::isfinite(u.armor) && u.armor >= 0.0 && u.armor <= 0.9
        && std::isfinite(u.repairRate) && u.repairRate >= 0.0
        && std::isfinite(u.subsystemRepairRate) && u.subsystemRepairRate >= 0.0;
    if (!validPosition || !validParams) {
        return QStringLiteral("单元参数无效: %1").arg(u.id);
    }
    if (u.kind == QLatin1String("attackuav")) {
        const bool validWeapon = u.ammoCapacity >= 0 && u.ammoCapacity <= 100000
            && u.initialAmmo >= 0 && u.initialAmmo <= u.ammoCapacity
            && std::isfinite(u.hitProbability) && u.hitProbability >= 0.0
            && u.hitProbability <= 1.0
            && std::isfinite(u.minAttackRange) && u.minAttackRange >= 0.0
            && std::isfinite(u.optimalRange) && u.optimalRange >= u.minAttackRange
            && u.optimalRange <= u.attackRange
            && std::isfinite(u.cooldownSec) && u.cooldownSec >= 0.0
            && std::isfinite(u.damageMin) && u.damageMin >= 0.0
            && std::isfinite(u.damageMax) && u.damageMax >= u.damageMin
            && std::isfinite(u.rangeFalloff) && u.rangeFalloff >= 0.0
            && u.rangeFalloff <= 1.0
            && std::isfinite(u.fuelCapacitySec) && u.fuelCapacitySec > 0.0
            && std::isfinite(u.initialFuelSec) && u.initialFuelSec >= 0.0
            && u.initialFuelSec <= u.fuelCapacitySec
            && std::isfinite(u.rearmDurationSec) && u.rearmDurationSec >= 0.0;
        if (!validWeapon) return QStringLiteral("攻击单元武器参数无效: %1").arg(u.id);
    }
    if (u.schedule.size() > kMaxSchedulePoints) {
        return QStringLiteral("单元计划点不能超过 %1 个: %2")
            .arg(kMaxSchedulePoints).arg(u.id);
    }
    for (const auto& point : u.schedule) {
        if (!std::isfinite(point.time) || point.time < 0.0
            || !std::isfinite(point.x) || !std::isfinite(point.y)
            || point.x < 0.0 || point.y < 0.0
            || point.x > map.widthMeters || point.y > map.heightMeters) {
            return QStringLiteral("单元计划点无效: %1").arg(u.id);
        }
    }
    return {};
}

bool isHostileTarget(const UnitBase* attacker, const UnitBase* target) {
    return attacker && target && attacker->alive() && target->alive()
        && attacker->side() != target->side();
}

} // namespace

SimulationEngine::SimulationEngine(QObject* parent)
    : SimulationEngine(nullptr, parent) {}

SimulationEngine::SimulationEngine(ITransport* transport, QObject* parent)
    : QObject(parent),
      m_ownedTransport(transport ? nullptr : std::make_unique<LocalTransport>(this)),
      m_transport(transport ? transport : m_ownedTransport.get()),
      m_map(std::make_unique<MapProvider>()),
      m_clock(std::make_unique<RealTimeClock>()),
      m_recorder(std::make_unique<MessageLogRecorder>()) {
    m_timer.setInterval(50);
    connect(&m_timer, &QTimer::timeout, this, [this](){ onTickInternal(false, 0.0); });
    // Forward bus emissions through the transport sink so the engine's
    // message cache and QML signals stay in sync regardless of which
    // transport is in use.
    m_transport->setMessageSink([this](const QJsonObject& obj) { onMessagePosted(obj); });

    m_dirtyTimer.setInterval(16);
    m_dirtyTimer.setSingleShot(true);
    connect(&m_dirtyTimer, &QTimer::timeout, this, &SimulationEngine::flushDirtyUnits);
    initCommandDispatch();
}

SimulationEngine::~SimulationEngine() {
    m_timer.stop();
    m_dirtyTimer.stop();
    // An externally-owned transport can outlive the engine. Do not leave its
    // message sink pointing at this destroyed object.
    if (m_transport) m_transport->setMessageSink({});
}

void SimulationEngine::loadDefaultScenario() {
    setScenario(ScenarioIo::defaultScenario());
}

bool SimulationEngine::setScenario(const Scenario& s) {
    if (s.units.empty()) {
        // Refuse to wipe a populated world with an empty scenario; emit a clear error
        // and let the caller (controller) decide what to do.
        m_lastError = QStringLiteral("场景单元为空，未应用");
        emit errorOccurred(m_lastError);
        return false;
    }
    if (!std::isfinite(s.map.widthMeters) || s.map.widthMeters <= 0.0
        || !std::isfinite(s.map.heightMeters) || s.map.heightMeters <= 0.0
        || s.map.widthMeters > kMaxMapExtentMeters
        || s.map.heightMeters > kMaxMapExtentMeters) {
        m_lastError = QStringLiteral("场景地图尺寸无效，未应用");
        emit errorOccurred(m_lastError);
        return false;
    }
    if (s.units.size() > kMaxScenarioUnits) {
        m_lastError = QStringLiteral("场景单元数量不能超过 %1，未应用").arg(kMaxScenarioUnits);
        emit errorOccurred(m_lastError);
        return false;
    }
    QSet<QString> ids;
    for (const auto& unit : s.units) {
        const QString validationError = validateScenarioUnit(unit, s.map);
        if (!validationError.isEmpty()) {
            m_lastError = validationError + QStringLiteral("，场景未应用");
            emit errorOccurred(m_lastError);
            return false;
        }
        if (ids.contains(unit.id)) {
            m_lastError = QStringLiteral("单元ID重复: %1，场景未应用").arg(unit.id);
            emit errorOccurred(m_lastError);
            return false;
        }
        ids.insert(unit.id);
    }
    setRunning(false);
    m_scenario = s;
    rebuildScenarioIndex();
    m_map->setLogicalSizeMeters(s.map.widthMeters, s.map.heightMeters);
    m_map->setName(s.map.name);
    m_map->setZoom(1.0);
    // Reset clock state in-place: keep speedMul, reset simTime.
    m_clock->setSpeedMul(m_clock->speedMul());
    // Reset simTime by swapping the clock to a fresh one with the same speedMul
    // (simTime is part of clock state; recreating is the simplest correct way
    // without expanding the IClock interface).
    const double carrySpeed = m_clock->speedMul();
    m_clock = std::make_unique<RealTimeClock>();
    m_clock->setSpeedMul(carrySpeed);
    emit speedMulChanged();
    m_messageCache.clear();
    m_lastError.clear();
    m_scanAccum = 0.0;
    m_reportCounter = 0;
    m_outcomeReported = false;
    m_destroyedReported.clear();
    m_cachedDetections.clear();
    m_pendingCombatRequests.clear();
    do {
        m_battleSeed = QRandomGenerator::system()->generate64();
    } while (m_battleSeed == 0);
    rebuildUnitsFromScenario();
    if (!m_replaying) {
        m_timeline = {};
        m_timelineSequence = 0;
        m_replayCommandSequence = 0;
        m_replayCommands.clear();
        m_replayCheckpoints.clear();
        m_replayInitialScenario = m_scenario;
        m_replayInitialSeed = m_battleSeed;
        m_lastReplayCheckpointTime = 0.0;
        m_recordedDuration = 0.0;
        m_recordedFinalSnapshot = collectAllUnitsSnapshot();
        captureReplayCheckpoint();
        appendTimeline(QStringLiteral("system"), QStringLiteral("场景已加载"),
                       QJsonObject{{QStringLiteral("unitCount"),
                                    static_cast<qint64>(m_units.size())}});
    }
    emit mapChanged();
    emit unitsChanged();
    emit messagesChanged();
    emit simTimeChanged();
    recomputeReadyForSim();
    return true;
}

void SimulationEngine::rebuildScenarioIndex() {
    m_scenarioIndex.clear();
    for (size_t i = 0; i < m_scenario.units.size(); ++i) {
        m_scenarioIndex.emplace(m_scenario.units[i].id, i);
    }
}

ScenarioUnit* SimulationEngine::findScenarioUnit(const QString& id) {
    auto it = m_scenarioIndex.find(id);
    if (it == m_scenarioIndex.end()) return nullptr;
    return &m_scenario.units[it->second];
}

void SimulationEngine::connectUnitSignals(UnitBase* unit, const QString& id) {
    connect(unit, &UnitBase::notifyEvent, this, [this, id](const QString& t, const QString& b, const QString& lvl){
        appendTimeline(QStringLiteral("unit"), t,
                       QJsonObject{{QStringLiteral("body"), b},
                                   {QStringLiteral("unitId"), id}}, lvl);
        emit eventPosted(t, b, lvl, id);
    });
    connect(unit, &UnitBase::perceptionChanged, this, [this, id]() {
        emit perceptionUpdated(id, unitSnapshot(id));
    });
    connect(unit, &UnitBase::hpChanged, this, [this, id]() {
        recomputeReadyForSim();
        auto* changed = this->unit(id);
        if (!m_inTick && changed && changed->kind() == UnitKind::CommandPost) {
            checkWinLoseCondition();
        }
        markUnitsDirty();
    });
}

void SimulationEngine::createSingleUnit(const ScenarioUnit& u) {
    if (u.id.isEmpty()) {
        m_lastError = QStringLiteral("单元ID不能为空");
        emit errorOccurred(m_lastError);
        return;
    }
    if (m_units.find(u.id) != m_units.end()) {
        // Duplicate id: silently dropping (via emplace returning false) would orphan the
        // scenario entry; surface it so the user can fix the JSON.
        m_lastError = QStringLiteral("单元ID重复: %1，已跳过").arg(u.id);
        emit errorOccurred(m_lastError);
        return;
    }
    UnitBase::Params p;
    p.detectRange = u.detectRange;
    p.attackRange = u.attackRange;
    p.commRange = u.commRange;
    p.speed = u.speed;
    p.maxHp = u.maxHp;
    p.attackPower = u.attackPower;
    p.armor = u.armor;
    p.repairRate = u.repairRate;
    p.subsystemRepairRate = u.subsystemRepairRate;
    p.pos = u.pos;
    auto unit = UnitBase::create(u.id, kindFromName(u.kind), sideFromName(u.side), m_transport->bus(), this);
    if (!unit) {
        m_lastError = QStringLiteral("无法创建单元类型: %1").arg(u.kind);
        emit errorOccurred(m_lastError);
        return;
    }
    const bool isCp = (unit->kind() == UnitKind::CommandPost);
    unit->setCallsign(u.callsign);
    unit->setParams(p);
    if (auto* attacker = qobject_cast<AttackUAV*>(unit.get())) attacker->configureWeapon(u);
    unit->setHp(p.maxHp);
    unit->setSchedule(u.schedule);
    connectUnitSignals(unit.get(), u.id);
    unit->setUnitLookup([this](const QString& uid) -> UnitBase* {
        auto it = m_units.find(uid);
        return it != m_units.end() ? it->second.get() : nullptr;
    });
    m_units.emplace(u.id, std::move(unit));
    if (isCp) {
        m_transport->setUnitCommandPost(u.id, true);
    }
}

void SimulationEngine::rebuildUnitsFromScenario() {
    m_units.clear();
    for (const auto& u : m_scenario.units) {
        createSingleUnit(u);
    }
    for (auto& [id, unit] : m_units) {
        unit->setCpId(commandSenderIdFor(unit.get()));
    }
}

void SimulationEngine::setRunning(bool r) {
    if (r && !m_readyForSim) {
        m_lastError = QStringLiteral("场景未就绪，无法开始推演: %1").arg(m_cpIssues);
        emit errorOccurred(m_lastError);
        return;
    }
    if (m_running == r) return;
    m_running = r;
    if (m_running) m_timer.start();
    else m_timer.stop();
    emit runningChanged();
    if (!m_replaying) {
        appendTimeline(QStringLiteral("control"),
                       r ? QStringLiteral("推演开始/继续")
                         : QStringLiteral("推演暂停"));
    }
}

void SimulationEngine::setSpeedMul(double m) {
    if (!std::isfinite(m)) return;
    m = std::max(0.0, m);
    if (m_clock->speedMul() == m) return;
    m_clock->setSpeedMul(m);
    emit speedMulChanged();
}

void SimulationEngine::stepOnce(double simSeconds) {
    if (m_running || !m_readyForSim || !std::isfinite(simSeconds) || simSeconds <= 0.0) return;
    onTickInternal(true, simSeconds);
}

void SimulationEngine::markUnitsDirty() {
    if (!m_unitsDirty) {
        m_unitsDirty = true;
        if (!m_dirtyTimer.isActive()) m_dirtyTimer.start();
    }
}

void SimulationEngine::flushDirtyUnits() {
    if (m_unitsDirty) {
        m_unitsDirty = false;
        emit unitsChanged();
    }
}

void SimulationEngine::onTickInternal(bool manual, double manualDt) {
    const double dt = manual ? manualDt : m_clock->tickInterval() * m_clock->speedMul();
    if (!manual && dt <= 0.0) return;
    m_clock->advance(dt);

    // Resolve all combat in the same simulation step before deciding the
    // winner. Otherwise unordered unit iteration can turn a simultaneous
    // command-post kill into an arbitrary red/blue victory.
    m_inTick = true;
    applySchedules(m_clock->simTime(), dt);
    tickUnits(dt);
    applyEcmJamming();
    resolveCombatRequests();
    scanReconDetections(dt);
    broadcastPositionReports(manual);
    refreshDetectionCache();
    m_inTick = false;

    checkWinLoseCondition();

    if (!m_replaying) {
        m_recordedDuration = std::max(m_recordedDuration, simTime());
        m_recordedFinalSnapshot = collectAllUnitsSnapshot();
        if (simTime() - m_lastReplayCheckpointTime >= 10.0) captureReplayCheckpoint();
    }

    emit simTimeChanged();
    markUnitsDirty();
}

void SimulationEngine::tickUnits(double dt) {
    for (auto& [id, u] : m_units) {
        // Remote-owned units are mirrored state; the local process does not
        // tick them — the peer does, and we receive the new state via the
        // transport. Single-process mode never has Remote units.
        if (u->owner() == UnitOwner::Remote) continue;
        u->onTick(dt);
        if (auto* attacker = qobject_cast<AttackUAV*>(u.get())) {
            if (auto request = attacker->takePendingShot()) {
                m_pendingCombatRequests.push_back(std::move(*request));
            }
        }
        // Don't sample path for dead units — they don't move and we'd be
        // pushing the same position into recentPath forever.
        if (u->alive()) u->sampleRecentPath(m_clock->simTime());
    }
}

void SimulationEngine::resolveCombatRequests() {
    if (m_pendingCombatRequests.empty()) return;
    std::sort(m_pendingCombatRequests.begin(), m_pendingCombatRequests.end(),
              [](const CombatRequest& left, const CombatRequest& right) {
                  if (left.attackerId != right.attackerId) {
                      return left.attackerId < right.attackerId;
                  }
                  return left.shotSequence < right.shotSequence;
              });

    QHash<QString, double> initialHp;
    QHash<QString, UnitBase::DamageDelta> accumulatedDamage;
    struct ResolvedShot {
        AttackUAV* attacker = nullptr;
        CombatOutcome outcome;
        bool killCredit = false;
    };
    std::vector<ResolvedShot> resolved;
    resolved.reserve(m_pendingCombatRequests.size());

    for (CombatRequest request : m_pendingCombatRequests) {
        auto* attacker = qobject_cast<AttackUAV*>(unit(request.attackerId));
        UnitBase* target = unit(request.targetId);
        if (!attacker || !attacker->alive() || !isHostileTarget(attacker, target)) continue;
        request.attackerEffectiveness = attacker->jamFactor();
        if (!initialHp.contains(request.targetId)) initialHp.insert(request.targetId, target->hp());

        CombatOutcome outcome = CombatResolver::resolve(request, m_battleSeed);
        UnitBase::DamageDelta& total = accumulatedDamage[request.targetId];
        const double damageBefore = total.hullDamage;
        outcome.hpBefore = std::max(0.0, initialHp.value(request.targetId) - damageBefore);
        int subsystemIndex = static_cast<int>(request.shotSequence % 4ULL);
        for (const char ch : request.attackerId.toUtf8()) subsystemIndex += static_cast<unsigned char>(ch);
        UnitBase::DamageDelta delta = outcome.hit()
            ? target->assessDamage(outcome.damage, subsystemIndex) : UnitBase::DamageDelta{};
        const double appliedDamage = std::min(delta.hullDamage, outcome.hpBefore);
        delta.hullDamage = appliedDamage;
        outcome.damage = appliedDamage;
        outcome.hpAfter = std::max(0.0, outcome.hpBefore - appliedDamage);
        const bool killCredit = outcome.hpBefore > 0.0 && outcome.hpAfter <= 0.0 && outcome.hit();
        total.hullDamage += delta.hullDamage;
        total.sensorLoss += delta.sensorLoss;
        total.commsLoss += delta.commsLoss;
        total.mobilityLoss += delta.mobilityLoss;
        total.weaponLoss += delta.weaponLoss;
        resolved.push_back(ResolvedShot{attacker, outcome, killCredit});
    }
    m_pendingCombatRequests.clear();

    QStringList damagedIds = accumulatedDamage.keys();
    damagedIds.sort();
    for (const QString& targetId : damagedIds) {
        UnitBase* target = unit(targetId);
        if (target && target->alive()) {
            target->applyDamageDelta(accumulatedDamage.value(targetId));
        }
    }
    for (ResolvedShot& shot : resolved) {
        shot.attacker->applyCombatOutcome(shot.outcome, shot.killCredit);
        appendTimeline(QStringLiteral("combat"),
                       shot.outcome.hit() ? QStringLiteral("武器命中")
                                          : QStringLiteral("武器未命中"),
                       QJsonObject{{QStringLiteral("shotId"), shot.outcome.shotId},
                                   {QStringLiteral("attackerId"), shot.outcome.attackerId},
                                   {QStringLiteral("targetId"), shot.outcome.targetId},
                                   {QStringLiteral("hit"), shot.outcome.hit()},
                                   {QStringLiteral("damage"), shot.outcome.damage},
                                   {QStringLiteral("distance"), shot.outcome.distance},
                                   {QStringLiteral("probability"), shot.outcome.effectiveProbability},
                                   {QStringLiteral("kill"), shot.killCredit}},
                       shot.killCredit ? QStringLiteral("warn") : QStringLiteral("info"));
    }
}

void SimulationEngine::applyEcmJamming() {
    constexpr double kJammedFactor = 0.4;
    std::vector<std::pair<QString, UnitBase*>> jammers;
    jammers.reserve(m_units.size());
    for (auto& [id, unit] : m_units) {
        if (unit->kind() == UnitKind::JammerUAV && unit->alive()) {
            jammers.emplace_back(id, unit.get());
        }
    }

    // Resolve jammer-on-jammer effects from base ranges as one simultaneous
    // phase. This avoids unordered_map iteration order deciding which jammer
    // gets to transmit at full range first.
    QSet<QString> jammedJammers;
    for (const auto& [sourceId, source] : jammers) {
        for (const auto& [targetId, target] : jammers) {
            if (sourceId == targetId || source->side() == target->side()) continue;
            if (source->pos().distanceTo2D(target->pos()) <= source->baseDetectRange()) {
                jammedJammers.insert(targetId);
            }
        }
    }

    std::unordered_map<QString, double> desiredFactors;
    desiredFactors.reserve(m_units.size());
    for (const auto& [id, _] : m_units) {
        desiredFactors.emplace(id, jammedJammers.contains(id) ? kJammedFactor : 1.0);
    }

    // A jammed jammer uses its reduced emission range against ordinary units.
    for (const auto& [sourceId, source] : jammers) {
        const double sourceFactor = jammedJammers.contains(sourceId) ? kJammedFactor : 1.0;
        const double jamRange = source->baseDetectRange() * sourceFactor;
        for (const auto& [targetId, target] : m_units) {
            if (!target->alive() || target->kind() == UnitKind::JammerUAV
                || target->side() == source->side()) {
                continue;
            }
            if (source->pos().distanceTo2D(target->pos()) <= jamRange) {
                desiredFactors[targetId] = kJammedFactor;
            }
        }
    }

    // Apply each result once so a continuously jammed unit does not emit a
    // reset-to-1.0 update followed by a second update on every tick.
    for (auto& [id, unit] : m_units) unit->applyJamming(desiredFactors[id]);
}

void SimulationEngine::scanReconDetections(double dt) {
    m_scanAccum += dt;
    constexpr double kScanInterval = 1.5;
    if (m_scanAccum < kScanInterval) return;
    m_scanAccum = 0.0;

    for (auto& [reconId, recon] : m_units) {
        if (recon->kind() != UnitKind::ReconUAV) continue;
        if (!recon->alive()) continue;
        const auto center = recon->pos();
        const double dr = recon->detectRange();
        const QString myCpId = commandSenderIdFor(recon.get());
        for (auto& [tid, target] : m_units) {
            if (target->side() == recon->side()) continue;
            if (!target->alive()) continue;
            const double d = center.distanceTo2D(target->pos());
            if (d > dr) continue;
            Message dm;
            dm.type = Message::Type::TargetDetect;
            dm.sender = reconId;
            dm.receiver = myCpId;
            dm.requiresAck = true;
            dm.payload["targetId"] = tid;
            dm.payload["callsign"] = target->callsign();
            dm.payload["x"] = target->pos().x;
            dm.payload["y"] = target->pos().y;
            dm.payload["alt"] = target->pos().alt;
            dm.payload["distance"] = d;
            m_transport->send(dm);
            // Communication range, not sensor range, determines which allies
            // receive the shared track. One broadcast also avoids one logged
            // message per ally.
            Message sm;
            sm.type = Message::Type::SharedDetect;
            sm.sender = reconId;
            sm.receiver = "*";
            sm.payload["targetId"] = tid;
            sm.payload["callsign"] = target->callsign();
            sm.payload["x"] = target->pos().x;
            sm.payload["y"] = target->pos().y;
            sm.payload["alt"] = target->pos().alt;
            sm.payload["distance"] = d;
            m_transport->send(sm);
        }
    }
}

void SimulationEngine::broadcastPositionReports(bool manual) {
    m_reportCounter++;
    if (m_reportCounter % 4 != 0 && !manual) return;
    for (auto& [id, u] : m_units) {
        if (!u->alive()) continue;
        Message m;
        m.type = Message::Type::PositionReport;
        m.sender = id;
        m.receiver = "*";
        m.payload["x"] = u->pos().x;
        m.payload["y"] = u->pos().y;
        m.payload["alt"] = u->pos().alt;
        m.payload["side"] = u->sideStr();
        m_transport->send(m);
    }
}

void SimulationEngine::refreshDetectionCache() {
    m_cachedDetections.clear();
    for (const auto& [id, u] : m_units) {
        if (!u->alive()) {
            m_cachedDetections[id] = {};
            continue;
        }
        QJsonArray dets;
        const auto center = u->pos();
        const double dr = u->detectRange();
        for (const auto& [oid, ou] : m_units) {
            if (oid == id) continue;
            if (ou->side() == u->side()) continue;
            if (!ou->alive()) continue;
            const double d = center.distanceTo2D(ou->pos());
            if (d <= dr) {
                QJsonObject det;
                det["id"] = oid;
                det["callsign"] = ou->callsign();
                det["kind"] = ou->kindStr();
                det["side"] = ou->sideStr();
                det["distance"] = d;
                QJsonArray p;
                p.append(ou->pos().x);
                p.append(ou->pos().y);
                p.append(ou->pos().alt);
                det["position"] = p;
                dets.append(det);
            }
        }
        m_cachedDetections[id] = dets;
    }
}

void SimulationEngine::applySchedules(double simTime, double dt) {
    for (auto& [id, u] : m_units) {
        if (!u->movable()) continue;
        if (!u->alive()) continue;
        if (u->hasActiveWaypoints()) continue;
        const auto& sched = u->schedule();
        if (sched.empty()) continue;

        GeoPos target;
        if (simTime <= sched.front().time) {
            target = GeoPos{sched.front().x, sched.front().y, u->pos().alt};
        } else if (simTime >= sched.back().time) {
            target = GeoPos{sched.back().x, sched.back().y, u->pos().alt};
        } else {
            for (size_t i = 0; i + 1 < sched.size(); i++) {
                const auto& a = sched[i];
                const auto& b = sched[i + 1];
                if (simTime >= a.time && simTime <= b.time) {
                    const double span = b.time - a.time;
                    const double t = span > 1e-6 ? (simTime - a.time) / span : 1.0;
                    target = GeoPos{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, u->pos().alt};
                    break;
                }
            }
        }

        const double dist = u->pos().distanceTo2D(target);
        constexpr double kSnapThreshold = 1.0;

        if (dist <= kSnapThreshold) {
            u->setPosition(target);
        } else {
            const double t = std::min(1.0, u->speed() * dt / std::max(1.0, dist));
            const GeoPos lerped = u->pos().lerp(target, t);
            u->setPosition(lerped);
        }
    }
}

void SimulationEngine::onMessagePosted(const QJsonObject& msg) {
    updateMessageCache(msg);
    if (m_recorder) m_recorder->record(msg);
    emit messagesChanged();

    if (msg.value("type").toString() == "TargetDestroyed") {
        const QString targetId = msg.value("payload").toObject().value("targetId").toString();
        const QString attackerId = msg.value("payload").toObject().value("attackerId").toString();
        if (targetId.isEmpty() || m_destroyedReported.contains(targetId)) return;
        auto it = m_units.find(targetId);
        // messagePosted also contains messages that failed communication
        // checks. A notification must never become an authoritative damage
        // command; local combat applies HP before posting this event.
        if (it != m_units.end() && !it->second->alive()) {
            m_destroyedReported.insert(targetId);
            const double dx = it->second->pos().x;
            const double dy = it->second->pos().y;
            emit targetDestroyedVisual(targetId, dx, dy);
            emit unitDestroyed(targetId);
            QString targetSide = it->second->sideStr();
            QString sideLabel = (targetSide == "red") ? QString::fromUtf8("红方") : QString::fromUtf8("蓝方");
            emit eventPosted(
                QString::fromUtf8("单元被摧毁"),
                QString::fromUtf8("%1单元 %2 被 %3 摧毁").arg(sideLabel, targetId, attackerId),
                "warn",
                targetId
            );
            recomputeReadyForSim();
        }
    }
}

void SimulationEngine::recomputeReadyForSim() {
    int redLive = 0, blueLive = 0;
    int redDead = 0, blueDead = 0;
    QStringList issueList;
    QString redFirstDead, blueFirstDead;
    for (const auto& [id, u] : m_units) {
        if (u->kind() != UnitKind::CommandPost) continue;
        if (!u->alive()) {
            if (u->side() == Side::Red) { redDead++; if (redFirstDead.isEmpty()) redFirstDead = id; }
            else { blueDead++; if (blueFirstDead.isEmpty()) blueFirstDead = id; }
            issueList << id + " (已摧毁)";
            continue;
        }
        if (u->side() == Side::Red) redLive++;
        else if (u->side() == Side::Blue) blueLive++;
    }
    if (redLive == 0) {
        if (redDead > 0) issueList << "红方指挥所已全部被摧毁";
        else issueList << "红方缺失指挥所";
    } else if (redLive > 1) issueList << "红方指挥所重复 (x" + QString::number(redLive) + ")";
    if (blueLive == 0) {
        if (blueDead > 0) issueList << "蓝方指挥所已全部被摧毁";
        else issueList << "蓝方缺失指挥所";
    } else if (blueLive > 1) issueList << "蓝方指挥所重复 (x" + QString::number(blueLive) + ")";
    const bool ok = (redLive == 1) && (blueLive == 1);
    const QString issues = issueList.join("; ");
    if (ok != m_readyForSim || issues != m_cpIssues) {
        const bool wasReady = m_readyForSim;
        m_readyForSim = ok;
        m_cpIssues = issues;
        if (!wasReady && ok) m_outcomeReported = false;
        emit readyForSimChanged();
        // CP-death path is owned by checkWinLoseCondition() (it emits
        // simulationEnded once and stops once). For non-death readiness loss
        // (duplicate CP, missing CP via addOrUpdateUnit), pause here so the
        // user notices the broken state.
        if (wasReady && !ok && m_running && !issues.contains(QStringLiteral("已全部被摧毁"))) {
            setRunning(false);
        }
    }
}

void SimulationEngine::updateMessageCache(const QJsonObject& msg) {
    m_messageCache.prepend(msg);
    if (m_messageCache.size() > 200) m_messageCache.removeLast();
}

QJsonObject SimulationEngine::unitSnapshot(const QString& id) const {
    auto it = m_units.find(id);
    if (it == m_units.end()) return {};
    auto& u = it->second;
    QJsonObject o;
    o["id"] = u->id();
    o["callsign"] = u->callsign();
    o["kind"] = u->kindStr();
    o["side"] = u->sideStr();
    o["movable"] = u->movable();
    QJsonArray pos;
    pos.append(u->pos().x);
    pos.append(u->pos().y);
    pos.append(u->pos().alt);
    o["position"] = pos;
    o["detectRange"] = u->detectRange();
    o["attackRange"] = u->attackRange();
    o["commRange"] = u->commRange();
    o["speed"] = u->speed();
    o["maxHp"] = u->maxHp();
    o["attackPower"] = u->attackPower();
    o["armor"] = u->armor();
    o["hp"] = u->hp();
    o["alive"] = u->alive();
    o["disabled"] = u->disabled();
    o["subsystems"] = u->subsystemStateJson();
    o["serviceRequested"] = u->serviceRequested();
    o["serviceProgress"] = u->serviceProgress();
    o["status"] = u->statusText();
    o["sharedKnowledge"] = u->sharedKnowledgeJson();
    QJsonArray rp;
    const auto rpv = u->recentPath();
    for (const auto& rppi : rpv) {
        const auto rpm = rppi.toMap();
        QJsonObject r;
        r["x"] = rpm.value("x").toDouble();
        r["y"] = rpm.value("y").toDouble();
        rp.append(r);
    }
    o["recentPath"] = rp;
    QJsonArray dets;
    auto dit = m_cachedDetections.find(id);
    if (dit != m_cachedDetections.end()) dets = dit->second;
    o["detections"] = dets;
    QJsonArray sched;
    for (const auto& sp : u->schedule()) {
        QJsonObject p;
        p["time"] = sp.time;
        p["x"] = sp.x;
        p["y"] = sp.y;
        sched.append(p);
    }
    o["schedule"] = sched;
    if (u->kind() == UnitKind::JammerUAV) {
        o["jammer"] = true;
        o["jamFactor"] = u->jamFactor();
    }
    if (auto* attacker = qobject_cast<AttackUAV*>(u.get())) {
        const ScenarioUnit* configured = nullptr;
        auto index = m_scenarioIndex.find(id);
        if (index != m_scenarioIndex.end()) configured = &m_scenario.units[index->second];
        o["ammoRemaining"] = attacker->ammoRemaining();
        o["ammoCapacity"] = attacker->ammoCapacity();
        o["cooldownRemaining"] = attacker->cooldownRemaining();
        o["lastShotOutcome"] = attacker->lastShotOutcome();
        o["fuelRemaining"] = attacker->fuelRemaining();
        o["fuelCapacity"] = attacker->fuelCapacity();
        o["turnaroundProgress"] = attacker->turnaroundProgress();
        o["turnaroundElapsed"] = attacker->turnaroundElapsed();
        o["rulesOfEngagement"] = attacker->rulesOfEngagement();
        o["targetId"] = attacker->targetId();
        o["armed"] = attacker->armed();
        if (configured) {
            o["initialAmmo"] = configured->initialAmmo;
            o["hitProbability"] = configured->hitProbability;
            o["optimalRange"] = configured->optimalRange;
            o["minAttackRange"] = configured->minAttackRange;
            o["cooldownSec"] = configured->cooldownSec;
            o["damageMin"] = configured->damageMin;
            o["damageMax"] = configured->damageMax;
            o["rangeFalloff"] = configured->rangeFalloff;
            o["rearmDurationSec"] = configured->rearmDurationSec;
        }
    }
    return o;
}

/// @brief Resolve the actual registered CommandPost id for a unit's side.
/// @details Avoids hardcoding "red_cp"/"blue_cp" so user-renamed CPs keep working.
QString SimulationEngine::commandSenderIdFor(const UnitBase* u) const {
    if (!u) return QString();
    for (const auto& [id, other] : m_units) {
        if (other->kind() == UnitKind::CommandPost && other->side() == u->side() && other->alive()) {
            return id;
        }
    }
    // No alive CP for this side. Caller (command dispatch) treats empty sender as "no comm available"
    // and surfaces a user-visible error instead of silently dropping the order.
    return QString();
}

void SimulationEngine::initCommandDispatch() {
    m_dispatch["assignTarget"]  = [this](auto& a){ cmdAssignTarget(a); };
    m_dispatch["setFlightPlan"] = [this](auto& a){ cmdSetFlightPlan(a); };
    m_dispatch["engageTarget"]  = [this](auto& a){ cmdEngageTarget(a); };
    m_dispatch["moveTo"]        = [this](auto& a){ cmdMoveTo(a); };
    m_dispatch["withdraw"]      = [this](auto& a){ cmdWithdraw(a); };
    m_dispatch["setSpeed"]      = [this](auto& a){ cmdSetSpeed(a); };
    m_dispatch["pursue"]        = [this](auto& a){ cmdPursue(a); };
    m_dispatch["guideAttack"]   = [this](auto& a){ cmdGuideAttack(a); };
    m_dispatch["setSchedule"]   = [this](auto& a){ cmdSetSchedule(a); };
    m_dispatch["halt"]          = [this](auto& a){ cmdHalt(a); };
    m_dispatch["service"]       = [this](auto& a){ cmdService(a); };
    m_dispatch["cancelEngagement"] = [this](auto& a){ cmdCancelEngagement(a); };
    m_dispatch["setRoe"]        = [this](auto& a){ cmdSetRulesOfEngagement(a); };
}

QVariantMap SimulationEngine::command(const QString& action, const QVariantMap& args) {
    return executeCommand(action, args).toVariantMap();
}

CommandResult SimulationEngine::executeCommand(const QString& action, const QVariantMap& args) {
    const CommandResult validation = validateCommand(action, args);
    if (!validation.accepted) {
        m_lastError = validation.message;
        emit errorOccurred(m_lastError);
        return validation;
    }
    auto it = m_dispatch.find(action);
    if (!m_replaying) {
        m_replayCommands.push_back(ReplayCommand{simTime(), ++m_replayCommandSequence,
                                                  action, args});
        appendTimeline(QStringLiteral("command"), QStringLiteral("命令已接受"),
                       QJsonObject{{QStringLiteral("action"), action},
                                   {QStringLiteral("args"),
                                    QJsonObject::fromVariantMap(args)}});
    }
    it->second(args);
    return CommandResult::ok();
}

CommandResult SimulationEngine::validateCommand(const QString& action,
                                                const QVariantMap& args) const {
    const auto dispatchIt = m_dispatch.find(action);
    if (dispatchIt == m_dispatch.end()) {
        return CommandResult::reject(QString::fromLatin1(CommandCode::UnknownAction),
                                     QStringLiteral("未知操作: %1").arg(action));
    }

    QString unitId;
    if (action == QLatin1String("guideAttack")) {
        unitId = args.value(QStringLiteral("guideId")).toString();
    } else if (action == QLatin1String("assignTarget")
               || action == QLatin1String("setFlightPlan")
               || action == QLatin1String("engageTarget")
               || action == QLatin1String("pursue")) {
        unitId = args.value(QStringLiteral("attackerId")).toString();
    } else {
        unitId = args.value(QStringLiteral("unitId")).toString();
    }

    UnitBase* controlled = unit(unitId);
    if (!controlled) {
        return CommandResult::reject(QString::fromLatin1(CommandCode::UnitNotFound),
                                     QStringLiteral("未知单元: %1").arg(unitId));
    }
    if (!controlled->alive()) {
        return CommandResult::reject(QString::fromLatin1(CommandCode::UnitDestroyed),
                                     QStringLiteral("单元已摧毁: %1").arg(unitId));
    }
    if (!controlled->movable()) {
        return CommandResult::reject(QString::fromLatin1(CommandCode::UnitNotMovable),
                                     QStringLiteral("该操作仅适用于可移动单元: %1").arg(unitId));
    }

    auto validPoint = [this](const QVariantMap& point) {
        if (!point.contains(QStringLiteral("x")) || !point.contains(QStringLiteral("y"))) {
            return false;
        }
        bool xOk = false;
        bool yOk = false;
        const double x = point.value(QStringLiteral("x")).toDouble(&xOk);
        const double y = point.value(QStringLiteral("y")).toDouble(&yOk);
        return xOk && yOk && std::isfinite(x) && std::isfinite(y)
            && x >= 0.0 && y >= 0.0
            && x <= m_scenario.map.widthMeters && y <= m_scenario.map.heightMeters;
    };

    const bool attackAction = action == QLatin1String("assignTarget")
        || action == QLatin1String("engageTarget")
        || action == QLatin1String("pursue");
    if ((attackAction || action == QLatin1String("setFlightPlan"))
        && controlled->kind() != UnitKind::AttackUAV) {
        return CommandResult::reject(QString::fromLatin1(CommandCode::InvalidUnitKind),
                                     QStringLiteral("该操作仅适用于攻击无人机"));
    }
    if ((action == QLatin1String("cancelEngagement") || action == QLatin1String("setRoe"))
        && controlled->kind() != UnitKind::AttackUAV) {
        return CommandResult::reject(QString::fromLatin1(CommandCode::InvalidUnitKind),
                                     QStringLiteral("该操作仅适用于攻击无人机"));
    }
    if (action == QLatin1String("setRoe")) {
        const QString roe = args.value(QStringLiteral("roe")).toString();
        if (roe != QLatin1String("hold") && roe != QLatin1String("free")) {
            return CommandResult::reject(QString::fromLatin1(CommandCode::InvalidArgument),
                                         QStringLiteral("交战规则必须为 hold 或 free"));
        }
    }

    if (attackAction) {
        const QString targetId = args.value(QStringLiteral("targetId")).toString();
        UnitBase* target = unit(targetId);
        if (!isHostileTarget(controlled, target)) {
            return CommandResult::reject(QString::fromLatin1(CommandCode::InvalidTarget),
                                         QStringLiteral("敌方目标不存在、已摧毁或阵营无效"));
        }
    }

    if (action == QLatin1String("guideAttack")) {
        UnitBase* attacker = unit(args.value(QStringLiteral("attackerId")).toString());
        UnitBase* target = unit(args.value(QStringLiteral("targetId")).toString());
        if (controlled->kind() != UnitKind::GroundScout || !attacker || !attacker->alive()
            || attacker->kind() != UnitKind::AttackUAV
            || controlled->side() != attacker->side()
            || !isHostileTarget(attacker, target)) {
            return CommandResult::reject(QString::fromLatin1(CommandCode::InvalidTarget),
                                         QStringLiteral("引导单元、攻击单元或敌方目标无效"));
        }
        if (!validPoint(args.value(QStringLiteral("targetPos")).toMap())) {
            return CommandResult::reject(QString::fromLatin1(CommandCode::InvalidArgument),
                                         QStringLiteral("引导目标位置无效或超出地图边界"));
        }
    }

    if (action == QLatin1String("moveTo")
        && !validPoint(args.value(QStringLiteral("pos")).toMap())) {
        return CommandResult::reject(QString::fromLatin1(CommandCode::InvalidArgument),
                                     QStringLiteral("移动目标无效或超出地图边界"));
    }
    if (action == QLatin1String("setFlightPlan")) {
        const QVariantList waypoints = args.value(QStringLiteral("waypoints")).toList();
        if (waypoints.isEmpty() || waypoints.size() > 512) {
            return CommandResult::reject(QString::fromLatin1(CommandCode::InvalidArgument),
                                         QStringLiteral("航路不能为空且不能超过 512 个航点"));
        }
        for (const QVariant& waypoint : waypoints) {
            if (!validPoint(waypoint.toMap())) {
                return CommandResult::reject(QString::fromLatin1(CommandCode::InvalidArgument),
                                             QStringLiteral("航路点无效或超出地图边界"));
            }
        }
    }
    if (action == QLatin1String("setSpeed")) {
        bool ok = false;
        const double speed = args.value(QStringLiteral("speed")).toDouble(&ok);
        if (!ok || !std::isfinite(speed) || speed <= 0.0 || speed > 1000.0) {
            return CommandResult::reject(QString::fromLatin1(CommandCode::InvalidArgument),
                                         QStringLiteral("单元速度必须在 0 到 1000 之间"));
        }
    }
    if (action == QLatin1String("setSchedule")) {
        const QVariantList schedule = args.value(QStringLiteral("schedule")).toList();
        if (schedule.size() > 512) {
            return CommandResult::reject(QString::fromLatin1(CommandCode::InvalidArgument),
                                         QStringLiteral("计划点不能超过 512 个"));
        }
        for (const QVariant& value : schedule) {
            const QVariantMap point = value.toMap();
            bool timeOk = false;
            const double time = point.value(QStringLiteral("time")).toDouble(&timeOk);
            if (!timeOk || !std::isfinite(time) || time < 0.0 || !validPoint(point)) {
                return CommandResult::reject(QString::fromLatin1(CommandCode::InvalidArgument),
                                             QStringLiteral("计划点时间或位置无效"));
            }
        }
    }

    const bool needsCommandPost = action != QLatin1String("setSpeed")
        && action != QLatin1String("setSchedule")
        && action != QLatin1String("guideAttack");
    if (needsCommandPost && commandSenderIdFor(controlled).isEmpty()) {
        return CommandResult::reject(QString::fromLatin1(CommandCode::CommandPostUnavailable),
                                     QStringLiteral("己方指挥所已摧毁，无法派单: %1").arg(unitId));
    }
    return CommandResult::ok();
}

void SimulationEngine::cmdAssignTarget(const QVariantMap& args) {
    const auto attackerId = args.value("attackerId").toString();
    const auto targetId = args.value("targetId").toString();
    auto* u = unit(attackerId);
    auto* target = unit(targetId);
    if (!isHostileTarget(u, target) || u->kind() != UnitKind::AttackUAV) return;
    const QString cpId = commandSenderIdFor(u);
    if (cpId.isEmpty()) {
        m_lastError = QStringLiteral("己方指挥所已摧毁，无法派单: %1").arg(attackerId);
        emit errorOccurred(m_lastError);
        return;
    }
    Message m;
    m.type = Message::Type::AttackOrder;
    m.sender = cpId;
    m.receiver = attackerId;
    m.requiresAck = true;
    m.payload["targetId"] = targetId;
    m_transport->send(m);
}

void SimulationEngine::cmdSetFlightPlan(const QVariantMap& args) {
    const auto attackerId = args.value("attackerId").toString();
    auto* u = unit(attackerId);
    if (!u || !u->alive() || !u->movable()) return;
    Message m;
    m.type = Message::Type::FlightPlan;
    m.receiver = attackerId;
    QJsonArray wp;
    for (const auto& v : args.value("waypoints").toList()) {
        const auto wpt = v.toMap();
        if (!wpt.contains("x") || !wpt.contains("y")) return;
        const double x = wpt.value("x").toDouble();
        const double y = wpt.value("y").toDouble();
        if (!std::isfinite(x) || !std::isfinite(y)) return;
        wp.append(QJsonObject{{"x", x}, {"y", y}, {"alt", 2000.0}});
    }
    if (wp.isEmpty()) return;
    const QString cpId = commandSenderIdFor(u);
    if (cpId.isEmpty()) {
        m_lastError = QStringLiteral("己方指挥所已摧毁，无法派单: %1").arg(attackerId);
        emit errorOccurred(m_lastError);
        return;
    }
    m.sender = cpId;
    m.payload["waypoints"] = wp;
    m_transport->send(m);
}

void SimulationEngine::cmdEngageTarget(const QVariantMap& args) {
    const auto attackerId = args.value("attackerId").toString();
    const auto targetId = args.value("targetId").toString();
    auto* u = unit(attackerId);
    auto* target = unit(targetId);
    if (!isHostileTarget(u, target) || u->kind() != UnitKind::AttackUAV) return;
    const QString cpId = commandSenderIdFor(u);
    if (cpId.isEmpty()) {
        m_lastError = QStringLiteral("己方指挥所已摧毁，无法派单: %1").arg(attackerId);
        emit errorOccurred(m_lastError);
        return;
    }
    Message m;
    m.type = Message::Type::AttackOrder;
    m.sender = cpId;
    m.receiver = attackerId;
    m.payload["fireNow"] = true;
    m.payload["targetId"] = targetId;
    m_transport->send(m);
}

void SimulationEngine::cmdMoveTo(const QVariantMap& args) {
    const auto uid = args.value("unitId").toString();
    const auto posMap = args.value("pos").toMap();
    auto* u = unit(uid);
    if (!u || !u->alive() || !u->movable()) return;
    if (!posMap.contains("x") || !posMap.contains("y")) return;
    if (!std::isfinite(posMap.value("x").toDouble())
        || !std::isfinite(posMap.value("y").toDouble())) return;
    const QString cpId = commandSenderIdFor(u);
    if (cpId.isEmpty()) {
        m_lastError = QStringLiteral("己方指挥所已摧毁，无法派单: %1").arg(uid);
        emit errorOccurred(m_lastError);
        return;
    }
    Message m;
    m.type = Message::Type::Guidance;
    m.sender = cpId;
    m.receiver = uid;
    m.payload["x"] = posMap.value("x").toDouble();
    m.payload["y"] = posMap.value("y").toDouble();
    m.payload["kind"] = QString("moveTo");
    m_transport->send(m);
}

void SimulationEngine::cmdWithdraw(const QVariantMap& args) {
    const auto uid = args.value("unitId").toString();
    auto* u = unit(uid);
    if (!u || !u->alive() || !u->movable()) return;
    const QString cpId = commandSenderIdFor(u);
    if (cpId.isEmpty()) {
        m_lastError = QStringLiteral("己方指挥所已摧毁，无法派单: %1").arg(uid);
        emit errorOccurred(m_lastError);
        return;
    }
    Message m;
    m.type = Message::Type::Withdraw;
    m.sender = cpId;
    m.receiver = uid;
    m.requiresAck = true;
    if (auto* cp = unit(cpId)) {
        m.payload["homeX"] = cp->pos().x;
        m.payload["homeY"] = cp->pos().y;
    } else {
        m.payload["homeX"] = u->pos().x;
        m.payload["homeY"] = u->pos().y;
    }
    m_transport->send(m);
}

void SimulationEngine::cmdSetSpeed(const QVariantMap& args) {
    const auto uid = args.value("unitId").toString();
    const auto v = args.value("speed").toDouble();
    auto* u = unit(uid);
    if (!u) return;
    if (!u->movable() || !u->alive()) return;
    if (!std::isfinite(v) || v <= 0.0) return;
    // Clamp absurd speeds (>1km/s would let a unit teleport across the map).
    constexpr double kMaxSpeed = 1000.0;
    u->setSpeed(std::min(v, kMaxSpeed));
}

void SimulationEngine::cmdPursue(const QVariantMap& args) {
    const auto attackerId = args.value("attackerId").toString();
    const auto targetId = args.value("targetId").toString();
    auto* u = unit(attackerId);
    auto* t = unit(targetId);
    if (!isHostileTarget(u, t) || u->kind() != UnitKind::AttackUAV) return;
    const QString cpId = commandSenderIdFor(u);
    if (cpId.isEmpty()) {
        m_lastError = QStringLiteral("己方指挥所已摧毁，无法派单: %1").arg(attackerId);
        emit errorOccurred(m_lastError);
        return;
    }
    Message m;
    m.type = Message::Type::Pursue;
    m.sender = cpId;
    m.receiver = attackerId;
    m.payload["targetId"] = targetId;
    m.payload["x"] = t->pos().x;
    m.payload["y"] = t->pos().y;
    m_transport->send(m);
}

void SimulationEngine::cmdGuideAttack(const QVariantMap& args) {
    const auto guideId = args.value("guideId").toString();
    const auto attackerId = args.value("attackerId").toString();
    const auto targetId = args.value("targetId").toString();
    const auto tpMap = args.value("targetPos").toMap();
    auto* guide = unit(guideId);
    auto* attacker = unit(attackerId);
    auto* target = unit(targetId);
    const double targetX = tpMap.value("x").toDouble();
    const double targetY = tpMap.value("y").toDouble();
    if (!guide || !guide->alive() || guide->kind() != UnitKind::GroundScout
        || !attacker || attacker->kind() != UnitKind::AttackUAV
        || guide->side() != attacker->side()
        || !isHostileTarget(attacker, target)
        || !tpMap.contains("x") || !tpMap.contains("y")
        || !std::isfinite(targetX) || !std::isfinite(targetY)) return;
    {
        Message m;
        m.type = Message::Type::FlightPlan;
        m.sender = guideId;
        m.receiver = attackerId;
        QJsonArray wp;
        QJsonObject w0; w0["x"] = targetX; w0["y"] = targetY; w0["alt"] = 2000.0;
        wp.append(w0);
        m.payload["waypoints"] = wp;
        m.payload["targetId"] = targetId;
        m_transport->send(m);
    }
    {
        Message m;
        m.type = Message::Type::AttackOrder;
        m.sender = guideId;
        m.receiver = attackerId;
        m.payload["fireNow"] = true;
        m.payload["targetId"] = targetId;
        m_transport->send(m);
    }
}

void SimulationEngine::cmdSetSchedule(const QVariantMap& args) {
    const auto uid = args.value("unitId").toString();
    auto* u = unit(uid);
    if (!u || !u->alive() || !u->movable()) {
        if (uid.isEmpty() || !unit(uid)) {
            m_lastError = QStringLiteral("未知单元: %1").arg(uid);
            emit errorOccurred(m_lastError);
        }
        return;
    }
    std::vector<SchedulePoint> sched;
    const auto list = args.value("schedule").toList();
    for (int i = 0; i < list.size(); ++i) {
        const auto m = list[i].toMap();
        if (!m.contains("time") || !m.contains("x") || !m.contains("y")) {
            m_lastError = QStringLiteral("计划点 #%1 缺少字段 (time/x/y): %2").arg(i).arg(uid);
            emit errorOccurred(m_lastError);
            return;
        }
        SchedulePoint p;
        p.time = m.value("time").toDouble();
        p.x = m.value("x").toDouble();
        p.y = m.value("y").toDouble();
        if (!std::isfinite(p.time) || !std::isfinite(p.x) || !std::isfinite(p.y)) {
            m_lastError = QStringLiteral("计划点 #%1 含 NaN/Inf: %2").arg(i).arg(uid);
            emit errorOccurred(m_lastError);
            return;
        }
        sched.push_back(p);
    }
    std::sort(sched.begin(), sched.end(),
              [](const SchedulePoint& a, const SchedulePoint& b) {
                  return a.time < b.time;
              });
    u->cancelWaypointMotion();
    u->setSchedule(sched);
    if (auto* su = findScenarioUnit(uid)) su->schedule = sched;
    emit unitsChanged();
}

void SimulationEngine::cmdHalt(const QVariantMap& args) {
    const auto unitId = args.value("unitId").toString();
    auto* u = unit(unitId);
    if (!u || !u->alive() || !u->movable()) return;
    const QString cpId = commandSenderIdFor(u);
    if (cpId.isEmpty()) {
        m_lastError = QStringLiteral("己方指挥所已摧毁，无法派单: %1").arg(unitId);
        emit errorOccurred(m_lastError);
        return;
    }
    Message m;
    m.type = Message::Type::Halt;
    m.sender = cpId;
    m.receiver = unitId;
    m_transport->send(m);
}

void SimulationEngine::cmdService(const QVariantMap& args) {
    const QString unitId = args.value(QStringLiteral("unitId")).toString();
    UnitBase* controlled = unit(unitId);
    if (!controlled || !controlled->alive() || !controlled->movable()) return;
    const QString cpId = commandSenderIdFor(controlled);
    UnitBase* cp = unit(cpId);
    if (!cp || !cp->alive()) return;
    Message message;
    message.type = Message::Type::Withdraw;
    message.sender = cpId;
    message.receiver = unitId;
    message.requiresAck = true;
    message.payload[QStringLiteral("homeX")] = cp->pos().x;
    message.payload[QStringLiteral("homeY")] = cp->pos().y;
    message.payload[QStringLiteral("service")] = true;
    m_transport->send(message);
}

void SimulationEngine::cmdCancelEngagement(const QVariantMap& args) {
    const QString unitId = args.value(QStringLiteral("unitId")).toString();
    UnitBase* controlled = unit(unitId);
    const QString cpId = commandSenderIdFor(controlled);
    if (!controlled || cpId.isEmpty()) return;
    Message message;
    message.type = Message::Type::CancelEngagement;
    message.sender = cpId;
    message.receiver = unitId;
    m_transport->send(message);
}

void SimulationEngine::cmdSetRulesOfEngagement(const QVariantMap& args) {
    const QString unitId = args.value(QStringLiteral("unitId")).toString();
    UnitBase* controlled = unit(unitId);
    const QString cpId = commandSenderIdFor(controlled);
    if (!controlled || cpId.isEmpty()) return;
    Message message;
    message.type = Message::Type::SetRulesOfEngagement;
    message.sender = cpId;
    message.receiver = unitId;
    message.payload[QStringLiteral("roe")] = args.value(QStringLiteral("roe")).toString();
    m_transport->send(message);
}

void SimulationEngine::addOrUpdateUnit(const ScenarioUnit& su) {
    const QString validationError = validateScenarioUnit(su, m_scenario.map);
    if (!validationError.isEmpty()) {
        m_lastError = validationError;
        emit errorOccurred(m_lastError);
        return;
    }
    if (m_scenarioIndex.find(su.id) == m_scenarioIndex.end()
        && m_scenario.units.size() >= kMaxScenarioUnits) {
        m_lastError = QStringLiteral("场景单元数量不能超过 %1").arg(kMaxScenarioUnits);
        emit errorOccurred(m_lastError);
        return;
    }

    // Update scenario store via O(1) index
    auto idxIt = m_scenarioIndex.find(su.id);
    if (idxIt != m_scenarioIndex.end()) {
        m_scenario.units[idxIt->second] = su;
    } else {
        m_scenarioIndex[su.id] = m_scenario.units.size();
        m_scenario.units.push_back(su);
    }

    // Incrementally update runtime units
    auto it = m_units.find(su.id);
    if (it != m_units.end()) {
        const bool runtimeTypeChanged = it->second->kindStr() != su.kind
            || it->second->sideStr() != su.side;
        if (runtimeTypeChanged) {
            m_units.erase(it);
            createSingleUnit(su);
        } else {
            UnitBase::Params p;
            p.detectRange = su.detectRange;
            p.attackRange = su.attackRange;
            p.commRange = su.commRange;
            p.speed = su.speed;
            p.maxHp = su.maxHp;
            p.attackPower = su.attackPower;
            p.armor = su.armor;
            p.repairRate = su.repairRate;
            p.subsystemRepairRate = su.subsystemRepairRate;
            p.pos = su.pos;
            it->second->setCallsign(su.callsign);
            it->second->setParams(p);
            if (auto* attacker = qobject_cast<AttackUAV*>(it->second.get())) {
                attacker->configureWeapon(su);
            }
            it->second->setSchedule(su.schedule);
        }
    } else {
        createSingleUnit(su);
    }

    for (auto& [oid, ou] : m_units) {
        ou->setCpId(commandSenderIdFor(ou.get()));
    }
    if (auto* updated = unit(su.id); updated && updated->alive()) {
        m_destroyedReported.remove(su.id);
    }

    emit unitsChanged();
    recomputeReadyForSim();
}

void SimulationEngine::removeUnit(const QString& id) {
    auto idxIt = m_scenarioIndex.find(id);
    if (idxIt != m_scenarioIndex.end()) {
        const size_t idx = idxIt->second;
        m_scenario.units.erase(m_scenario.units.begin() + static_cast<ptrdiff_t>(idx));
        // Rebuild the index since vector indices shift after erase.
        rebuildScenarioIndex();
    }

    m_units.erase(id);
    m_destroyedReported.remove(id);
    for (auto& [oid, unit] : m_units) {
        if (unit) unit->setCpId(commandSenderIdFor(unit.get()));
    }
    emit unitsChanged();
    recomputeReadyForSim();
}

QStringList SimulationEngine::unitIds() const {
    QStringList r;
    for (auto& [id, _] : m_units) r << id;
    return r;
}

void SimulationEngine::setMessageLogEnabled(bool enabled, const QString& path) {
    if (!m_recorder) return;
    if (!m_recorder->setEnabled(enabled, path)) {
        m_lastError = path.isEmpty()
            ? QStringLiteral("消息日志路径不能为空")
            : QStringLiteral("无法打开消息日志: %1").arg(path);
        emit errorOccurred(m_lastError);
    }
}

void SimulationEngine::persistScenario(const QString& path) {
    QString err;
    if (!ScenarioIo::saveToFile(m_scenario, path, &err)) {
        m_lastError = err.isEmpty() ? QStringLiteral("保存场景失败") : err;
        emit errorOccurred(m_lastError);
    }
}

UnitBase* SimulationEngine::unit(const QString& id) const {
    auto it = m_units.find(id);
    if (it == m_units.end()) return nullptr;
    return it->second.get();
}

QJsonArray SimulationEngine::collectPerceptionSnapshot(const QString& forSide) const {
    QJsonArray arr;
    for (const auto& [id, u] : m_units) {
        if (!forSide.isEmpty() && u->sideStr() != forSide) continue;
        arr.append(unitSnapshot(id));
    }
    return arr;
}

QJsonArray SimulationEngine::collectAllUnitsSnapshot() const {
    QJsonArray arr;
    for (const auto& [id, u] : m_units) arr.append(unitSnapshot(id));
    return arr;
}

void SimulationEngine::applyRemoteRuntimeState(const QJsonArray& units, double simTime,
                                               bool running, double speedMul) {
    m_timer.stop();
    const bool runningChangedValue = m_running != running;
    const bool speedChangedValue = m_clock->speedMul() != speedMul;
    m_running = running;
    m_clock->setSimTime(std::max(0.0, simTime));
    m_clock->setSpeedMul(std::max(0.0, speedMul));
    SnapshotCodec::decodeRuntimeUnits(*this, units);
    if (runningChangedValue) emit runningChanged();
    if (speedChangedValue) emit speedMulChanged();
    emit simTimeChanged();
    emit unitsChanged();
}

QJsonArray SimulationEngine::collectCheckpointState() const {
    return SnapshotCodec::encodeCheckpointUnits(*this);
}

bool SimulationEngine::restoreCheckpointState(const QJsonArray& units, double simTime,
                                              bool running, double speedMul,
                                              QString* error) {
    if (error) error->clear();
    if (!std::isfinite(simTime) || simTime < 0.0
        || !std::isfinite(speedMul) || speedMul < 0.0 || speedMul > 8.0) {
        if (error) *error = QStringLiteral("检查点时间或推演速率无效");
        return false;
    }
    const QJsonArray rollbackState = collectCheckpointState();
    const bool wasRunning = m_running;
    m_timer.stop();
    QString restoreError;
    if (!SnapshotCodec::decodeCheckpointUnits(*this, units, &restoreError)) {
        SnapshotCodec::decodeCheckpointUnits(*this, rollbackState, nullptr);
        if (wasRunning) m_timer.start();
        if (error) *error = restoreError;
        return false;
    }
    m_clock->setSimTime(simTime);
    m_clock->setSpeedMul(speedMul);
    m_running = running && m_readyForSim;
    if (m_running) m_timer.start();
    emit runningChanged();
    emit speedMulChanged();
    emit simTimeChanged();
    emit unitsChanged();
    return true;
}

QVariantList SimulationEngine::unitsForView() const {
    QVariantList l;
    for (const auto& [id, u] : m_units) {
        QVariantMap m;
        m["id"] = id;
        m["callsign"] = u->callsign();
        m["kind"] = u->kindStr();
        m["side"] = u->sideStr();
        m["hp"] = u->hp();
        m["maxHp"] = u->maxHp();
        m["alive"] = u->alive();
        m["movable"] = u->movable();
        QVariantList p; p << u->pos().x << u->pos().y << u->pos().alt;
        m["position"] = p;
        m["detectRange"] = u->detectRange();
        m["attackRange"] = u->attackRange();
        m["commRange"] = u->commRange();
        if (auto* attacker = qobject_cast<AttackUAV*>(u.get())) {
            m["ammoRemaining"] = attacker->ammoRemaining();
            m["ammoCapacity"] = attacker->ammoCapacity();
            m["cooldownRemaining"] = attacker->cooldownRemaining();
            m["lastShotOutcome"] = attacker->lastShotOutcome();
            m["fuelRemaining"] = attacker->fuelRemaining();
            m["fuelCapacity"] = attacker->fuelCapacity();
            m["turnaroundProgress"] = attacker->turnaroundProgress();
        }
        l.append(m);
    }
    return l;
}

void SimulationEngine::appendTimeline(const QString& category, const QString& title,
                                      const QJsonObject& details,
                                      const QString& level) {
    if (m_replaying) return;
    QJsonObject event{{QStringLiteral("sequence"), ++m_timelineSequence},
                      {QStringLiteral("simTime"), simTime()},
                      {QStringLiteral("category"), category},
                      {QStringLiteral("title"), title},
                      {QStringLiteral("level"), level},
                      {QStringLiteral("details"), details}};
    m_timeline.append(event);
    constexpr qsizetype kMaxTimelineEvents = 20000;
    while (m_timeline.size() > kMaxTimelineEvents) m_timeline.removeFirst();
    emit timelineChanged();
}

void SimulationEngine::captureReplayCheckpoint() {
    if (m_replaying) return;
    m_replayCheckpoints.push_back(ReplayCheckpoint{
        simTime(), static_cast<qsizetype>(m_replayCommands.size()), collectCheckpointState()});
    m_lastReplayCheckpointTime = simTime();
    constexpr size_t kMaxReplayCheckpoints = 720;
    if (m_replayCheckpoints.size() > kMaxReplayCheckpoints) {
        m_replayCheckpoints.erase(m_replayCheckpoints.begin() + 1);
    }
}

double SimulationEngine::replayDuration() const {
    return std::max(m_recordedDuration,
                    m_replayCommands.empty() ? 0.0 : m_replayCommands.back().time);
}

bool SimulationEngine::seekReplay(double targetTime, QString* error) {
    if (error) error->clear();
    if (!std::isfinite(targetTime) || targetTime < 0.0
        || targetTime > replayDuration() + 1e-6) {
        if (error) *error = QStringLiteral("回放时间超出可用范围");
        return false;
    }
    if (m_replayInitialScenario.units.empty() || m_replayInitialSeed == 0) {
        if (error) *error = QStringLiteral("当前推演没有可用回放基线");
        return false;
    }

    const auto commands = m_replayCommands;
    const auto checkpoints = m_replayCheckpoints;
    const QJsonArray timeline = m_timeline;
    const qint64 timelineSequence = m_timelineSequence;
    const Scenario initialScenario = m_replayInitialScenario;
    const quint64 initialSeed = m_replayInitialSeed;
    const double duration = m_recordedDuration;
    const QJsonArray finalSnapshot = m_recordedFinalSnapshot;
    const double previousSpeed = speedMul();

    const ReplayCheckpoint* selected = nullptr;
    for (const ReplayCheckpoint& checkpoint : checkpoints) {
        if (checkpoint.time <= targetTime + 1e-9
            && (!selected || checkpoint.time > selected->time)) {
            selected = &checkpoint;
        }
    }

    m_replaying = true;
    setRunning(false);
    if (!setScenario(initialScenario)) {
        m_replaying = false;
        if (error) *error = QStringLiteral("无法重建回放初始场景");
        return false;
    }
    restoreCombatSeed(initialSeed);
    qsizetype commandIndex = 0;
    if (selected && selected->time > 0.0) {
        QString restoreError;
        if (!restoreCheckpointState(selected->state, selected->time, false,
                                    previousSpeed, &restoreError)) {
            m_replaying = false;
            if (error) *error = QStringLiteral("回放检查点恢复失败: %1").arg(restoreError);
            return false;
        }
        commandIndex = selected->commandCount;
    }

    constexpr double kReplayStep = 0.05;
    while (simTime() < targetTime - 1e-9) {
        while (commandIndex < static_cast<qsizetype>(commands.size())
               && commands[commandIndex].time <= simTime() + 1e-9) {
            executeCommand(commands[commandIndex].action, commands[commandIndex].args);
            ++commandIndex;
        }
        double nextTime = targetTime;
        if (commandIndex < static_cast<qsizetype>(commands.size())) {
            nextTime = std::min(nextTime, commands[commandIndex].time);
        }
        const double step = std::min(kReplayStep, nextTime - simTime());
        if (step <= 1e-9) continue;
        onTickInternal(true, step);
    }
    while (commandIndex < static_cast<qsizetype>(commands.size())
           && commands[commandIndex].time <= targetTime + 1e-9) {
        executeCommand(commands[commandIndex].action, commands[commandIndex].args);
        ++commandIndex;
    }

    m_replaying = false;
    m_replayCommands = commands;
    m_replayCheckpoints = checkpoints;
    m_replayInitialScenario = initialScenario;
    m_replayInitialSeed = initialSeed;
    m_timeline = timeline;
    m_timelineSequence = timelineSequence;
    m_recordedDuration = duration;
    m_recordedFinalSnapshot = finalSnapshot;
    m_clock->setSpeedMul(previousSpeed);
    emit runningChanged();
    emit speedMulChanged();
    emit simTimeChanged();
    emit unitsChanged();
    emit timelineChanged();
    return true;
}

QJsonObject SimulationEngine::battleReport() const {
    int shots = 0;
    int hits = 0;
    int kills = 0;
    double damage = 0.0;
    for (const QJsonValue& value : m_timeline) {
        const QJsonObject event = value.toObject();
        if (event.value(QStringLiteral("category")).toString() != QLatin1String("combat")) continue;
        ++shots;
        const QJsonObject details = event.value(QStringLiteral("details")).toObject();
        if (details.value(QStringLiteral("hit")).toBool()) ++hits;
        if (details.value(QStringLiteral("kill")).toBool()) ++kills;
        damage += details.value(QStringLiteral("damage")).toDouble();
    }
    int redLosses = 0;
    int blueLosses = 0;
    for (const QJsonValue& value : m_recordedFinalSnapshot) {
        const QJsonObject unit = value.toObject();
        if (unit.value(QStringLiteral("alive")).toBool()) continue;
        if (unit.value(QStringLiteral("side")).toString() == QLatin1String("red")) ++redLosses;
        else if (unit.value(QStringLiteral("side")).toString() == QLatin1String("blue")) ++blueLosses;
    }
    return {{QStringLiteral("schemaVersion"), 1},
            {QStringLiteral("duration"), replayDuration()},
            {QStringLiteral("battleSeed"), QString::number(m_replayInitialSeed, 16)},
            {QStringLiteral("summary"),
             QJsonObject{{QStringLiteral("shots"), shots},
                         {QStringLiteral("hits"), hits},
                         {QStringLiteral("kills"), kills},
                         {QStringLiteral("damage"), damage},
                         {QStringLiteral("redLosses"), redLosses},
                         {QStringLiteral("blueLosses"), blueLosses}}},
            {QStringLiteral("scenario"), ScenarioIo::toJson(m_replayInitialScenario)},
            {QStringLiteral("finalUnits"), m_recordedFinalSnapshot},
            {QStringLiteral("events"), m_timeline}};
}

void SimulationEngine::checkWinLoseCondition() {
    if (m_outcomeReported) return;
    bool redCpAlive = false, blueCpAlive = false;
    for (const auto& [id, u] : m_units) {
        if (u->kind() != UnitKind::CommandPost || !u->alive()) continue;
        if (u->sideStr() == "red") redCpAlive = true;
        if (u->sideStr() == "blue") blueCpAlive = true;
    }
    if (!redCpAlive || !blueCpAlive) {
        m_outcomeReported = true;
        QString winner, loser;
        if (!redCpAlive && !blueCpAlive) {
            winner = "平局——双方指挥所均被摧毁";
            loser = "";
        } else if (!redCpAlive) {
            winner = "蓝方"; loser = "红方";
        } else {
            winner = "红方"; loser = "蓝方";
        }
        setRunning(false);
        appendTimeline(QStringLiteral("outcome"), QStringLiteral("推演结束"),
                       QJsonObject{{QStringLiteral("winner"), winner},
                                   {QStringLiteral("loser"), loser}},
                       QStringLiteral("warn"));
        emit simulationEnded(winner, loser);
    }
}

} // namespace gbr
