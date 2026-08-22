#pragma once

#include "core/UnitBase.h"

namespace gbr {

/// Server-owned non-seat target used by the strict VMF demonstration scenario.
class GroundTarget final : public UnitBase {
public:
    GroundTarget(const QString& id, Side side, MessageBus* bus, QObject* parent = nullptr);

    void onTick(double simSeconds) override;
};

} // namespace gbr
