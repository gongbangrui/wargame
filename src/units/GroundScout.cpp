#include <QJsonArray>
#include "GroundScout.h"

namespace gbr {

GroundScout::GroundScout(const QString& id, Side side, MessageBus* bus, QObject* parent)
    : MobileUnitBase(id, UnitKind::GroundScout, side, bus, parent) {
    setStatus("地面侦察");
    setupFsm();
}

void GroundScout::setupFsm() {
    setupMobileFsm("moving", "地面行进中", "地面侦察");
}

void GroundScout::onMessage(const Message& m) {
    if (m.type == Message::Type::SharedDetect) {
        const QString tid = m.payload.value("targetId").toString();
        if (!tid.isEmpty()) {
            QJsonObject info;
            info["targetId"] = tid;
            info["callsign"] = m.payload.value("callsign").toString();
            info["x"] = m.payload.value("x").toDouble();
            info["y"] = m.payload.value("y").toDouble();
            info["alt"] = m.payload.value("alt").toDouble();
            info["distance"] = m.payload.value("distance").toDouble();
            info["reportedBy"] = m.sender;
            rememberShared(QStringLiteral("shared:detect:%1").arg(tid), info);
        }
    }
    onMobileMessage(m);
}

void GroundScout::setRoute(const QVariantList& waypoints) {
    m_waypoints = waypoints;
    m_wpIdx = 0;
    m_fsm.goTo(m_waypoints.isEmpty() ? QStringLiteral("idle")
                                      : QStringLiteral("moving"));
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
