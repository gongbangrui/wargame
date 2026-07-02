#pragma once

#include "../core/UnitBase.h"
#include <QVariantList>
#include <vector>

namespace gbr {

class GroundScout : public UnitBase {
    Q_OBJECT
public:
    explicit GroundScout(const QString& id, Side side, MessageBus* bus, QObject* parent = nullptr);

    void onTick(double dt) override;

    Q_INVOKABLE void setRoute(const QVariantList& waypoints);
    Q_INVOKABLE void guideAttack(const QString& attackerId, const QString& targetId, const QPointF& lastSeen);

protected:
    void onMessage(const Message& m) override;

private:
    void stepMotion(double dt);

    QVariantList m_route;
    int m_routeIdx = 0;
};

} // namespace gbr

