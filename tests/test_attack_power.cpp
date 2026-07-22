#include <gtest/gtest.h>
#include "core/SimulationEngine.h"
#include "core/UnitBase.h"
#include "core/Scenario.h"
#include "units/AttackUAV.h"

#include <cmath>

using namespace gbr;

TEST(AttackPower, ScenarioRoundTrip) {
    ScenarioUnit su;
    su.id = "t1"; su.callsign = "test"; su.kind = "attackuav"; su.side = "red";
    su.pos = GeoPos{0, 0, 2000};
    su.detectRange = 4000; su.attackRange = 2000; su.commRange = 20000;
    su.speed = 100; su.maxHp = 100; su.attackPower = 250;
    su.ammoCapacity = 8; su.initialAmmo = 6; su.hitProbability = 0.75;
    su.minAttackRange = 100; su.optimalRange = 1200; su.cooldownSec = 2.0;
    su.damageMin = 80; su.damageMax = 140; su.rangeFalloff = 0.4;

    Scenario s;
    s.units.push_back(su);
    QJsonObject j = ScenarioIo::toJson(s);
    auto parsed = ScenarioIo::fromJson(j);
    ASSERT_EQ(parsed.units.size(), 1u);
    EXPECT_DOUBLE_EQ(parsed.units[0].attackPower, 250.0);
    EXPECT_EQ(parsed.units[0].ammoCapacity, 8);
    EXPECT_EQ(parsed.units[0].initialAmmo, 6);
    EXPECT_DOUBLE_EQ(parsed.units[0].hitProbability, 0.75);
    EXPECT_DOUBLE_EQ(parsed.units[0].damageMin, 80.0);
    EXPECT_DOUBLE_EQ(parsed.units[0].damageMax, 140.0);
}

TEST(AttackPower, DefaultIs100) {
    ScenarioUnit su;
    su.id = "t1"; su.kind = "attackuav"; su.side = "red";
    su.pos = GeoPos{0, 0, 0};
    // Leave attackPower at default
    EXPECT_DOUBLE_EQ(su.attackPower, 100.0);
}

TEST(AttackPower, ParseMissingDefaultsTo100) {
    QJsonObject j;
    j["map"] = QJsonObject();
    QJsonArray units;
    QJsonObject u;
    u["id"] = "t1"; u["callsign"] = "x"; u["kind"] = "attackuav"; u["side"] = "red";
    u["x"] = 0; u["y"] = 0; u["alt"] = 0;
    u["maxHp"] = 100;
    // no attackPower field
    units.append(u);
    j["units"] = units;
    auto parsed = ScenarioIo::fromJson(j);
    ASSERT_EQ(parsed.units.size(), 1u);
    EXPECT_DOUBLE_EQ(parsed.units[0].attackPower, 100.0);
    EXPECT_DOUBLE_EQ(parsed.units[0].damageMin, 100.0);
    EXPECT_DOUBLE_EQ(parsed.units[0].damageMax, 100.0);
    EXPECT_DOUBLE_EQ(parsed.units[0].optimalRange, parsed.units[0].attackRange);
}

TEST(AttackPower, InvalidWeaponProfileIsRejectedAtomically) {
    SimulationEngine engine;
    engine.loadDefaultScenario();
    const qsizetype before = engine.unitIds().size();
    ScenarioUnit invalid = engine.scenario().units.front();
    invalid.id = QStringLiteral("invalid_attack");
    invalid.kind = QStringLiteral("attackuav");
    invalid.attackRange = 1000.0;
    invalid.minAttackRange = 900.0;
    invalid.optimalRange = 800.0;
    invalid.ammoCapacity = 2;
    invalid.initialAmmo = 3;
    engine.addOrUpdateUnit(invalid);
    EXPECT_EQ(engine.unitIds().size(), before);
    EXPECT_EQ(engine.unit(QStringLiteral("invalid_attack")), nullptr);
}

TEST(AttackPower, MalformedV2WeaponFieldsRemainInvalidAfterParsing) {
    QJsonObject unit{{QStringLiteral("id"), QStringLiteral("red_bad")},
                     {QStringLiteral("callsign"), QStringLiteral("异常单元")},
                     {QStringLiteral("kind"), QStringLiteral("attackuav")},
                     {QStringLiteral("side"), QStringLiteral("red")},
                     {QStringLiteral("ammoCapacity"), QStringLiteral("four")},
                     {QStringLiteral("hitProbability"), QStringLiteral("always")}};
    const Scenario parsed = ScenarioIo::fromJson(
        QJsonObject{{QStringLiteral("schemaVersion"), 2},
                    {QStringLiteral("units"), QJsonArray{unit}}});
    ASSERT_EQ(parsed.units.size(), 1u);
    EXPECT_LT(parsed.units.front().ammoCapacity, 0);
    EXPECT_FALSE(std::isfinite(parsed.units.front().hitProbability));
}

TEST(AttackPower, AttackUavAppliesConfiguredDamage) {
    SimulationEngine engine;
    engine.loadDefaultScenario();
    auto* atk = dynamic_cast<AttackUAV*>(engine.unit("red_a1"));
    auto* tgt = engine.unit("blue_r1");
    ASSERT_NE(atk, nullptr);
    ASSERT_NE(tgt, nullptr);

    // Set attackPower directly on the unit (simulating QML upsert)
    UnitBase::Params p = atk->params();
    p.attackPower = 250;
    atk->setParams(p);

    double hpBefore = tgt->hp();
    // Move attacker onto target so stepCombat fires
    atk->setPosition(tgt->pos());
    engine.command("engageTarget", QVariantMap{{"attackerId", "red_a1"},
                                                {"targetId", "blue_r1"}});

    // Force one tick so attackRange check + combat fire happens
    engine.stepOnce(2.0);
    EXPECT_LT(tgt->hp(), hpBefore);
}

TEST(AttackPower, AttackUavZeroPowerDoesNoDamage) {
    SimulationEngine engine;
    engine.loadDefaultScenario();
    auto* atk = dynamic_cast<AttackUAV*>(engine.unit("red_a1"));
    auto* tgt = engine.unit("blue_r1");
    ASSERT_NE(atk, nullptr);
    ASSERT_NE(tgt, nullptr);

    UnitBase::Params p = atk->params();
    p.attackPower = 0;
    atk->setParams(p);

    double hpBefore = tgt->hp();
    atk->setPosition(tgt->pos());
    engine.command("engageTarget", QVariantMap{{"attackerId", "red_a1"},
                                                {"targetId", "blue_r1"}});
    engine.stepOnce(2.0);
    EXPECT_DOUBLE_EQ(tgt->hp(), hpBefore);
}

TEST(AttackPower, SustainedFireHonorsCooldownAndAmmo) {
    Scenario scenario = ScenarioIo::defaultScenario();
    for (ScenarioUnit& unit : scenario.units) {
        unit.schedule.clear();
        if (unit.id == QLatin1String("red_a1")) {
            unit.initialAmmo = 3;
            unit.ammoCapacity = 3;
            unit.hitProbability = 1.0;
            unit.cooldownSec = 1.0;
            unit.damageMin = 20.0;
            unit.damageMax = 20.0;
        }
        if (unit.id == QLatin1String("blue_cp")) unit.maxHp = 500.0;
    }
    SimulationEngine engine;
    ASSERT_TRUE(engine.setScenario(scenario));
    auto* attacker = dynamic_cast<AttackUAV*>(engine.unit(QStringLiteral("red_a1")));
    UnitBase* target = engine.unit(QStringLiteral("blue_cp"));
    ASSERT_NE(attacker, nullptr);
    ASSERT_NE(target, nullptr);
    attacker->setPosition(target->pos());
    ASSERT_TRUE(engine.executeCommand(
        QStringLiteral("engageTarget"),
        QVariantMap{{QStringLiteral("attackerId"), QStringLiteral("red_a1")},
                    {QStringLiteral("targetId"), QStringLiteral("blue_cp")}}).accepted);

    engine.stepOnce(0.1);
    EXPECT_EQ(attacker->ammoRemaining(), 2);
    EXPECT_DOUBLE_EQ(target->hp(), 480.0);
    engine.stepOnce(0.5);
    EXPECT_EQ(attacker->ammoRemaining(), 2);
    engine.stepOnce(0.5);
    EXPECT_EQ(attacker->ammoRemaining(), 1);
    EXPECT_DOUBLE_EQ(target->hp(), 460.0);
}

TEST(AttackPower, MissKeepsTargetUntilAmmoIsExhausted) {
    Scenario scenario = ScenarioIo::defaultScenario();
    for (ScenarioUnit& unit : scenario.units) {
        unit.schedule.clear();
        if (unit.id == QLatin1String("red_a1")) {
            unit.initialAmmo = 2;
            unit.ammoCapacity = 2;
            unit.hitProbability = 0.0;
            unit.cooldownSec = 0.1;
        }
    }
    SimulationEngine engine;
    ASSERT_TRUE(engine.setScenario(scenario));
    auto* attacker = dynamic_cast<AttackUAV*>(engine.unit(QStringLiteral("red_a1")));
    UnitBase* target = engine.unit(QStringLiteral("blue_r1"));
    ASSERT_NE(attacker, nullptr);
    ASSERT_NE(target, nullptr);
    attacker->setPosition(target->pos());
    ASSERT_TRUE(engine.executeCommand(
        QStringLiteral("engageTarget"),
        QVariantMap{{QStringLiteral("attackerId"), QStringLiteral("red_a1")},
                    {QStringLiteral("targetId"), QStringLiteral("blue_r1")}}).accepted);

    engine.stepOnce(0.1);
    EXPECT_EQ(attacker->targetId(), QStringLiteral("blue_r1"));
    EXPECT_EQ(attacker->ammoRemaining(), 1);
    engine.stepOnce(0.1);
    EXPECT_TRUE(attacker->targetId().isEmpty());
    EXPECT_EQ(attacker->ammoRemaining(), 0);
    EXPECT_DOUBLE_EQ(target->hp(), target->maxHp());
}
