#include <gtest/gtest.h>
#include "core/SimulationEngine.h"
#include "core/UnitBase.h"
#include "core/Scenario.h"
#include "units/AttackUAV.h"

#include <algorithm>
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
    EXPECT_DOUBLE_EQ(parsed.units[0].damageMin, 80.0);
    EXPECT_DOUBLE_EQ(parsed.units[0].damageMax, 110.0);
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

TEST(AttackPower, MalformedV2WeaponFieldsAreRejectedAtParsingBoundary) {
    QJsonObject unit{{QStringLiteral("id"), QStringLiteral("red_bad")},
                     {QStringLiteral("callsign"), QStringLiteral("异常单元")},
                     {QStringLiteral("kind"), QStringLiteral("attackuav")},
                     {QStringLiteral("side"), QStringLiteral("red")},
                     {QStringLiteral("ammoCapacity"), QStringLiteral("four")},
                     {QStringLiteral("hitProbability"), QStringLiteral("always")}};
    QString error;
    const Scenario parsed = ScenarioIo::fromJson(
        QJsonObject{{QStringLiteral("schemaVersion"), 2},
                    {QStringLiteral("units"), QJsonArray{unit}}}, &error);
    EXPECT_TRUE(parsed.units.empty());
    EXPECT_FALSE(error.isEmpty());
}

TEST(AttackPower, AttackUavAppliesConfiguredDamage) {
    Scenario scenario = ScenarioIo::defaultScenario();
    for (ScenarioUnit& unit : scenario.units) {
        unit.schedule.clear();
        if (unit.id == QLatin1String("red_a1")) {
            unit.hitProbability = 1.0;
            unit.minAttackRange = 0.0;
            unit.damageMin = 250.0;
            unit.damageMax = 250.0;
        }
    }
    SimulationEngine engine;
    ASSERT_TRUE(engine.setScenario(scenario));
    auto* atk = dynamic_cast<AttackUAV*>(engine.unit("red_a1"));
    auto* tgt = engine.unit("blue_r1");
    ASSERT_NE(atk, nullptr);
    ASSERT_NE(tgt, nullptr);

    double hpBefore = tgt->hp();
    atk->setPosition(tgt->pos());
    atk->fireOnTarget(QStringLiteral("blue_r1"));

    engine.stepOnce(2.0);
    EXPECT_LT(tgt->hp(), hpBefore);
}

TEST(AttackPower, SimultaneousLethalHitsDoNotOverApplySubsystemDamage) {
    Scenario scenario = ScenarioIo::defaultScenario();
    ScenarioUnit secondAttacker;
    for (ScenarioUnit& unit : scenario.units) {
        unit.schedule.clear();
        if (unit.id == QLatin1String("red_a1")) {
            unit.pos = GeoPos{5000.0, 5000.0, 1000.0};
            unit.hitProbability = 1.0;
            unit.damageMin = 100.0;
            unit.damageMax = 100.0;
            unit.cooldownSec = 0.0;
            unit.attackRange = 2000.0;
            unit.optimalRange = 0.0;
            unit.minAttackRange = 0.0;
            secondAttacker = unit;
            secondAttacker.id = QStringLiteral("red_a5");
            secondAttacker.callsign = QStringLiteral("Second attacker");
        } else if (unit.id == QLatin1String("blue_r1")) {
            unit.pos = GeoPos{5000.0, 5000.0, 0.0};
            unit.maxHp = 100.0;
            unit.armor = 0.0;
        }
    }
    scenario.units.push_back(secondAttacker);
    SimulationEngine engine;
    ASSERT_TRUE(engine.setScenario(scenario));
    auto* first = dynamic_cast<AttackUAV*>(engine.unit(QStringLiteral("red_a1")));
    auto* second = dynamic_cast<AttackUAV*>(engine.unit(QStringLiteral("red_a5")));
    UnitBase* target = engine.unit(QStringLiteral("blue_r1"));
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    ASSERT_NE(target, nullptr);
    first->fireOnTarget(target->id());
    second->fireOnTarget(target->id());

    engine.stepOnce(0.1);

    EXPECT_DOUBLE_EQ(target->hp(), 0.0);
    EXPECT_NEAR(target->subsystemStateJson().value(QStringLiteral("sensor")).toDouble(),
                0.3, 1e-9);
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
    atk->clearSchedule();
    tgt->clearSchedule();

    double hpBefore = tgt->hp();
    atk->setPosition(tgt->pos());
    atk->fireOnTarget(QStringLiteral("blue_r1"));
    engine.stepOnce(2.0);
    EXPECT_DOUBLE_EQ(tgt->hp(), hpBefore);
}

TEST(AttackPower, ReloadCompletesWithoutAutomaticFollowUpShot) {
    Scenario scenario = ScenarioIo::defaultScenario();
    for (ScenarioUnit& unit : scenario.units) {
        unit.schedule.clear();
        if (unit.id == QLatin1String("red_a1")) {
            unit.initialAmmo = 3;
            unit.ammoCapacity = 3;
            unit.hitProbability = 1.0;
            unit.cooldownSec = 4.0;
            unit.damageMin = 20.0;
            unit.damageMax = 20.0;
            unit.minAttackRange = 0.0;
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
    attacker->fireOnTarget(QStringLiteral("blue_cp"));

    engine.stepOnce(0.1);
    EXPECT_EQ(attacker->ammoRemaining(), 2);
    EXPECT_DOUBLE_EQ(target->hp(), 480.0);
    EXPECT_FALSE(attacker->armed());
    EXPECT_GT(attacker->cooldownRemaining(), 3.8);
    engine.stepOnce(2.0);
    EXPECT_EQ(attacker->ammoRemaining(), 2);
    EXPECT_DOUBLE_EQ(target->hp(), 480.0);
    engine.stepOnce(2.0);
    EXPECT_DOUBLE_EQ(attacker->cooldownRemaining(), 0.0);
    EXPECT_EQ(attacker->ammoRemaining(), 2);
    EXPECT_DOUBLE_EQ(target->hp(), 480.0);

    attacker->fireOnTarget(QStringLiteral("blue_cp"));
    engine.stepOnce(0.1);
    EXPECT_EQ(attacker->ammoRemaining(), 1);
    EXPECT_DOUBLE_EQ(target->hp(), 460.0);
}

TEST(AttackPower, GeometryContactCanMissAccordingToHitProbability) {
    Scenario scenario = ScenarioIo::defaultScenario();
    for (ScenarioUnit& unit : scenario.units) {
        unit.schedule.clear();
        if (unit.id == QLatin1String("red_a1")) {
            unit.initialAmmo = 2;
            unit.ammoCapacity = 2;
            unit.hitProbability = 0.0;
            unit.cooldownSec = 0.1;
            unit.minAttackRange = 0.0;
        }
    }
    SimulationEngine engine;
    ASSERT_TRUE(engine.setScenario(scenario));
    auto* attacker = dynamic_cast<AttackUAV*>(engine.unit(QStringLiteral("red_a1")));
    UnitBase* target = engine.unit(QStringLiteral("blue_r1"));
    ASSERT_NE(attacker, nullptr);
    ASSERT_NE(target, nullptr);
    attacker->setPosition(target->pos());
    attacker->fireOnTarget(QStringLiteral("blue_r1"));

    engine.stepOnce(0.1);
    EXPECT_EQ(attacker->ammoRemaining(), 1);
    EXPECT_TRUE(target->alive());
    EXPECT_EQ(attacker->lastShotOutcome(), QStringLiteral("miss"));
}

TEST(AttackPower, MinimumRangeClearsAndHoldsUntilExplicitReassignment) {
    Scenario scenario = ScenarioIo::defaultScenario();
    for (ScenarioUnit& unit : scenario.units) {
        unit.schedule.clear();
        if (unit.id == QLatin1String("red_a1")) {
            unit.pos = GeoPos{5000.0, 5000.0, unit.pos.alt};
            unit.minAttackRange = 1000.0;
            unit.attackRange = 2500.0;
            unit.initialAmmo = 3;
            unit.ammoCapacity = 3;
            unit.hitProbability = 1.0;
            unit.cooldownSec = 0.0;
            unit.damageMin = 10.0;
            unit.damageMax = 10.0;
        }
        if (unit.id == QLatin1String("blue_r1")) {
            unit.pos = GeoPos{5500.0, 5000.0, unit.pos.alt};
        }
    }

    SimulationEngine engine;
    ASSERT_TRUE(engine.setScenario(scenario));
    auto* attacker = dynamic_cast<AttackUAV*>(engine.unit(QStringLiteral("red_a1")));
    auto* target = engine.unit(QStringLiteral("blue_r1"));
    ASSERT_NE(attacker, nullptr);
    ASSERT_NE(target, nullptr);

    QVariantList waypoints{QVariantMap{{QStringLiteral("x"), 6000.0},
                                       {QStringLiteral("y"), 5000.0}}};
    ASSERT_TRUE(engine.executeCommand(
        QStringLiteral("setFlightPlan"),
        QVariantMap{{QStringLiteral("attackerId"), QStringLiteral("red_a1")},
                    {QStringLiteral("waypoints"), waypoints}}).accepted);
    ASSERT_TRUE(engine.executeCommand(
        QStringLiteral("engageTarget"),
        QVariantMap{{QStringLiteral("attackerId"), QStringLiteral("red_a1")},
                    {QStringLiteral("targetId"), QStringLiteral("blue_r1")}}).accepted);

    const GeoPos before = attacker->pos();
    const int ammoBefore = attacker->ammoRemaining();
    int targetChanges = 0;
    int armedChanges = 0;
    int weaponChanges = 0;
    QObject::connect(attacker, &AttackUAV::targetChanged, [&]{ ++targetChanges; });
    QObject::connect(attacker, &AttackUAV::armedChanged, [&]{ ++armedChanges; });
    QObject::connect(attacker, &AttackUAV::weaponStateChanged, [&]{ ++weaponChanges; });
    engine.stepOnce(0.05);

    EXPECT_TRUE(attacker->targetId().isEmpty());
    EXPECT_FALSE(attacker->armed());
    EXPECT_EQ(attacker->ammoRemaining(), ammoBefore);
    EXPECT_FALSE(attacker->takePendingShot().has_value());
    EXPECT_DOUBLE_EQ(attacker->pos().x, before.x);
    EXPECT_DOUBLE_EQ(attacker->pos().y, before.y);
    EXPECT_FALSE(attacker->hasActiveWaypoints());
    EXPECT_TRUE(attacker->statusText().contains(QStringLiteral("距离过近")));
    EXPECT_EQ(targetChanges, 1);
    EXPECT_EQ(armedChanges, 1);
    EXPECT_EQ(weaponChanges, 1);
    const QJsonArray heldWaypoints = attacker->checkpointState()
        .value(QStringLiteral("behavior")).toObject()
        .value(QStringLiteral("waypoints")).toArray();
    ASSERT_EQ(heldWaypoints.size(), 1);
    EXPECT_DOUBLE_EQ(heldWaypoints.at(0).toObject().value(QStringLiteral("x")).toDouble(), 6000.0);

    for (int i = 0; i < 5; ++i) engine.stepOnce(0.05);
    EXPECT_EQ(attacker->ammoRemaining(), ammoBefore);
    EXPECT_EQ(targetChanges, 1);
    EXPECT_EQ(armedChanges, 1);
    EXPECT_EQ(weaponChanges, 1);

    target->setPosition(GeoPos{6500.0, 5000.0, target->pos().alt});
    ASSERT_TRUE(engine.executeCommand(
        QStringLiteral("engageTarget"),
        QVariantMap{{QStringLiteral("attackerId"), QStringLiteral("red_a1")},
                    {QStringLiteral("targetId"), QStringLiteral("blue_r1")}}).accepted);
    engine.stepOnce(0.05);
    EXPECT_EQ(attacker->ammoRemaining(), ammoBefore - 1);
    for (int i = 0; i < 100 && attacker->hasActiveProjectile(); ++i) {
        engine.stepOnce(0.05);
    }
    EXPECT_DOUBLE_EQ(target->hp(), target->maxHp() - 10.0);
}

TEST(AttackPower, TargetJustOutsideWeaponRangeTriggersReapproach) {
    Scenario scenario = ScenarioIo::defaultScenario();
    for (ScenarioUnit& unit : scenario.units) {
        unit.schedule.clear();
        if (unit.id == QLatin1String("red_a1")) {
            unit.pos = GeoPos{1000.0, 1000.0, 1000.0};
            unit.attackRange = 2000.0;
            unit.commRange = 20000.0;
        } else if (unit.id == QLatin1String("blue_r1")) {
            unit.pos = GeoPos{2500.0, 1000.0, 1000.0};
        }
    }
    SimulationEngine engine;
    ASSERT_TRUE(engine.setScenario(scenario));
    auto* attacker = dynamic_cast<AttackUAV*>(engine.unit(QStringLiteral("red_a1")));
    UnitBase* target = engine.unit(QStringLiteral("blue_r1"));
    ASSERT_NE(attacker, nullptr);
    ASSERT_NE(target, nullptr);
    ASSERT_TRUE(engine.executeCommand(
                    QStringLiteral("setRoe"),
                    QVariantMap{{QStringLiteral("unitId"), QStringLiteral("red_a1")},
                                {QStringLiteral("roe"), QStringLiteral("hold")}}).accepted);
    ASSERT_TRUE(engine.executeCommand(
                    QStringLiteral("pursue"),
                    QVariantMap{{QStringLiteral("attackerId"), QStringLiteral("red_a1")},
                                {QStringLiteral("targetId"), QStringLiteral("blue_r1")}}).accepted);
    engine.stepOnce(0.05);
    target->setPosition(GeoPos{3200.0, 1000.0, 1000.0});
    const double before = attacker->pos().x;

    engine.stepOnce(0.05);

    EXPECT_TRUE(attacker->hasActiveWaypoints());
    engine.stepOnce(0.05);
    EXPECT_GT(attacker->pos().x, before);
}

TEST(AttackPower, WeaponReconfigurationPreservesShotSequence) {
    Scenario scenario = ScenarioIo::defaultScenario();
    for (ScenarioUnit& unit : scenario.units) {
        unit.schedule.clear();
        if (unit.id == QLatin1String("red_a1")) {
            unit.commRange = 20000.0;
            unit.cooldownSec = 0.0;
            unit.hitProbability = 1.0;
            unit.minAttackRange = 0.0;
        }
    }
    SimulationEngine engine;
    ASSERT_TRUE(engine.setScenario(scenario));
    auto* attacker = dynamic_cast<AttackUAV*>(engine.unit(QStringLiteral("red_a1")));
    UnitBase* target = engine.unit(QStringLiteral("blue_r1"));
    ASSERT_NE(attacker, nullptr);
    ASSERT_NE(target, nullptr);
    attacker->setPosition(target->pos());
    attacker->fireOnTarget(QStringLiteral("blue_r1"));
    engine.stepOnce(0.05);
    ASSERT_GT(attacker->shotSequence(), 0U);
    const quint64 sequence = attacker->shotSequence();
    const ScenarioUnit configured = *std::find_if(
        scenario.units.cbegin(), scenario.units.cend(), [](const ScenarioUnit& unit) {
            return unit.id == QLatin1String("red_a1");
        });

    attacker->configureWeapon(configured);

    EXPECT_EQ(attacker->shotSequence(), sequence);
}
