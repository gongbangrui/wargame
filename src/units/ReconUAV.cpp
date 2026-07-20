#include "ReconUAV.h"

namespace gbr {

ReconUAV::ReconUAV(const QString& id, Side side, MessageBus* bus, QObject* parent)
    : MobileUnitBase(id, UnitKind::ReconUAV, side, bus, parent) {
    setStatus("巡航侦察");
    setupFsm();
}

void ReconUAV::setupFsm() {
    setupMobileFsm("patrolling", "巡航侦察", "待命");
}

void ReconUAV::onMessage(const Message& m) {
    onMobileMessage(m);
}

void ReconUAV::setPatrol(const QVariantList& waypoints) {
    m_waypoints = waypoints;
    m_wpIdx = 0;
    m_fsm.goTo(m_waypoints.isEmpty() ? QStringLiteral("idle")
                                      : QStringLiteral("patrolling"));
}

void ReconUAV::clearPatrol() {
    m_waypoints.clear();
    m_wpIdx = 0;
    setHasActiveWaypoints(false);
    m_fsm.goTo("idle");
}

} // namespace gbr
