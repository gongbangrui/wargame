#pragma once

#include "MobileUnitBase.h"

namespace gbr {

class ReconUAV : public MobileUnitBase {
    Q_OBJECT
public:
    explicit ReconUAV(const QString& id, Side side, MessageBus* bus, QObject* parent = nullptr);

    Q_INVOKABLE void setPatrol(const QVariantList& waypoints);
    Q_INVOKABLE void clearPatrol();

protected:
    void onMessage(const Message& m) override;

private:
    void setupFsm();
};

} // namespace gbr
