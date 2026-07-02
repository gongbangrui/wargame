#pragma once

#include "../core/UnitBase.h"

namespace gbr {

class ReconUAV : public UnitBase {
    Q_OBJECT
public:
    explicit ReconUAV(const QString& id, Side side, MessageBus* bus, QObject* parent = nullptr);

    void onTick(double dt) override;

    Q_INVOKABLE void setPatrol(const QVariantList& waypoints);
    Q_INVOKABLE void clearPatrol();

protected:
    void onMessage(const Message& m) override;

private:
    void stepMotion(double dt);

    QVariantList m_patrol;
    int m_patrolIdx = 0;
};

} // namespace gbr

