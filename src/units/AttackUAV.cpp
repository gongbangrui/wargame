#include <QJsonArray>
#include "AttackUAV.h"

#include <QSet>

#include <cmath>
#include <algorithm>

namespace gbr {

AttackUAV::AttackUAV(const QString& id, Side side, MessageBus* bus, QObject* parent)
    : UnitBase(id, UnitKind::AttackUAV, side, bus, parent) {
    setStatus("待命");
    setupFsm();
}

void AttackUAV::setParams(const Params& p) {
    const double previousPower = attackPower();
    const bool followsLegacyPower = m_damageMin == previousPower && m_damageMax == previousPower;
    UnitBase::setParams(p);
    if (followsLegacyPower) {
        m_damageMin = p.attackPower;
        m_damageMax = p.attackPower;
    }
}

void AttackUAV::setAttackPower(double value) {
    const double previousPower = attackPower();
    const bool followsLegacyPower = m_damageMin == previousPower && m_damageMax == previousPower;
    UnitBase::setAttackPower(value);
    if (followsLegacyPower && std::isfinite(value) && value >= 0.0) {
        m_damageMin = value;
        m_damageMax = value;
        emit weaponStateChanged();
    }
}

void AttackUAV::configureWeapon(const ScenarioUnit& unit) {
    m_ammoCapacity = std::max(0, unit.ammoCapacity);
    m_ammoRemaining = std::clamp(unit.initialAmmo, 0, m_ammoCapacity);
    m_hitProbability = std::clamp(unit.hitProbability, 0.0, 1.0);
    m_optimalRange = std::clamp(unit.optimalRange, unit.minAttackRange, unit.attackRange);
    m_minAttackRange = std::max(0.0, unit.minAttackRange);
    m_cooldownSec = std::max(0.0, unit.cooldownSec);
    m_damageMin = std::max(0.0, unit.damageMin);
    m_damageMax = std::max(m_damageMin, unit.damageMax);
    m_rangeFalloff = std::max(0.0, unit.rangeFalloff);
    m_fuelCapacity = std::max(1.0, unit.fuelCapacitySec);
    m_fuelRemaining = std::clamp(unit.initialFuelSec, 0.0, m_fuelCapacity);
    m_rearmDurationSec = std::max(0.0, unit.rearmDurationSec);
    m_turnaroundElapsed = 0.0;
    m_cooldown = 0.0;
    m_shotSequence = 0;
    m_lastShotOutcome.clear();
    m_pendingShot.reset();
    emit weaponStateChanged();
}

void AttackUAV::setupFsm() {
    m_fsm.addState("idle", [this](double) {
    }, [this]{ setHasActiveWaypoints(false); setStatus("待命"); });

    m_fsm.addState("moving", [this](double dt){ stepMotion(dt); },
    [this]{ setHasActiveWaypoints(true); });

    // Transition from moving → idle when waypoints exhausted and no target
    m_fsm.addTransition("moving", "idle", [this]{
        return m_waypoints.isEmpty() && m_targetId.isEmpty();
    });
    // Transition from moving → in_position when at target area with a target
    m_fsm.addTransition("moving", "in_position", [this]{
        return m_waypoints.isEmpty() && !m_targetId.isEmpty();
    });

    m_fsm.addState("in_position", [this](double) {
        // 持续检查目标是否在攻击范围内，超出则重新追击
    }, [this]{
        setHasActiveWaypoints(false);
        setStatus(QStringLiteral("已抵达攻击阵位，目标 %1（持续监控）").arg(m_targetId));
    });
    // Transition back to moving when target moves away
    m_fsm.addTransition("in_position", "moving", [this]{
        if (m_targetId.isEmpty()) return false;
        double d = distanceToTarget();
        return d > attackRange() * 1.2 && d < std::numeric_limits<double>::max();
    });
    // Transition to idle if target cleared
    m_fsm.addTransition("in_position", "idle", [this]{
        return m_targetId.isEmpty();
    });

    m_fsm.addState("withdrawing", [this](double dt){ stepMotion(dt); },
    [this]{ setHasActiveWaypoints(true); setStatus("撤离至指挥所"); });
    // Transition from withdrawing → idle when arrived
    m_fsm.addTransition("withdrawing", "idle", [this]{
        return m_waypoints.isEmpty() && !serviceRequested();
    });
    m_fsm.addTransition("withdrawing", "servicing", [this]{
        return m_waypoints.isEmpty() && serviceRequested();
    });

    m_fsm.addState("servicing", [this](double dt) {
        serviceTick(dt);
        setStatus(QStringLiteral("补给维修中 %1%")
                      .arg(qRound(turnaroundProgress() * 100.0)));
    }, [this]{
        setHasActiveWaypoints(false);
        m_turnaroundElapsed = 0.0;
        setStatus("开始补给与检修");
    });
    m_fsm.addTransition("servicing", "idle", [this]{
        return !serviceRequested();
    });

    m_fsm.setInitialState("idle");
}

void AttackUAV::onTick(double dt) {
    if (!alive()) { setStatus("已摧毁"); return; }
    if (!m_targetId.isEmpty()) {
        auto* target = findUnit(m_targetId);
        if (!target || !target->alive()) {
            m_targetId.clear();
            emit targetChanged();
            if (m_armed) {
                m_armed = false;
                emit armedChanged();
            }
            cancelWaypointMotion();
        }
    }
    const QString stateBeforeTick = m_fsm.currentState();
    if (stateBeforeTick != QLatin1String("idle")
        && stateBeforeTick != QLatin1String("servicing")) {
        m_fuelRemaining = std::max(0.0, m_fuelRemaining - dt);
        emit weaponStateChanged();
    }
    if (m_fuelRemaining <= m_fuelCapacity * 0.2
        && stateBeforeTick != QLatin1String("withdrawing")
        && stateBeforeTick != QLatin1String("servicing")) {
        beginReturnForService(QStringLiteral("燃油低于 20%"));
    }
    m_fsm.tick(dt);
    stepCombat(dt);
    if (!m_targetId.isEmpty()) {
        emit attackProgress(distanceToTarget(), attackRange());
    }
}

void AttackUAV::cancelWaypointMotion() {
    m_waypoints.clear();
    m_wpIdx = 0;
    setHasActiveWaypoints(false);
    requestService(false);
    m_fsm.goTo("idle");
}

void AttackUAV::stepMotion(double dt) {
    if (!m_targetId.isEmpty() && m_waypoints.isEmpty()) {
        auto* target = findUnit(m_targetId);
        if (target && target->alive()) {
            m_waypoints.append(QVariant::fromValue(target->pos().toPointF()));
            m_wpIdx = 0;
            setHasActiveWaypoints(true);
        }
    }
    if (!m_targetId.isEmpty() && !m_waypoints.isEmpty() && m_wpIdx == (int)m_waypoints.size() - 1) {
        auto* tgt = findUnit(m_targetId);
        if (tgt && tgt->alive()) {
            const QPointF tpos = tgt->pos().toPointF();
            m_waypoints[m_wpIdx] = QVariant::fromValue(tpos);
        }
    }
    if (m_waypoints.isEmpty()) return;
    if (m_wpIdx < 0 || m_wpIdx >= m_waypoints.size()) {
        m_waypoints.clear();
        m_wpIdx = 0;
        setHasActiveWaypoints(false);
        m_fsm.goTo(serviceRequested() ? QStringLiteral("servicing")
                                      : QStringLiteral("idle"));
        return;
    }
    const auto target = m_waypoints[m_wpIdx].toPointF();
    GeoPos here = pos();
    const double dx = target.x() - here.x;
    const double dy = target.y() - here.y;
    const double dist = std::sqrt(dx*dx + dy*dy);
    if (dist < 50.0) {
        setPosition(GeoPos{target.x(), target.y(), here.alt});
        if (m_wpIdx + 1 < m_waypoints.size()) {
            m_wpIdx++;
            setStatus(QStringLiteral("沿航路点 %1/%2 飞行").arg(m_wpIdx+1).arg(m_waypoints.size()));
        } else {
            // Exhausted imperative waypoints. Preserve the stored schedule;
            // applySchedules takes ownership again on the next engine tick.
            m_waypoints.clear();
            m_wpIdx = 0;
            setHasActiveWaypoints(false);
            if (m_fsm.currentState() == QLatin1String("withdrawing")) {
                m_fsm.goTo(serviceRequested() ? QStringLiteral("servicing")
                                              : QStringLiteral("idle"));
            }
        }
        return;
    }
    const double stepLen = speed() * dt;
    const double t = std::min(1.0, stepLen / std::max(1.0, dist));
    setPosition(GeoPos{ here.x + dx * t, here.y + dy * t, here.alt });
}

void AttackUAV::stepCombat(double dt) {
    if (m_cooldown > 0.0) {
        m_cooldown = std::max(0.0, m_cooldown - dt);
        emit weaponStateChanged();
    }
    if (m_targetId.isEmpty() || m_pendingShot.has_value()) return;
    if (m_ammoRemaining <= 0) {
        m_targetId.clear();
        m_armed = false;
        beginReturnForService(QStringLiteral("弹药耗尽"));
        emit targetChanged();
        emit armedChanged();
        emit weaponStateChanged();
        return;
    }
    const double d = distanceToTarget();
    if (!m_armed || m_rulesOfEngagement == QLatin1String("hold")
        || weaponHealth() <= 0.05 || m_cooldown > 0.0 || d > attackRange()) return;
    if (d < m_minAttackRange) {
        setStatus(QStringLiteral("目标距离过近，无法射击 %1").arg(m_targetId));
        return;
    }

    CombatRequest request;
    request.attackerId = id();
    request.targetId = m_targetId;
    request.shotSequence = m_shotSequence++;
    request.distance = d;
    request.attackerEffectiveness = jamFactor() * weaponEffectiveness();
    request.weapon = WeaponProfile{m_hitProbability, m_minAttackRange, m_optimalRange,
                                   attackRange(), m_damageMin, m_damageMax, m_rangeFalloff};
    m_pendingShot = request;
    --m_ammoRemaining;
    m_cooldown = m_cooldownSec;
    setStatus(QStringLiteral("交战中：%1（剩余弹药 %2）").arg(m_targetId).arg(m_ammoRemaining));
    emit weaponStateChanged();
}

std::optional<CombatRequest> AttackUAV::takePendingShot() {
    std::optional<CombatRequest> request = std::move(m_pendingShot);
    m_pendingShot.reset();
    return request;
}

void AttackUAV::applyCombatOutcome(const CombatOutcome& outcome, bool killCredit) {
    if (outcome.attackerId != id()) return;
    m_lastShotOutcome = outcome.result;

    Message report;
    report.type = Message::Type::EngagementReport;
    report.sender = id();
    report.receiver = cpId();
    report.payload["shotId"] = outcome.shotId;
    report.payload["targetId"] = outcome.targetId;
    report.payload["distance"] = outcome.distance;
    report.payload["outcome"] = outcome.result;
    report.payload["hit"] = outcome.hit();
    report.payload["probability"] = outcome.effectiveProbability;
    report.payload["roll"] = outcome.roll;
    report.payload["damage"] = outcome.damage;
    report.payload["hpBefore"] = outcome.hpBefore;
    report.payload["hpAfter"] = outcome.hpAfter;
    report.payload["ammoRemaining"] = m_ammoRemaining;
    report.payload["cooldownRemaining"] = m_cooldown;
    send(report);

    UnitBase* target = findUnit(outcome.targetId);
    const bool targetGone = !target || !target->alive();
    const bool ammoExhausted = m_ammoRemaining <= 0;
    if (targetGone || ammoExhausted) {
        m_targetId.clear();
        m_armed = false;
        m_waypoints.clear();
        m_wpIdx = 0;
        setHasActiveWaypoints(false);
        emit targetChanged();
        emit armedChanged();
    }

    if (killCredit) {
        Message destroyed;
        destroyed.type = Message::Type::TargetDestroyed;
        destroyed.sender = id();
        destroyed.receiver = cpId();
        destroyed.payload["targetId"] = outcome.targetId;
        destroyed.payload["attackerId"] = id();
        destroyed.payload["shotId"] = outcome.shotId;
        send(destroyed);
        setStatus(QStringLiteral("目标 %1 已摧毁").arg(outcome.targetId));
        emit notifyEvent("目标摧毁", QStringLiteral("%1 摧毁 %2").arg(id(), outcome.targetId), "success");
    } else if (ammoExhausted) {
        beginReturnForService(QStringLiteral("弹药耗尽"));
    } else if (outcome.hit()) {
        setStatus(QStringLiteral("命中 %1，造成 %2 伤害")
                      .arg(outcome.targetId).arg(outcome.damage, 0, 'f', 1));
    } else {
        setStatus(QStringLiteral("未命中 %1，等待再攻击").arg(outcome.targetId));
    }
    emit weaponStateChanged();
}

bool AttackUAV::restoreRuntimeWeaponState(int ammoRemaining, double cooldown,
                                          const QString& lastOutcome,
                                          double fuelRemaining,
                                          double turnaroundElapsed) {
    if (ammoRemaining < 0 || ammoRemaining > m_ammoCapacity
        || !std::isfinite(cooldown) || cooldown < 0.0
        || (fuelRemaining >= 0.0 && (!std::isfinite(fuelRemaining)
                                    || fuelRemaining > m_fuelCapacity))
        || !std::isfinite(turnaroundElapsed) || turnaroundElapsed < 0.0) return false;
    m_ammoRemaining = ammoRemaining;
    m_cooldown = cooldown;
    m_lastShotOutcome = lastOutcome;
    if (fuelRemaining >= 0.0) m_fuelRemaining = fuelRemaining;
    m_turnaroundElapsed = turnaroundElapsed;
    m_pendingShot.reset();
    emit weaponStateChanged();
    return true;
}

double AttackUAV::turnaroundProgress() const {
    const double resourceProgress = m_rearmDurationSec <= 0.0
        ? 1.0 : std::clamp(m_turnaroundElapsed / m_rearmDurationSec, 0.0, 1.0);
    return std::min(resourceProgress, UnitBase::serviceProgress());
}

bool AttackUAV::serviceTick(double dt) {
    requestService(true);
    const bool repaired = UnitBase::serviceTick(dt);
    m_turnaroundElapsed = std::min(m_rearmDurationSec, m_turnaroundElapsed + dt);
    const double fillRatio = m_rearmDurationSec <= 0.0
        ? 1.0 : std::clamp(m_turnaroundElapsed / m_rearmDurationSec, 0.0, 1.0);
    m_fuelRemaining = m_fuelCapacity * fillRatio;
    m_ammoRemaining = static_cast<int>(std::floor(m_ammoCapacity * fillRatio + 1e-9));
    const bool resourcesReady = fillRatio >= 1.0 - 1e-9;
    requestService(!(repaired && resourcesReady));
    emit weaponStateChanged();
    return repaired && resourcesReady;
}

void AttackUAV::cancelEngagement() {
    m_targetId.clear();
    m_armed = false;
    m_pendingShot.reset();
    emit targetChanged();
    emit armedChanged();
    setStatus(QStringLiteral("已取消交战，保持当前任务"));
}

void AttackUAV::setRulesOfEngagement(const QString& value) {
    if (value != QLatin1String("hold") && value != QLatin1String("free")) return;
    m_rulesOfEngagement = value;
    if (value == QLatin1String("hold")) cancelEngagement();
    emit weaponStateChanged();
}

void AttackUAV::beginReturnForService(const QString& reason) {
    if (m_fsm.currentState() == QLatin1String("withdrawing")
        || m_fsm.currentState() == QLatin1String("servicing")) return;
    UnitBase* cp = findUnit(cpId());
    if (!cp || !cp->alive()) {
        cancelEngagement();
        setStatus(QStringLiteral("%1，指挥所不可用").arg(reason));
        return;
    }
    cancelEngagement();
    requestService(true);
    clearSchedule();
    m_waypoints.clear();
    m_waypoints.append(QVariant::fromValue(cp->pos().toPointF()));
    m_wpIdx = 0;
    m_fsm.goTo("withdrawing");
    setStatus(QStringLiteral("%1，自动返航").arg(reason));
}

double AttackUAV::distanceToTarget() const {
    if (m_targetId.isEmpty()) return std::numeric_limits<double>::infinity();
    auto* tgt = findUnit(m_targetId);
    if (tgt) return pos().distanceTo2D(tgt->pos());
    return std::numeric_limits<double>::infinity();
}

void AttackUAV::fireOnTarget(const QString& targetId) {
    m_targetId = targetId;
    emit targetChanged();
    m_armed = true;
    emit armedChanged();
    setStatus(QStringLiteral("已开火，攻击 %1").arg(targetId));
}

void AttackUAV::onMessage(const Message& m) {
    switch (m.type) {
    case Message::Type::AttackOrder: {
        if (m.payload.value("fireNow").toBool()) {
            fireOnTarget(m.payload.value("targetId").toString());
        } else {
            m_targetId = m.payload.value("targetId").toString();
            emit targetChanged();
            setStatus(QStringLiteral("已分配目标 %1，等待开火指令").arg(m_targetId));
            Message ack;
            ack.type = Message::Type::Ack;
            ack.sender = id();
            ack.receiver = m.sender;
            ack.payload["inReplyTo"] = m.id;
            send(ack);
        }
        break;
    }
    case Message::Type::FlightPlan: {
        const QString payloadTarget = m.payload.value("targetId").toString();
        // Imperative flight plans temporarily own motion but do not erase the
        // stored time schedule; it resumes after the waypoint list completes.
        m_waypoints.clear();
        for (auto v : m.payload.value("waypoints").toArray()) {
            auto o = v.toObject();
            m_waypoints.append(QVariant::fromValue(QPointF(o.value("x").toDouble(), o.value("y").toDouble())));
        }
        m_wpIdx = 0;
        if (!payloadTarget.isEmpty()) {
            m_targetId = payloadTarget;
            m_armed = true;
            emit targetChanged();
            emit armedChanged();
            setStatus(QStringLiteral("接收航路与目标 %1，前往攻击").arg(m_targetId));
        } else if (!m_targetId.isEmpty()) {
            m_armed = true;
            emit armedChanged();
            setStatus(QStringLiteral("接收航路，前往目标 %1").arg(m_targetId));
        } else {
            setStatus(QStringLiteral("接收航路，无目标"));
        }
        m_fsm.goTo("moving");
        break;
    }
    case Message::Type::Guidance: {
        if (m.payload.value("kind").toString() == "moveTo") {
            m_waypoints.clear();
            m_waypoints.append(QVariant::fromValue(QPointF(m.payload.value("x").toDouble(), m.payload.value("y").toDouble())));
            m_wpIdx = 0;
            clearSchedule();
            m_fsm.goTo("moving");
            setStatus(QStringLiteral("引导中 → (%1, %2)")
                      .arg(m.payload.value("x").toDouble(), 0, 'f', 0)
                      .arg(m.payload.value("y").toDouble(), 0, 'f', 0));
        }
        break;
    }
    case Message::Type::Withdraw: {
        requestService(m.payload.value("service").toBool(false));
        m_targetId.clear();
        emit targetChanged();
        m_armed = false;
        emit armedChanged();
        m_waypoints.clear();
        clearSchedule();
        if (m.payload.contains("homeX") && m.payload.contains("homeY")) {
            m_waypoints.append(QVariant::fromValue(QPointF(m.payload.value("homeX").toDouble(),
                                                           m.payload.value("homeY").toDouble())));
            m_wpIdx = 0;
            m_fsm.goTo("withdrawing");
        } else {
            m_fsm.goTo("idle");
        }
        break;
    }
    case Message::Type::SharedDetect: {
        const QString tid = m.payload.value("targetId").toString();
        if (tid.isEmpty()) break;
        QJsonObject info;
        info["targetId"] = tid;
        info["callsign"] = m.payload.value("callsign").toString();
        info["x"] = m.payload.value("x").toDouble();
        info["y"] = m.payload.value("y").toDouble();
        info["alt"] = m.payload.value("alt").toDouble();
        info["distance"] = m.payload.value("distance").toDouble();
        info["reportedBy"] = m.sender;
        rememberShared(QStringLiteral("shared:detect:%1").arg(tid), info);
        break;
    }
    case Message::Type::Pursue: {
        m_targetId = m.payload.value("targetId").toString();
        emit targetChanged();
        m_armed = true; emit armedChanged();
        m_waypoints.clear();
        m_waypoints.append(QVariant::fromValue(
            QPointF(m.payload.value("x").toDouble(),
                    m.payload.value("y").toDouble())));
        m_wpIdx = 0;
        clearSchedule();
        m_fsm.goTo("moving");
        setStatus(QStringLiteral("追击 %1").arg(m_targetId));
        break;
    }
    case Message::Type::Halt: {
        requestService(false);
        m_targetId.clear();
        emit targetChanged();
        m_armed = false;
        emit armedChanged();
        m_waypoints.clear();
        m_wpIdx = 0;
        setHasActiveWaypoints(false);
        clearSchedule();
        m_fsm.goTo("idle");
        setStatus("待命");
        break;
    }
    case Message::Type::CancelEngagement: {
        cancelEngagement();
        break;
    }
    case Message::Type::SetRulesOfEngagement: {
        setRulesOfEngagement(m.payload.value("roe").toString());
        break;
    }
    default: break;
    }
}

QJsonObject AttackUAV::behaviorCheckpoint() const {
    QJsonArray waypoints;
    for (const QVariant& value : m_waypoints) {
        const QPointF point = value.toPointF();
        waypoints.append(QJsonObject{{QStringLiteral("x"), point.x()},
                                     {QStringLiteral("y"), point.y()}});
    }
    return {{QStringLiteral("fsmState"), m_fsm.currentState()},
            {QStringLiteral("waypoints"), waypoints},
            {QStringLiteral("waypointIndex"), m_wpIdx},
            {QStringLiteral("targetId"), m_targetId},
            {QStringLiteral("armed"), m_armed},
            {QStringLiteral("cooldown"), m_cooldown},
            {QStringLiteral("ammoRemaining"), m_ammoRemaining},
            {QStringLiteral("shotSequence"), static_cast<qint64>(m_shotSequence)},
            {QStringLiteral("lastShotOutcome"), m_lastShotOutcome},
            {QStringLiteral("fuelRemaining"), m_fuelRemaining},
            {QStringLiteral("turnaroundElapsed"), m_turnaroundElapsed},
            {QStringLiteral("rulesOfEngagement"), m_rulesOfEngagement}};
}

bool AttackUAV::restoreBehaviorCheckpoint(const QJsonObject& state, QString* error) {
    if (error) error->clear();
    const QString fsmState = state.value(QStringLiteral("fsmState")).toString(QStringLiteral("idle"));
    const QSet<QString> knownStates{QStringLiteral("idle"), QStringLiteral("moving"),
                                    QStringLiteral("in_position"), QStringLiteral("withdrawing"),
                                    QStringLiteral("servicing")};
    if (!knownStates.contains(fsmState)) {
        if (error) *error = QStringLiteral("攻击无人机 FSM 状态无效: %1").arg(id());
        return false;
    }
    QVariantList waypoints;
    for (const QJsonValue& value : state.value(QStringLiteral("waypoints")).toArray()) {
        const QJsonObject object = value.toObject();
        const double x = object.value(QStringLiteral("x")).toDouble();
        const double y = object.value(QStringLiteral("y")).toDouble();
        if (!std::isfinite(x) || !std::isfinite(y)) {
            if (error) *error = QStringLiteral("攻击无人机航点无效: %1").arg(id());
            return false;
        }
        waypoints.append(QVariant::fromValue(QPointF(x, y)));
    }
    const int waypointIndex = state.value(QStringLiteral("waypointIndex")).toInt();
    const double cooldown = state.value(QStringLiteral("cooldown")).toDouble();
    const int ammoRemaining = state.contains(QStringLiteral("ammoRemaining"))
        ? state.value(QStringLiteral("ammoRemaining")).toInt(-1) : m_ammoRemaining;
    const qint64 shotSequence = state.value(QStringLiteral("shotSequence")).toInteger(0);
    const double fuelRemaining = state.value(QStringLiteral("fuelRemaining")).toDouble(m_fuelCapacity);
    const double turnaroundElapsed = state.value(QStringLiteral("turnaroundElapsed")).toDouble(0.0);
    const QString roe = state.value(QStringLiteral("rulesOfEngagement")).toString(QStringLiteral("free"));
    if ((!waypoints.isEmpty() && (waypointIndex < 0 || waypointIndex >= waypoints.size()))
        || (waypoints.isEmpty() && waypointIndex != 0) || !std::isfinite(cooldown)
        || cooldown < 0.0 || ammoRemaining < 0 || ammoRemaining > m_ammoCapacity
        || shotSequence < 0 || !std::isfinite(fuelRemaining) || fuelRemaining < 0.0
        || fuelRemaining > m_fuelCapacity || !std::isfinite(turnaroundElapsed)
        || turnaroundElapsed < 0.0
        || (roe != QLatin1String("hold") && roe != QLatin1String("free"))) {
        if (error) *error = QStringLiteral("攻击无人机运行态无效: %1").arg(id());
        return false;
    }
    m_waypoints = waypoints;
    m_wpIdx = waypointIndex;
    const QString restoredTargetId = state.value(QStringLiteral("targetId")).toString();
    const bool restoredArmed = state.value(QStringLiteral("armed")).toBool();
    UnitBase* restoredTarget = restoredTargetId.isEmpty() ? nullptr : findUnit(restoredTargetId);
    if ((!restoredTargetId.isEmpty()
         && (!restoredTarget || restoredTarget->side() == side()))
        || (restoredArmed && restoredTargetId.isEmpty())) {
        if (error) *error = QStringLiteral("攻击无人机目标状态无效: %1").arg(id());
        return false;
    }
    const QString restoredOutcome = state.value(QStringLiteral("lastShotOutcome")).toString();
    const QSet<QString> knownOutcomes{QString(), QStringLiteral("hit"), QStringLiteral("miss"),
                                      QStringLiteral("out_of_range")};
    if (!knownOutcomes.contains(restoredOutcome)) {
        if (error) *error = QStringLiteral("攻击无人机交战结果无效: %1").arg(id());
        return false;
    }
    m_targetId = restoredTargetId;
    m_armed = restoredArmed;
    m_cooldown = cooldown;
    m_ammoRemaining = ammoRemaining;
    m_shotSequence = static_cast<quint64>(shotSequence);
    m_lastShotOutcome = restoredOutcome;
    m_fuelRemaining = fuelRemaining;
    m_turnaroundElapsed = turnaroundElapsed;
    m_rulesOfEngagement = roe;
    m_pendingShot.reset();
    m_fsm.goTo(fsmState);
    setHasActiveWaypoints(!m_waypoints.isEmpty());
    emit targetChanged();
    emit armedChanged();
    emit weaponStateChanged();
    return true;
}

} // namespace gbr
