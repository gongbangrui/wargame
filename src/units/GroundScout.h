#pragma once

#include "MobileUnitBase.h"

namespace gbr {

class GroundScout : public MobileUnitBase {
    Q_OBJECT
public:
    explicit GroundScout(const QString& id, Side side, MessageBus* bus, QObject* parent = nullptr);

    Q_INVOKABLE void setRoute(const QVariantList& waypoints);
    Q_INVOKABLE void guideAttack(const QString& attackerId, const QString& targetId, const QPointF& lastSeen);

protected:
    void onMessage(const Message& m) override;

private:
    void setupFsm();
};

} // namespace gbr
