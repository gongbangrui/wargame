#include "JammerUAV.h"

namespace gbr {

JammerUAV::JammerUAV(const QString& id, Side side, MessageBus* bus, QObject* parent)
    : MobileUnitBase(id, UnitKind::JammerUAV, side, bus, parent) {
    setStatus("电子干扰巡航");
    setupFsm();
}

void JammerUAV::setupFsm() {
    setupMobileFsm("patrolling", "电子干扰巡航", "待命");
}

void JammerUAV::onMessage(const Message& m) {
    onMobileMessage(m);
}

void JammerUAV::setPatrol(const QVariantList& waypoints) {
    m_waypoints = waypoints;
    m_wpIdx = 0;
    m_fsm.goTo(m_waypoints.isEmpty() ? QStringLiteral("idle")
                                      : QStringLiteral("patrolling"));
}

void JammerUAV::clearPatrol() {
    m_waypoints.clear();
    m_wpIdx = 0;
    setHasActiveWaypoints(false);
    m_fsm.goTo("idle");
}

} // namespace gbr
