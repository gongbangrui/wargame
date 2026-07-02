#include <QJsonArray>
#include "GroundScout.h"

#include <cmath>
#include <algorithm>

namespace gbr {

GroundScout::GroundScout(const QString& id, Side side, MessageBus* bus, QObject* parent)
    : UnitBase(id, UnitKind::GroundScout, side, bus, parent) {
    setStatus("地面侦察");
}

void GroundScout::onTick(double dt) {
    if (!alive()) { setStatus("已摧毁"); return; }
    stepMotion(dt);
}

void GroundScout::stepMotion(double dt) {
    if (m_route.isEmpty()) return;
    const auto target = m_route[m_routeIdx].toPointF();
    GeoPos here = pos();
    const double dx = target.x() - here.x;
    const double dy = target.y() - here.y;
    const double dist = std::sqrt(dx*dx + dy*dy);
    if (dist < 30.0) {
        if (m_routeIdx + 1 < m_route.size()) {
            m_routeIdx++;
        } else {
            setStatus("到达目标区域，保持监视");
        }
        return;
    }
    const double stepLen = speed() * dt;
    const double t = std::min(1.0, stepLen / std::max(1.0, dist));
    setPosition(GeoPos{ here.x + dx * t, here.y + dy * t, 0.0 });
}

void GroundScout::onMessage(const Message& m) {
    if (m.type == Message::Type::Guidance) {
        if (m.payload.value("kind").toString() == "moveTo") {
            m_route.clear();
            m_route.append(QVariant::fromValue(QPointF(m.payload.value("x").toDouble(), m.payload.value("y").toDouble())));
            m_routeIdx = 0;
            setStatus("机动中");
        }
    } else if (m.type == Message::Type::Withdraw) {
        setStatus("撤离中");
    }
}

void GroundScout::setRoute(const QVariantList& waypoints) {
    m_route = waypoints;
    m_routeIdx = 0;
    setStatus("地面行进中");
}

void GroundScout::guideAttack(const QString& attackerId, const QString& targetId, const QPointF& lastSeen) {
    Message m;
    m.type = Message::Type::FlightPlan;
    m.sender = id();
    m.receiver = attackerId;
    QJsonArray wp;
    QJsonObject w; w["x"] = lastSeen.x(); w["y"] = lastSeen.y(); w["alt"] = 2000.0;
    wp.append(w);
    m.payload["waypoints"] = wp;
    m.payload["targetId"] = targetId;
    send(m);
    setStatus(QStringLiteral("已引导 %1 攻击 %2").arg(attackerId, targetId));
    emit notifyEvent("地面分队引导", QStringLiteral("%1 引导 %2 攻击 %3").arg(id(), attackerId, targetId), "info");
}

} // namespace gbr



