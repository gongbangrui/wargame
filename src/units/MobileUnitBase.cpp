#include "MobileUnitBase.h"
#include <QJsonArray>

namespace gbr {

MobileUnitBase::MobileUnitBase(const QString& id, UnitKind kind, Side side,
                               MessageBus* bus, QObject* parent)
    : UnitBase(id, kind, side, bus, parent) {}

void MobileUnitBase::onTick(double dt) {
    if (!alive()) { setStatus("已摧毁"); return; }
    m_fsm.tick(dt);
}

void MobileUnitBase::cancelWaypointMotion() {
    m_waypoints.clear();
    m_wpIdx = 0;
    setHasActiveWaypoints(false);
    m_fsm.goTo("idle");
}

void MobileUnitBase::setupMobileFsm(const QString& movingState,
                                     const char* movingStatus,
                                     const char* idleStatus) {
    m_movingState = movingState;
    m_loopWaypoints = (movingState == QLatin1String("patrolling"));
    m_fsm.addState("idle", [](double) {},
                   [this, idleStatus]{
                       setHasActiveWaypoints(false);
                       setStatus(idleStatus);
                   });

    m_fsm.addState(m_movingState, [this](double dt){ stepMotion(m_waypoints, m_wpIdx, dt); },
                   [this, movingStatus]{ setHasActiveWaypoints(true); setStatus(movingStatus); });

    m_fsm.addTransition(movingState, "idle",
                        [this]{ return m_waypoints.isEmpty(); });

    m_fsm.addState("withdrawing", [this](double dt){ stepMotion(m_waypoints, m_wpIdx, dt); },
                   [this]{ setHasActiveWaypoints(true); setStatus("撤离至指挥所"); });

    m_fsm.addTransition("withdrawing", "idle",
                        [this]{ return m_waypoints.isEmpty(); });

    m_fsm.setInitialState("idle");
}

void MobileUnitBase::stepMotion(QVariantList& waypoints, int& idx, double dt,
                                 double snapThreshold) {
    if (waypoints.isEmpty()) return;
    if (idx < 0 || idx >= waypoints.size()) {
        waypoints.clear();
        idx = 0;
        setHasActiveWaypoints(false);
        m_fsm.goTo("idle");
        return;
    }
    const auto target = waypoints[idx].toPointF();
    GeoPos here = pos();
    const double dx = target.x() - here.x;
    const double dy = target.y() - here.y;
    const double dist = std::sqrt(dx * dx + dy * dy);
    if (dist < snapThreshold) {
        setPosition(GeoPos{target.x(), target.y(), here.alt});
        if (idx + 1 < waypoints.size()) {
            ++idx;
            return;
        }
        if (m_loopWaypoints && waypoints.size() > 1) {
            idx = 0;
            return;
        }
        waypoints.clear();
        idx = 0;
        setHasActiveWaypoints(false);
        // The stored schedule, if any, takes ownership on the next engine tick.
        m_fsm.goTo("idle");
        return;
    }
    const double stepLen = speed() * dt;
    const double t = std::min(1.0, stepLen / std::max(1.0, dist));
    setPosition(GeoPos{here.x + dx * t, here.y + dy * t, here.alt});
}

void MobileUnitBase::onMobileMessage(const Message& m) {
    if (m.type == Message::Type::Withdraw) {
        setStatus("撤离中");
        // Withdraw is an abort command, not a temporary waypoint override.
        clearSchedule();
        if (m.payload.contains("homeX") && m.payload.contains("homeY")) {
            m_waypoints.clear();
            m_waypoints.append(QVariant::fromValue(
                QPointF(m.payload.value("homeX").toDouble(),
                        m.payload.value("homeY").toDouble())));
            m_wpIdx = 0;
            m_fsm.goTo("withdrawing");
        } else {
            m_waypoints.clear();
            m_wpIdx = 0;
            m_fsm.goTo("idle");
        }
    } else if (m.type == Message::Type::Guidance) {
        if (m.payload.value("kind").toString() == "moveTo") {
            m_waypoints.clear();
            m_waypoints.append(QVariant::fromValue(
                QPointF(m.payload.value("x").toDouble(),
                        m.payload.value("y").toDouble())));
            m_wpIdx = 0;
            clearSchedule();
            m_fsm.goTo(m_movingState);
            setStatus("机动到指定点");
        }
    } else if (m.type == Message::Type::Halt) {
        m_waypoints.clear();
        m_wpIdx = 0;
        setHasActiveWaypoints(false);
        clearSchedule();
        m_fsm.goTo("idle");
        setStatus("待命");
    }
}

QJsonObject MobileUnitBase::behaviorCheckpoint() const {
    QJsonArray waypoints;
    for (const QVariant& value : m_waypoints) {
        const QPointF point = value.toPointF();
        waypoints.append(QJsonObject{{QStringLiteral("x"), point.x()},
                                     {QStringLiteral("y"), point.y()}});
    }
    return {{QStringLiteral("fsmState"), m_fsm.currentState()},
            {QStringLiteral("waypoints"), waypoints},
            {QStringLiteral("waypointIndex"), m_wpIdx}};
}

bool MobileUnitBase::restoreBehaviorCheckpoint(const QJsonObject& state, QString* error) {
    if (error) error->clear();
    const QString fsmState = state.value(QStringLiteral("fsmState")).toString(QStringLiteral("idle"));
    if (fsmState != QLatin1String("idle") && fsmState != m_movingState
        && fsmState != QLatin1String("withdrawing")) {
        if (error) *error = QStringLiteral("移动单元 FSM 状态无效: %1").arg(id());
        return false;
    }
    QVariantList waypoints;
    for (const QJsonValue& value : state.value(QStringLiteral("waypoints")).toArray()) {
        const QJsonObject object = value.toObject();
        const double x = object.value(QStringLiteral("x")).toDouble();
        const double y = object.value(QStringLiteral("y")).toDouble();
        if (!std::isfinite(x) || !std::isfinite(y)) {
            if (error) *error = QStringLiteral("移动单元航点无效: %1").arg(id());
            return false;
        }
        waypoints.append(QVariant::fromValue(QPointF(x, y)));
    }
    const int waypointIndex = state.value(QStringLiteral("waypointIndex")).toInt();
    if ((!waypoints.isEmpty() && (waypointIndex < 0 || waypointIndex >= waypoints.size()))
        || (waypoints.isEmpty() && waypointIndex != 0)) {
        if (error) *error = QStringLiteral("移动单元航点索引无效: %1").arg(id());
        return false;
    }
    m_waypoints = waypoints;
    m_wpIdx = waypointIndex;
    m_fsm.goTo(fsmState);
    setHasActiveWaypoints(!m_waypoints.isEmpty());
    return true;
}

} // namespace gbr
