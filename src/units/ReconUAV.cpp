#include <QJsonArray>
#include "ReconUAV.h"

#include <cmath>
#include <algorithm>

namespace gbr {

ReconUAV::ReconUAV(const QString& id, Side side, MessageBus* bus, QObject* parent)
    : UnitBase(id, UnitKind::ReconUAV, side, bus, parent) {
    setStatus("巡航侦察");
}

void ReconUAV::onTick(double dt) {
    if (!alive()) { setStatus("已摧毁"); return; }
    stepMotion(dt);
}

void ReconUAV::stepMotion(double dt) {
    if (m_patrol.isEmpty()) return;
    const auto target = m_patrol[m_patrolIdx].toPointF();
    GeoPos here = pos();
    const double dx = target.x() - here.x;
    const double dy = target.y() - here.y;
    const double dist = std::sqrt(dx*dx + dy*dy);
    if (dist < 50.0) {
        m_patrolIdx = (m_patrolIdx + 1) % m_patrol.size();
        return;
    }
    const double stepLen = speed() * dt;
    const double t = std::min(1.0, stepLen / std::max(1.0, dist));
    setPosition(GeoPos{ here.x + dx * t, here.y + dy * t, here.alt });
}

void ReconUAV::onMessage(const Message& m) {
    if (m.type == Message::Type::Withdraw) {
        setStatus("撤离中");
        clearPatrol();
    } else if (m.type == Message::Type::Guidance) {
        if (m.payload.value("kind").toString() == "moveTo") {
            m_patrol.clear();
            m_patrol.append(QVariant::fromValue(QPointF(m.payload.value("x").toDouble(), m.payload.value("y").toDouble())));
            m_patrolIdx = 0;
            setStatus("机动到指定点");
        }
    }
}

void ReconUAV::setPatrol(const QVariantList& waypoints) {
    m_patrol = waypoints;
    m_patrolIdx = 0;
    setStatus("巡航侦察");
}

void ReconUAV::clearPatrol() {
    m_patrol.clear();
    m_patrolIdx = 0;
}

} // namespace gbr


