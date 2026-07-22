#pragma once

#include <QString>
#include <QtGlobal>

namespace gbr {

struct WeaponProfile {
    double hitProbability = 1.0;
    double minRange = 0.0;
    double optimalRange = 1500.0;
    double maxRange = 1500.0;
    double damageMin = 100.0;
    double damageMax = 100.0;
    double rangeFalloff = 0.0;
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
