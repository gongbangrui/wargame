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
        return m_waypoints.isEmpty();
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
        cancelWaypointMotion();
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
        }
        return;
    }
    const double stepLen = speed() * dt;
    const double t = std::min(1.0, stepLen / std::max(1.0, dist));
    setPosition(GeoPos{ here.x + dx * t, here.y + dy * t, here.alt });
}

void AttackUAV::stepCombat(double dt) {
    if (m_targetId.isEmpty()) return;
    if (m_cooldown > 0.0) m_cooldown -= dt;
    const double d = distanceToTarget();
    if (d <= attackRange() && m_armed && m_cooldown <= 0.0) {
        const QString engagedTargetId = m_targetId;
        const QString cp = cpId();
        Message r;
        r.type = Message::Type::EngagementReport;
        r.sender = id();
        r.receiver = cp;
        r.payload["targetId"] = engagedTargetId;
        r.payload["distance"] = d;
        r.payload["hit"] = true;
        send(r);

        bool destroyed = false;
        auto* target = findUnit(engagedTargetId);
        if (target && target->alive()) {
            target->setHp(target->hp() - params().attackPower);
            destroyed = !target->alive();
        }

        m_targetId.clear();
        emit targetChanged();
        m_armed = false;
        emit armedChanged();
        m_cooldown = kFireCooldownSec;
        m_waypoints.clear();
        m_wpIdx = 0;
        setHasActiveWaypoints(false);
        // 保留时间表：原 CP 自动 Withdraw 行为已移除，攻击方销毁目标后应留在原位，
        // 由指挥员决定下一步动作；不应抹掉已规划的航路时间表。

        if (destroyed) {
            Message dest;
            dest.type = Message::Type::TargetDestroyed;
            dest.sender = id();
            dest.receiver = cp;
            dest.payload["targetId"] = engagedTargetId;
            dest.payload["attackerId"] = id();
            send(dest);
            emit notifyEvent("目标摧毁", QStringLiteral("%1 摧毁 %2").arg(id(), engagedTargetId), "success");
        }
    }
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
            {QStringLiteral("cooldown"), m_cooldown}};
}

bool AttackUAV::restoreBehaviorCheckpoint(const QJsonObject& state, QString* error) {
    if (error) error->clear();
    const QString fsmState = state.value(QStringLiteral("fsmState")).toString(QStringLiteral("idle"));
    const QSet<QString> knownStates{QStringLiteral("idle"), QStringLiteral("moving"),
                                    QStringLiteral("in_position"), QStringLiteral("withdrawing")};
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
    if ((!waypoints.isEmpty() && (waypointIndex < 0 || waypointIndex >= waypoints.size()))
        || (waypoints.isEmpty() && waypointIndex != 0) || !std::isfinite(cooldown)
        || cooldown < 0.0) {
        if (error) *error = QStringLiteral("攻击无人机运行态无效: %1").arg(id());
        return false;
    }
    m_waypoints = waypoints;
    m_wpIdx = waypointIndex;
    m_targetId = state.value(QStringLiteral("targetId")).toString();
    m_armed = state.value(QStringLiteral("armed")).toBool();
    m_cooldown = cooldown;
    m_fsm.goTo(fsmState);
    setHasActiveWaypoints(!m_waypoints.isEmpty());
    emit targetChanged();
    emit armedChanged();
    return true;
}

} // namespace gbr
