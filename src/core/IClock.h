#pragma once

namespace gbr {

/// @brief Abstract simulation clock.
/// @details Single-process mode uses RealTimeClock (50ms tick × speedMul).
/// Future networked mode will inject LockedStepClock that advances only when
/// all peers agree on the step. The engine depends on this interface only.
class IClock {
public:
    virtual ~IClock() = default;
    /// @brief Seconds elapsed since simulation start.
    virtual double simTime() const = 0;
    /// @brief Speed multiplier; ticks advance simTime by 0.05 * speedMul.
    virtual double speedMul() const = 0;
    virtual void setSpeedMul(double m) = 0;
    /// @brief Step simulation by @p dt seconds once (used by stepOnce).
    virtual void advance(double dt) = 0;
    /// @brief Restore an authoritative time from a trusted checkpoint.
    virtual void reset(double simTime = 0.0) = 0;
    /// @brief Real-time tick interval (seconds). Returns 0.05 for the default.
    virtual double tickInterval() const = 0;
};

} // namespace gbr
