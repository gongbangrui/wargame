#include "LockedStepClock.h"

namespace gbr {

LockedStepClock::LockedStepClock(QObject* parent) : QObject(parent) {}

double LockedStepClock::simTime() const {
    QMutexLocker lock(&m_mutex);
    return m_simTime;
}

double LockedStepClock::speedMul() const {
    QMutexLocker lock(&m_mutex);
    return m_speedMul;
}

void LockedStepClock::setSpeedMul(double m) {
    QMutexLocker lock(&m_mutex);
    if (m_speedMul == m) return;
    m_speedMul = m;
}

void LockedStepClock::setSimTime(double seconds) {
    QMutexLocker lock(&m_mutex);
    m_simTime = seconds;
}

void LockedStepClock::advance(double dt) {
    // Direct advance (used by tests). For network mode prefer step() which
    // also emits the barrier signal.
    double newSimTime = 0.0;
    {
        QMutexLocker lock(&m_mutex);
        m_simTime += dt;
        newSimTime = m_simTime;
    }
    emit stepped(newSimTime, 0);
}

void LockedStepClock::step(double dt, int barrier) {
    double newSimTime = 0.0;
    {
        QMutexLocker lock(&m_mutex);
        m_simTime += dt;
        newSimTime = m_simTime;
    }
    emit stepped(newSimTime, barrier);
}

double LockedStepClock::tickInterval() const {
    // LockedStepClock has no fixed interval; the tick is driven externally.
    return 0.0;
}

} // namespace gbr
