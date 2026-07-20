#pragma once

#include "../core/UnitBase.h"
#include "../core/Geo.h"
#include "../core/UnitFsm.h"
#include <QVariantList>
#include <vector>

namespace gbr {

class AttackUAV : public UnitBase {
    Q_OBJECT
    Q_PROPERTY(QString targetId READ targetId NOTIFY targetChanged)
    Q_PROPERTY(double distanceToTarget READ distanceToTarget NOTIFY targetChanged)
    Q_PROPERTY(bool armed READ armed NOTIFY armedChanged)
public:
    explicit AttackUAV(const QString& id, Side side, MessageBus* bus, QObject* parent = nullptr);

    void onTick(double dt) override;
    void cancelWaypointMotion() override;

    QString targetId() const { return m_targetId; }
    double distanceToTarget() const;
    bool armed() const { return m_armed; }

    void fireOnTarget(const QString& targetId);

protected:
    void onMessage(const Message& m) override;
    QJsonObject behaviorCheckpoint() const override;
    bool restoreBehaviorCheckpoint(const QJsonObject& state, QString* error) override;

signals:
    void targetChanged();
    void armedChanged();

private:
    void setupFsm();
    void stepMotion(double dt);
    void stepCombat(double dt);

    QString m_targetId;
    QVariantList m_waypoints;
    int m_wpIdx = 0;
    bool m_armed = false;
    double m_cooldown = 0.0;
    UnitFsm m_fsm;

    /// @brief Minimum seconds between successive fires; prevents re-arm spam.
    static constexpr double kFireCooldownSec = 1.0;
};

} // namespace gbr
