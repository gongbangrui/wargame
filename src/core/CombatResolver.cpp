#include "CombatResolver.h"

#include <QByteArray>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace gbr {
namespace {

quint64 mix64(quint64 value) {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

quint64 hashBytes(quint64 seed, const QByteArray& bytes) {
    quint64 value = seed;
    for (const unsigned char byte : bytes) value = mix64(value ^ byte);
    return value;
}

double unitSample(quint64 value) {
    return static_cast<double>(value >> 11U) * (1.0 / 9007199254740992.0);
}

quint64 shotSeed(const CombatRequest& request, quint64 battleSeed) {
    quint64 value = mix64(battleSeed ^ request.shotSequence);
    value = hashBytes(value, request.attackerId.toUtf8());
    return hashBytes(value, request.targetId.toUtf8());
}

} // namespace

CombatOutcome CombatResolver::resolve(const CombatRequest& request, quint64 battleSeed) {
    CombatOutcome outcome;
    outcome.attackerId = request.attackerId;
    outcome.targetId = request.targetId;
    outcome.shotSequence = request.shotSequence;
    outcome.distance = request.distance;
    outcome.shotId = QStringLiteral("%1:%2").arg(request.attackerId)
                         .arg(request.shotSequence);

    const WeaponProfile& weapon = request.weapon;
    const bool finite = std::isfinite(request.distance)
        && std::isfinite(request.attackerEffectiveness)
        && std::isfinite(weapon.hitProbability)
        && std::isfinite(weapon.minRange) && std::isfinite(weapon.optimalRange)
        && std::isfinite(weapon.maxRange) && std::isfinite(weapon.damageMin)
        && std::isfinite(weapon.damageMax) && std::isfinite(weapon.rangeFalloff);
    if (!finite || request.distance < weapon.minRange || request.distance > weapon.maxRange
        || weapon.maxRange < weapon.minRange || weapon.damageMax < weapon.damageMin) {
        outcome.result = QStringLiteral("out_of_range");
        return outcome;
    }

    double probability = std::clamp(weapon.hitProbability, 0.0, 1.0);
    // A profile configured as 100% is the v1 compatibility mode and remains
    // guaranteed. Probabilistic weapons are degraded by ECM effectiveness.
    if (probability < 1.0) {
        probability *= std::clamp(request.attackerEffectiveness, 0.0, 1.0);
    }
    if (request.distance > weapon.optimalRange && weapon.maxRange > weapon.optimalRange) {
        const double rangeRatio = (request.distance - weapon.optimalRange)
            / (weapon.maxRange - weapon.optimalRange);
        probability *= std::max(0.0, 1.0 - std::max(0.0, weapon.rangeFalloff) * rangeRatio);
    }
    outcome.effectiveProbability = std::clamp(probability, 0.0, 1.0);

    const quint64 seed = shotSeed(request, battleSeed);
    outcome.roll = unitSample(mix64(seed));
    if (outcome.roll >= outcome.effectiveProbability) {
        outcome.result = QStringLiteral("miss");
        return outcome;
    }

    outcome.result = QStringLiteral("hit");
    const double damageSample = unitSample(mix64(seed ^ 0xd1b54a32d192ed03ULL));
    outcome.damage = std::max(0.0, weapon.damageMin
        + (weapon.damageMax - weapon.damageMin) * damageSample);
    return outcome;
}

} // namespace gbr
