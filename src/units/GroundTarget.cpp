#include "GroundTarget.h"

namespace gbr {

GroundTarget::GroundTarget(const QString& id, Side side, MessageBus* bus, QObject* parent)
    : UnitBase(id, UnitKind::GroundTarget, side, bus, parent) {
    setStatus(QStringLiteral("托管目标"));
}

void GroundTarget::onTick(double simSeconds) {
    Q_UNUSED(simSeconds);
    // Targets are static and have no autonomous behavior. Damage remains
    // authoritative in SimulationEngine's combat resolver.
}

} // namespace gbr
