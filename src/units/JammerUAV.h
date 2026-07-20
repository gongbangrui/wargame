#pragma once

#include "MobileUnitBase.h"

namespace gbr {

class JammerUAV : public MobileUnitBase {
    Q_OBJECT
public:
    explicit JammerUAV(const QString& id, Side side, MessageBus* bus, QObject* parent = nullptr);

    Q_INVOKABLE void setPatrol(const QVariantList& waypoints);
    Q_INVOKABLE void clearPatrol();

protected:
    void onMessage(const Message& m) override;

private:
    void setupFsm();
};

} // namespace gbr
