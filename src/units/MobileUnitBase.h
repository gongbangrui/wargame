#pragma once

#include "../core/UnitBase.h"
#include "../core/UnitFsm.h"
#include <QVariantList>
#include <QPointF>
#include <cmath>

namespace gbr {

/// @brief Base class for mobile units (ReconUAV, JammerUAV, GroundScout)
/// @details Provides shared FSM setup, step-motion logic, and common message
/// handlers for Guidance and Withdraw.
class MobileUnitBase : public UnitBase {
    Q_OBJECT
public:
    MobileUnitBase(const QString& id, UnitKind kind, Side side, MessageBus* bus,
                   QObject* parent = nullptr);

    void onTick(double dt) override;
    void cancelWaypointMotion() override;

protected:
    void setupMobileFsm(const QString& movingState, const char* movingStatus,
                        const char* idleStatus);
    void stepMotion(QVariantList& waypoints, int& idx, double dt,
                    double snapThreshold = 50.0);
    void onMobileMessage(const Message& m);

    QVariantList m_waypoints;
    int m_wpIdx = 0;
    UnitFsm m_fsm;
    QString m_movingState;
    bool m_loopWaypoints = false;
};

} // namespace gbr
