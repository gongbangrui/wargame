#include <gtest/gtest.h>

#include "core/CombatResolver.h"

using namespace gbr;

namespace {

CombatRequest request() {
    CombatRequest value;
    value.attackerId = QStringLiteral("red_a1");
    value.targetId = QStringLiteral("blue_a1");
    value.shotSequence = 7;
    value.distance = 1000.0;
    value.weapon = WeaponProfile{0.65, 0.0, 1000.0, 2000.0, 20.0, 40.0, 0.5};
    return value;
}

} // namespace

TEST(CombatResolverTest, SameShotAndSeedAreReproducible) {
    const CombatOutcome first = CombatResolver::resolve(request(), 12345);
    const CombatOutcome second = CombatResolver::resolve(request(), 12345);
    EXPECT_EQ(first.shotId, second.shotId);
    EXPECT_EQ(first.result, second.result);
    EXPECT_DOUBLE_EQ(first.roll, second.roll);
    EXPECT_DOUBLE_EQ(first.damage, second.damage);
}

TEST(CombatResolverTest, ProbabilityEndpointsAreStable) {
    CombatRequest value = request();
    value.weapon.hitProbability = 0.0;
    EXPECT_EQ(CombatResolver::resolve(value, 1).result, QStringLiteral("miss"));
    value.weapon.hitProbability = 1.0;
    EXPECT_EQ(CombatResolver::resolve(value, 1).result, QStringLiteral("hit"));
}

TEST(CombatResolverTest, RejectsDistancesOutsideWeaponEnvelope) {
    CombatRequest value = request();
    value.weapon.minRange = 500.0;
    value.weapon.maxRange = 1500.0;
    value.distance = 499.0;
    EXPECT_EQ(CombatResolver::resolve(value, 1).result, QStringLiteral("out_of_range"));
    value.distance = 1501.0;
    EXPECT_EQ(CombatResolver::resolve(value, 1).result, QStringLiteral("out_of_range"));
}

TEST(CombatResolverTest, RangeAndEcmReduceProbabilisticAccuracy) {
    CombatRequest near = request();
    near.weapon.hitProbability = 0.8;
    near.attackerEffectiveness = 1.0;
    near.distance = near.weapon.optimalRange;
    CombatRequest far = near;
    far.distance = far.weapon.maxRange;
    far.attackerEffectiveness = 0.5;
    const CombatOutcome nearOutcome = CombatResolver::resolve(near, 5);
    const CombatOutcome farOutcome = CombatResolver::resolve(far, 5);
    EXPECT_DOUBLE_EQ(nearOutcome.effectiveProbability, 0.8);
    EXPECT_DOUBLE_EQ(farOutcome.effectiveProbability, 0.2);
}

TEST(CombatResolverTest, HitDamageStaysWithinConfiguredBounds) {
    CombatRequest value = request();
    value.weapon.hitProbability = 1.0;
    const CombatOutcome outcome = CombatResolver::resolve(value, 7);
    ASSERT_TRUE(outcome.hit());
    EXPECT_GE(outcome.damage, value.weapon.damageMin);
    EXPECT_LE(outcome.damage, value.weapon.damageMax);
}
