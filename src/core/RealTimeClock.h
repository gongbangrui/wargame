#pragma once

#include "IClock.h"

namespace gbr {

/// @brief Default clock: real-time ticks at 50ms with speedMul scaling.
/// @details Behaviour-identical to the original pre-refactor SimulationEngine
/// timer. speedMul = 0 pauses; speedMul = 1 advances 0.05s per real-time tick.
class RealTimeClock : public IClock {
public:
    double simTime() const override { return m_simTime; }
    double speedMul() const override { return m_speedMul; }
    void setSpeedMul(double m) override { m_speedMul = m; }
    void setSimTime(double seconds) override { m_simTime = seconds; }
    void advance(double dt) override { m_simTime += dt; }
    double tickInterval() const override { return 0.05; }

private:
    double m_simTime = 0.0;
    double m_speedMul = 1.0;
};

} // namespace gbr
