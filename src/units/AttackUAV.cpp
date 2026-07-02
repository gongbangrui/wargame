#include <QJsonArray>
#include "AttackUAV.h"

#include <cmath>
#include <algorithm>

namespace gbr {

AttackUAV::AttackUAV(const QString& id, Side side, MessageBus* bus, QObject* parent)
    : UnitBase(id, UnitKind::AttackUAV, side, bus, parent) {
    setStatus("待命");
}

void AttackUAV::onTick(double dt) {
    if (!alive()) { setStatus("已摧毁"); return; }
    stepMotion(dt);
    stepCombat(dt);
    if (!m_targetId.isEmpty()) {
        emit attackProgress(distanceToTarget(), attackRange());
    }
}

void AttackUAV::stepMotion(double dt) {
    if (m_waypoints.isEmpty()) return;
    const auto target = m_waypoints[m_wpIdx].toPointF();
    GeoPos here = pos();
    const double dx = target.x() - here.x;
    const double dy = target.y() - here.y;
    const double dist = std::sqrt(dx*dx + dy*dy);
    if (dist < 50.0) {
        if (m_wpIdx + 1 < m_waypoints.size()) {
            m_wpIdx++;
            setStatus(QStringLiteral("沿航路点 %1/%2 飞行").arg(m_wpIdx+1).arg(m_waypoints.size()));
        } else {
            if (!m_targetId.isEmpty()) {
                setStatus(QStringLiteral("已抵达攻击阵位，目标 %1").arg(m_targetId));
            } else {
                setStatus("航路完成，待命");
            }
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
        Message r;
        r.type = Message::Type::EngagementReport;
        r.sender = id();
        r.receiver = sideStr() == "red" ? "red_cp" : "blue_cp";
        r.payload["targetId"] = m_targetId;
        r.payload["distance"] = d;
        send(r);
        r.payload["hit"] = true;
        send(r);
        Message dest;
        dest.type = Message::Type::TargetDestroyed;
        dest.sender = id();
        dest.receiver = sideStr() == "red" ? "red_cp" : "blue_cp";
        dest.payload["targetId"] = m_targetId;
        dest.payload["attackerId"] = id();
        send(dest);
        emit notifyEvent("目标摧毁", QStringLiteral("%1 摧毁 %2").arg(id(), m_targetId), "success");
        m_targetId.clear();
        emit targetChanged();
        m_armed = false;
        emit armedChanged();
        setStatus("攻击完成，待命");
    }
}

double AttackUAV::distanceToTarget() const {
    if (m_targetId.isEmpty()) return std::numeric_limits<double>::infinity();
    for (auto& [oid, opos] : m_others) {
        if (oid == m_targetId) return pos().distanceTo(opos);
    }
    return std::numeric_limits<double>::infinity();
}

void AttackUAV::setOtherPositions(const std::vector<std::pair<QString, GeoPos>>& others) {
    m_others = others;
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
        m_waypoints.clear();
        for (auto v : m.payload.value("waypoints").toArray()) {
            auto o = v.toObject();
            m_waypoints.append(QVariant::fromValue(QPointF(o.value("x").toDouble(), o.value("y").toDouble())));
        }
        m_wpIdx = 0;
        if (!m_targetId.isEmpty()) {
            m_armed = true; // 进入攻击航线即武装
            emit armedChanged();
            setStatus(QStringLiteral("接收航路，前往目标 %1").arg(m_targetId));
        } else {
            setStatus(QStringLiteral("接收航路，无目标"));
        }
        break;
    }
    case Message::Type::Guidance: {
        if (m.payload.value("kind").toString() == "moveTo") {
            m_waypoints.clear();
            m_waypoints.append(QVariant::fromValue(QPointF(m.payload.value("x").toDouble(), m.payload.value("y").toDouble())));
            m_wpIdx = 0;
            setStatus("机动到指定点");
        }
        break;
    }
    case Message::Type::Withdraw: {
        m_waypoints.clear();
        m_targetId.clear();
        emit targetChanged();
        m_armed = false;
        emit armedChanged();
        setStatus("撤离中");
        break;
    }
    default: break;
    }
}

} // namespace gbr


