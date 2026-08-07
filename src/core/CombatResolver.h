#pragma once

#include <QString>
#include <QtGlobal>

namespace gbr {

struct WeaponProfile {
    double hitProbability = 0.72;
    double minRange = 300.0;
    double optimalRange = 1500.0;
    double maxRange = 2500.0;
    double damageMin = 80.0;
    double damageMax = 110.0;
    double rangeFalloff = 0.25;
};

struct CombatRequest {
    QString attackerId;
    QString targetId;
    quint64 shotSequence = 0;
    double distance = 0.0;
    double attackerEffectiveness = 1.0;
    WeaponProfile weapon;
};

struct CombatOutcome {
    QString shotId;
    QString attackerId;
    QString targetId;
    QString result;
    quint64 shotSequence = 0;
    double distance = 0.0;
    double effectiveProbability = 0.0;
    double roll = 0.0;
    double damage = 0.0;
    double hpBefore = 0.0;
    double hpAfter = 0.0;

    bool hit() const { return result == QStringLiteral("hit"); }
};

class CombatResolver final {
public:
    static CombatOutcome resolve(const CombatRequest& request, quint64 battleSeed);
};

} // namespace gbr
