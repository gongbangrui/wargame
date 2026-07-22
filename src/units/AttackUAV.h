#pragma once

#include "../core/UnitBase.h"
#include "../core/Geo.h"
#include "../core/UnitFsm.h"
#include "../core/CombatResolver.h"
#include <QVariantList>
#include <optional>
#include <vector>

namespace gbr {

class AttackUAV : public UnitBase {
    Q_OBJECT
    Q_PROPERTY(QString targetId READ targetId NOTIFY targetChanged)
    Q_PROPERTY(double distanceToTarget READ distanceToTarget NOTIFY targetChanged)
    Q_PROPERTY(bool armed READ armed NOTIFY armedChanged)
    Q_PROPERTY(int ammoRemaining READ ammoRemaining NOTIFY weaponStateChanged)
    Q_PROPERTY(int ammoCapacity READ ammoCapacity NOTIFY weaponStateChanged)
    Q_PROPERTY(double cooldownRemaining READ cooldownRemaining NOTIFY weaponStateChanged)
    Q_PROPERTY(QString lastShotOutcome READ lastShotOutcome NOTIFY weaponStateChanged)
    Q_PROPERTY(double fuelRemaining READ fuelRemaining NOTIFY weaponStateChanged)
    Q_PROPERTY(double fuelCapacity READ fuelCapacity NOTIFY weaponStateChanged)
    Q_PROPERTY(double serviceProgress READ turnaroundProgress NOTIFY weaponStateChanged)
    Q_PROPERTY(QString rulesOfEngagement READ rulesOfEngagement NOTIFY weaponStateChanged)
public:
    explicit AttackUAV(const QString& id, Side side, MessageBus* bus, QObject* parent = nullptr);

    void onTick(double dt) override;
    void setParams(const Params& p) override;
    void setAttackPower(double value) override;
    void cancelWaypointMotion() override;

    QString targetId() const { return m_targetId; }
    double distanceToTarget() const;
    bool armed() const { return m_armed; }
    int ammoRemaining() const { return m_ammoRemaining; }
    int ammoCapacity() const { return m_ammoCapacity; }
    double cooldownRemaining() const { return m_cooldown; }
    QString lastShotOutcome() const { return m_lastShotOutcome; }
    double fuelRemaining() const { return m_fuelRemaining; }
    double fuelCapacity() const { return m_fuelCapacity; }
    double turnaroundProgress() const;
    double turnaroundElapsed() const { return m_turnaroundElapsed; }
    QString rulesOfEngagement() const { return m_rulesOfEngagement; }
    quint64 shotSequence() const { return m_shotSequence; }

    void configureWeapon(const ScenarioUnit& unit);
    std::optional<CombatRequest> takePendingShot();
    void applyCombatOutcome(const CombatOutcome& outcome, bool killCredit);
    bool restoreRuntimeWeaponState(int ammoRemaining, double cooldown,
                                   const QString& lastOutcome,
                                   double fuelRemaining = -1.0,
                                   double turnaroundElapsed = 0.0);
    bool serviceTick(double dt) override;
    void cancelEngagement();
    void setRulesOfEngagement(const QString& value);

    void fireOnTarget(const QString& targetId);

protected:
    void onMessage(const Message& m) override;
    QJsonObject behaviorCheckpoint() const override;
    bool restoreBehaviorCheckpoint(const QJsonObject& state, QString* error) override;

signals:
    void targetChanged();
    void armedChanged();
    void weaponStateChanged();

private:
    void setupFsm();
    void stepMotion(double dt);
    void stepCombat(double dt);
    void beginReturnForService(const QString& reason);

    QString m_targetId;
    QVariantList m_waypoints;
    int m_wpIdx = 0;
    bool m_armed = false;
    double m_cooldown = 0.0;
    int m_ammoCapacity = 4;
    int m_ammoRemaining = 4;
    double m_hitProbability = 1.0;
    double m_optimalRange = 1500.0;
    double m_minAttackRange = 0.0;
    double m_cooldownSec = 1.0;
    double m_damageMin = 100.0;
    double m_damageMax = 100.0;
    double m_rangeFalloff = 0.0;
    quint64 m_shotSequence = 0;
    QString m_lastShotOutcome;
    double m_fuelCapacity = 1800.0;
    double m_fuelRemaining = 1800.0;
    double m_rearmDurationSec = 12.0;
    double m_turnaroundElapsed = 0.0;
    QString m_rulesOfEngagement = QStringLiteral("free");
    std::optional<CombatRequest> m_pendingShot;
    UnitFsm m_fsm;
};

} // namespace gbr
