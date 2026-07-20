#pragma once

#include "IClock.h"
#include <QObject>
#include <QMutex>
#include <QMutexLocker>
#include <QString>
#include <QWaitCondition>

namespace gbr {

/// @brief 用于确定性检查和回放工具的步进时钟。
/// @details 联网权威服务器使用 RealTimeClock，客户端直接应用服务器时间，
/// 不参与锁步屏障。
class LockedStepClock : public QObject, public IClock {
    Q_OBJECT
public:
    explicit LockedStepClock(QObject* parent = nullptr);

    // IClock
    double simTime() const override;
    double speedMul() const override;
    void setSpeedMul(double m) override;
    void setSimTime(double seconds) override;
    void advance(double dt) override;
    double tickInterval() const override;

    /// @brief Advance the clock by exactly @p dt seconds, gated by the barrier.
    /// @details In single-process deterministic mode this is called once per
    /// received network tick. The barrier argument is reserved for future
    /// lockstep arbitration (round number, peer id, etc.) and is currently
    /// accepted but unused.
    void step(double dt, int barrier = 0);

signals:
    /// @brief Emitted whenever the clock advances; mirrors the @c simTimeChanged
    /// semantics used by the engine's clock observer.
    void stepped(double newSimTime, int barrier);

private:
    mutable QMutex m_mutex;
    double m_simTime = 0.0;
    double m_speedMul = 1.0;
};

} // namespace gbr
