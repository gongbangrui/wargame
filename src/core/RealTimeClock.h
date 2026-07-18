#pragma once

#include "IClock.h"

#include <algorithm>

namespace gbr {

/// @brief Default clock: real-time ticks at 50ms with speedMul scaling.
/// @details Behaviour-identical to the original pre-refactor SimulationEngine
/// timer. speedMul = 0 pauses; speedMul = 1 advances 0.05s per real-time tick.
class RealTimeClock : public IClock {
public:
    double simTime() const override { return m_simTime; }
    double speedMul() const override { return m_speedMul; }
    void setSpeedMul(double m) override { m_speedMul = m; }
    void advance(double dt) override { m_simTime += dt; }
    void reset(double simTime = 0.0) override { m_simTime = std::max(0.0, simTime); }
    double tickInterval() const override { return 0.05; }

private:
    double m_simTime = 0.0;
    double m_speedMul = 1.0;
};

} // namespace gbr
